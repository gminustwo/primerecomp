#!/usr/bin/env python3
"""Log triage for GitHub issues: a short summary of the primerecomp.log files attached to an issue.

.github/workflows/log_triage.yml runs it when an issue is opened / edited or a comment is added. It reads the issue
and its comments through the GitHub API, downloads the attached logs (github.com/user-attachments/files/... links
whose file name starts with "primerecomp", at most MAX_BYTES each), and keeps ONE bot comment per issue up to date
with a summary of the newest logs, plus a "log: crash" / "log: device-lost" / "log: ok" label for the newest one.

Everything in an issue and in a log is untrusted data: it is only ever parsed here, never run or handed to a shell.
Excerpts quoted in the comment are cut short, put in code spans, and have file paths reduced to their last part.

usage:
  log_triage.py parse <log>...      print the comment for local log files (no network)
  log_triage.py issue               the workflow: env GITHUB_TOKEN, GITHUB_REPOSITORY, ISSUE_NUMBER,
                                    DRY_RUN=1 prints the comment instead of posting it and leaves labels alone
Standard library only.
"""
import json
import os
import re
import statistics
import sys
import urllib.error
import urllib.parse
import urllib.request

MARKER = "<!-- primerecomp-log-triage -->"
MAX_BYTES = 20 * 1024 * 1024  # per log
MAX_LOGS = 3                  # newest logs summarised per comment
LABELS = {"crash": ("log: crash", "b60205", "Newest attached log ends in a crash or hang"),
          "device-lost": ("log: device-lost", "d93f0b", "Newest attached log ends with the GPU device lost"),
          "ok": ("log: ok", "0e8a16", "Newest attached log ends with a clean exit")}
ATTACH_RE = re.compile(r"https://github\.com/user-attachments/files/\d+/[A-Za-z0-9._%+-]+")
REDIRECT_HOSTS = ("objects.githubusercontent.com",)  # (where github.com/user-attachments/files/... redirects)


# ------------------------------------------------------------------ parsing a log

def clean(s, n=160):
    """A log excerpt made safe to quote: paths cut to their last part, no backticks / control characters, short."""
    s = re.sub(r"[A-Za-z]:[\\/][^\s:;,)\"']*[\\/]([^\s\\/:;,)\"']+)", r"\1", s)        # C:\Users\x\...\file
    s = re.sub(r"(?<![\w.])/(?:[^\s/:;,)\"']+/)+([^\s/:;,)\"']+)", r"\1", s)           # /home/x/.../file
    s = "".join(c if c.isprintable() else " " for c in s).replace("`", "'").strip()
    return s if len(s) <= n else s[:n - 3] + "..."


def code(s, n=160):
    return "`" + clean(s, n) + "`"


FPS_RE = re.compile(r"^\[(vk)\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms level (\S+)")
BUILD_RE = re.compile(r"Prime[ ._-]?Recomp[ ._-]((?:v\d+\.\d+\.\d+)|(?:test-[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*?))"
                      r"(?:-(windows|macos|linux|android)\b|[\\/\s]|$)", re.I)
MS_RE = re.compile(r"([\d.]+)\s*ms")

# notable lines: (label, regex); each is reported once, with its count and first line
NOTABLE = [
    ("heap as driver memory failed", re.compile(r"heap as driver memory: (?!.*shared as before$).*(fail|not allowed|no way|can't|no cached|would be)")),
    ("heap as driver memory fell back", re.compile(r"heap as driver memory: .*shared as before")),
    ("guest heap imported in chunks (not enough GPU memory for one import)", re.compile(r"\[vk\] guest heap: .*(ran out of memory|imported in chunks)")),
    ("no BC texture formats", re.compile(r"no BC texture formats")),
    ("GPU device fault", re.compile(r"\[vk\] device fault")),
    ("shaders not found or failed in preparation", re.compile(r"\[shader\] prepared \d+, not found [1-9]|\[shader\] prepared .*\(([1-9]\d*) failed")),
    ("shader translation failed", re.compile(r"translate failed|pipeline (creation )?failed|\[shader\] stale program")),
    ("MoltenVK error (macOS)", re.compile(r"\[mvk-error\]")),
    ("frame pacing switched to the game's 30 fps mode", re.compile(r"\[pace\] frames don't fit")),
    ("slow shader builds (stutter)", re.compile(r"\[shader\] slow build")),
    ("settings file could not be written", re.compile(r"\[settings\] cannot write")),
    ("developer overrides (LARECOMP_*) in use", re.compile(r"\(LARECOMP_[A-Z_]+\)")),
]


def parse(text):
    lines = text.splitlines()
    r = {"backend": None, "gpu": None, "gpus": [], "api": None, "driver": None, "os": None, "cpu": None,
         "build": None, "settings": None, "swapchain": None, "integrated": False, "fps": [], "slow": [],
         "notable": {}, "end": None, "end_line": None, "lines": len(lines)}
    fatal = exc = crash_head = None
    devlost = hang = closed = False
    for i, ln in enumerate(lines):
        if ln.startswith("[paths] data: "):
            p = ln[14:]
            r["os"] = ("Windows" if re.match(r"[A-Za-z]:\\", p) else "Android" if p.startswith("/storage/")
                       else "macOS" if p.startswith("/Users/") or "Library/Application Support" in p
                       else "Linux" if p.startswith("/") else None)
            m = BUILD_RE.search(p)
            if m: r["build"] = m.group(1) + (f" ({m.group(2).lower()})" if m.group(2) else "")
        elif ln.startswith("ANDROID:"):
            r["os"] = "Android"
        elif not r["build"] and re.match(r"^\[(version|build)\] ", ln):
            r["build"] = ln.split("] ", 1)[1]
        elif m := re.match(r"^\[vk\] gpu \d+: (.+) \(api ([\d.]+)\)", ln):
            r["gpus"].append(m.group(1))
            r["backend"] = "Vulkan"
        elif m := re.match(r"^\[vk\] using (.+)", ln):
            r["gpu"] = m.group(1)
            r["backend"] = "Vulkan"
            for g in lines[:i]:
                mm = re.match(r"^\[vk\] gpu \d+: (.+) \(api ([\d.]+)\)", g)
                if mm and mm.group(1) == r["gpu"]: r["api"] = mm.group(2)
        elif m := re.match(r"^\[vk\] device: driver (\S+) \(([^)]*)\)", ln):
            r["driver"] = f"{m.group(2)} {m.group(1)}"
        elif ln.startswith("[settings] ") and " GPU: upscaler " in ln:
            r["settings"] = ln.split(" GPU: ", 1)[1]
            r["integrated"] = ln.startswith("[settings] integrated")
        elif m := re.match(r"^\[vk\] swapchain (\d+x\d+), (\d+) images, (\w+)", ln):
            r["swapchain"] = f"{m.group(1)}, {m.group(3)}"
        elif m := re.match(r"^\[pcore\] CPU (.+?) \((\d+) logical processors", ln):
            r["cpu"] = f"{m.group(2)} logical processors" + (", hybrid" if m.group(1).startswith("hybrid") else "")
        elif m := re.match(r"^\[cpu\] fastest core \S+ \((\d+) MHz\)", ln):
            r["cpu"] = f"fastest core {m.group(1)} MHz"
        elif m := FPS_RE.match(ln):
            r["fps"].append((float(m.group(2)), float(m.group(4)), int(m.group(5)), m.group(6)))
        elif ln.startswith("[slow]"):
            r["slow"].append(ln)
        elif ln.startswith("FATAL: ") and not fatal:
            fatal = ln[7:]
            if "device lost" in ln or re.search(r"failed: -4 \(", ln): devlost = True
        elif ln.startswith("*** ") and not exc:
            exc = ln[4:]
        elif ln.startswith("[crash] "):
            crash_head = ln[8:]
        elif ln.startswith("[vk] last GPU operations before the device loss") or "VK_ERROR_DEVICE_LOST" in ln:
            devlost = True
        elif ln.startswith("[hang] ====="):
            hang = True
        elif ln.strip() == "window closed":
            closed = True
        for label, rx in NOTABLE:
            if rx.search(ln):
                n, first = r["notable"].get(label, (0, ln))
                r["notable"][label] = (n + 1, first)
    if r["integrated"] or any("integrated GPU" in x for x in lines[:200]):
        r["integrated"] = True
    if devlost:
        r["end"], r["end_line"] = "device-lost", fatal or "the GPU device was lost"
    elif fatal or exc or crash_head:
        r["end"], r["end_line"] = "crash", fatal or exc or crash_head
        r["crash_head"] = crash_head
    elif hang:
        r["end"], r["end_line"] = "hang", next((x for x in lines if x.startswith("[hang] =====") and x.strip() != "[hang] ====="), None)
    elif closed:
        r["end"] = "ok"
    else:
        r["end"] = "unknown"
    return r


def fps_summary(fps):
    if not fps: return None
    vals = [f for _, f, _, _ in fps]
    span = fps[-1][0] - fps[0][0]
    worst_ms = max(m for _, _, m, _ in fps)
    by_level = {}
    for _, f, _, lv in fps: by_level.setdefault(lv, []).append(f)
    lows = sorted(((statistics.median(v), lv) for lv, v in by_level.items() if len(v) >= 2 and lv != "-"))[:2]
    s = (f"{len(fps)} samples over {span:.0f} s: median {statistics.median(vals):.1f} fps, lowest {min(vals):.1f} fps, "
         f"longest frame {worst_ms} ms")
    if len(by_level) > 1 and lows:
        s += "; slowest areas: " + ", ".join(f"{code(lv, 40)} {f:.1f} fps" for f, lv in lows)
    return s


def summarise(r):
    out = []
    out.append(f"- **Build:** {code(r['build']) if r['build'] else 'not in the log'}")
    sysl = [r["os"] or "OS not in the log"]
    if r["cpu"]: sysl.append(r["cpu"])
    out.append("- **System:** " + ", ".join(sysl))
    if r["gpu"]:
        g = code(r["gpu"], 80) + (" (integrated)" if r["integrated"] else "")
        if r["api"]: g += f", Vulkan {r['api']}"
        if r["driver"]: g += f", driver {code(r['driver'], 60)}"
        if len(r["gpus"]) > 1: g += f" ({len(r['gpus'])} GPUs found)"
        out.append(f"- **GPU:** {g}")
    else:
        out.append("- **GPU:** not in the log")
    out.append(f"- **Backend:** {r['backend'] or 'not in the log'}")
    st = []
    if r["settings"]: st.append(code(r["settings"], 120))
    if r["swapchain"]: st.append(f"window {r['swapchain']}")
    out.append("- **Graphics settings:** " + ("; ".join(st) if st else "not in the log"))
    end = {"ok": "clean exit (window closed)",
           "crash": "crash" + (f" ({clean(r.get('crash_head'), 80)})" if r.get("crash_head") else "") + f": {code(r['end_line'] or '')}",
           "device-lost": f"the GPU device was lost: {code(r['end_line'] or '')}",
           "hang": f"hang (the hang report is in the log){': ' + code(r['end_line']) if r['end_line'] else ''}",
           "unknown": "the log stops without an exit line (closed from outside, frozen, or still running when copied)"}[r["end"]]
    out.append(f"- **How the run ended:** {end}")
    fs = fps_summary(r["fps"])
    out.append(f"- **Frame rate:** {fs or 'no frame rate lines'}")
    if r["slow"]:
        def ms(x):
            m = MS_RE.search(x)
            return float(m.group(1)) if m else 0.0
        worst = max(r["slow"], key=ms)
        out.append(f"- **Slow frames:** {len(r['slow'])} `[slow]` lines; worst: {code(worst, 140)}")
    if r["notable"]:
        out.append("- **Notable lines:**")
        for label, (n, first) in r["notable"].items():
            out.append(f"  - {label}{f' ({n}x)' if n > 1 else ''}: {code(first, 140)}")
    return "\n".join(out)


def render(entries, total):
    """entries: newest first, (source description, parse result or error string)"""
    out = [MARKER,
           "This is an automatic summary of the PrimeRecomp log files attached to this issue. It only reads the logs, "
           "so a person will still look at the details.",
           "",
           "For a new report, please attach `primerecomp.log` from PrimeRecomp's data folder (it always holds the latest "
           "run; `primerecomp-backup-*.log` there are the earlier runs)."]
    for k, (src, r) in enumerate(entries):
        out += ["", f"#### {'Newest log' if k == 0 else 'Earlier log'}: {src}"]
        out.append(summarise(r) if isinstance(r, dict) else f"- Could not read this log: {clean(r, 120)}")
    if total > len(entries):
        out += ["", f"({total - len(entries)} earlier log{'s' if total - len(entries) > 1 else ''} on this issue not summarised.)"]
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------ GitHub

class Api:
    def __init__(self, token, repo):
        self.token, self.repo = token, repo

    def call(self, method, path, body=None):
        url = path if path.startswith("https://") else f"https://api.github.com/repos/{self.repo}/{path}"
        req = urllib.request.Request(url, method=method, data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Authorization": f"Bearer {self.token}", "Accept": "application/vnd.github+json",
                                              "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "primerecomp-log-triage"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = resp.read()
            link = resp.headers.get("Link", "")
        return (json.loads(data) if data else None), link

    def pages(self, path):
        out, url = [], f"https://api.github.com/repos/{self.repo}/{path}"
        for _ in range(50):
            data, link = self.call("GET", url)
            out += data
            m = re.search(r'<([^>]+)>;\s*rel="next"', link)
            if not m: break
            url = m.group(1)
        return out


class _Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code_, msg, headers, newurl):
        host = urllib.parse.urlparse(newurl).hostname or ""
        if urllib.parse.urlparse(newurl).scheme != "https" or host not in REDIRECT_HOSTS:
            raise urllib.error.HTTPError(newurl, code_, f"redirect to {host} not allowed", headers, fp)
        return super().redirect_request(req, fp, code_, msg, headers, newurl)


def download(url):
    if not ATTACH_RE.fullmatch(url): raise ValueError("not a GitHub attachment link")
    opener = urllib.request.build_opener(_Redirects)
    req = urllib.request.Request(url, headers={"User-Agent": "primerecomp-log-triage"})  # (no token: public files)
    with opener.open(req, timeout=60) as resp:
        if int(resp.headers.get("Content-Length") or 0) > MAX_BYTES: raise ValueError("log larger than 20 MB")
        data = resp.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES: raise ValueError("log larger than 20 MB")
    return data.decode("utf-8", "replace")


def attachments(text):
    """primerecomp*.log / *.txt attachment links in a body, in order, without repeats"""
    seen, out = set(), []
    for u in ATTACH_RE.findall(text or ""):
        name = urllib.parse.unquote(u.rsplit("/", 1)[1]).lower()
        if name.startswith("primerecomp") and name.endswith((".log", ".txt")) and u not in seen:
            seen.add(u)
            out.append(u)
    return out


def run_issue():
    token, repo = os.environ["GITHUB_TOKEN"], os.environ["GITHUB_REPOSITORY"]
    num = os.environ.get("ISSUE_NUMBER", "")
    if not re.fullmatch(r"[1-9]\d{0,6}", num): sys.exit(f"bad issue number {num!r}")
    dry = os.environ.get("DRY_RUN", "") in ("1", "true")
    api = Api(token, repo)
    issue, _ = api.call("GET", f"issues/{num}")
    if issue.get("pull_request"): print("a pull request: skipped"); return
    comments = api.pages(f"issues/{num}/comments?per_page=100")
    found = [(u, "the issue text", issue["html_url"]) for u in attachments(issue.get("body"))]
    mine = None
    for c in comments:
        if (c.get("user") or {}).get("type") == "Bot":
            if MARKER in (c.get("body") or "") and c["user"]["login"] == "github-actions[bot]": mine = c
            continue
        found += [(u, f"comment from {c['created_at'][:10]}", c["html_url"]) for u in attachments(c.get("body"))]
    if not found: print("no primerecomp log attached: nothing to do"); return
    newest = list(reversed(found))[:MAX_LOGS]
    entries = []
    for u, where, link in newest:
        name = urllib.parse.unquote(u.rsplit("/", 1)[1])
        src = f"[{clean(name, 60)}]({u}) in [{where}]({link})"
        try:
            entries.append((src, parse(download(u))))
        except Exception as e:  # (a bad download is reported in the comment, never fatal)
            entries.append((src, f"{type(e).__name__}: {e}"))
    body = render(entries, len(found))
    first = entries[0][1]
    label = None
    if isinstance(first, dict):
        label = LABELS["crash"][0] if first["end"] in ("crash", "hang") else LABELS["device-lost"][0] if first["end"] == "device-lost" \
            else LABELS["ok"][0] if first["end"] == "ok" else None
    print(body)
    print(f"label: {label or '(none)'}; existing bot comment: {mine['html_url'] if mine else '(none)'}")
    if dry:
        print("dry run: nothing posted")
        summary = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary:
            with open(summary, "a", encoding="utf-8") as f:
                f.write(f"Dry run for issue #{num} (label: {label or 'none'}):\n\n{body}\n")
        return
    if mine:
        if mine["body"] != body: api.call("PATCH", f"issues/comments/{mine['id']}", {"body": body})
    else:
        api.call("POST", f"issues/{num}/comments", {"body": body})
    have = {l["name"] for l in issue.get("labels", [])}
    for name, color, desc in LABELS.values():
        if name in have and name != label:
            api.call("DELETE", f"issues/{num}/labels/{urllib.parse.quote(name)}")
    if label and label not in have:
        try:
            name, color, desc = next(v for v in LABELS.values() if v[0] == label)
            api.call("POST", "labels", {"name": name, "color": color, "description": desc})
        except urllib.error.HTTPError as e:
            if e.code != 422: raise  # (422: it exists already)
        api.call("POST", f"issues/{num}/labels", {"labels": [label]})


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "parse":
        entries = []
        for p in reversed(sys.argv[2:]):
            with open(p, encoding="utf-8", errors="replace") as f:
                entries.append((os.path.basename(p), parse(f.read())))
        sys.stdout.write(render(entries[:MAX_LOGS], len(entries)))
        print("label:", {"crash": "log: crash", "hang": "log: crash", "device-lost": "log: device-lost", "ok": "log: ok"}
              .get(entries[0][1]["end"], "(none)"))
    elif len(sys.argv) == 2 and sys.argv[1] == "issue":
        run_issue()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
