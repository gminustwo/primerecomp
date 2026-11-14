#!/usr/bin/env python3
"""Manifest contracts and optional runtime smoke tests; never require game content."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "recomplauncher.json"
SCHEMA = EXE = PARSER = None


def load_manifest():
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


class ManifestTests(unittest.TestCase):
    def test_one_file_starts_windows_and_linux(self):
        m = load_manifest()
        self.assertEqual(m["repo"], "gminustwo/primerecomp")
        # The exe is tried first; the AppImage name is rebound by the launcher to the downloaded file.
        self.assertEqual(m["start"]["program"], ["primerecomp.exe", "PrimeRecomp.AppImage"])
        self.assertEqual(m["prepare"]["program"], m["start"]["program"])
        self.assertGreaterEqual(m["prepare"]["timeout"], 1800)

    def test_matches_the_schema(self):
        if not SCHEMA: self.skipTest("pass --schema to validate against the published schema")
        import jsonschema
        jsonschema.Draft7Validator(json.loads(SCHEMA.read_text())).validate(load_manifest())

    def test_only_an_extracted_folder_no_keys(self):
        m = load_manifest()
        self.assertEqual(len(m["inputs"]), 1)
        self.assertEqual(m["inputs"][0]["formats"], [{"type": "folder"}])
        self.assertFalse(any(i.get("role") == "key" for i in m["inputs"]))
        text = json.dumps(m).lower()
        for word in ("prod.keys", "title.keys", "prod-keys", "--keys"):
            self.assertNotIn(word, text)

    def test_data_roots_match_the_game(self):
        # runtime/paths.c: next to the exe on Windows, ~/.local/share/primerecomp on Linux. No --data-root is passed,
        # so the game picks the same folder the launcher reads.
        m = load_manifest()
        self.assertFalse(any("--data-root" in a for a in m["start"]["args"] + m["prepare"]["args"]))
        self.assertEqual(m["saves"], ["{install}/save", "{local}/primerecomp/save"])
        files = m["config"][0]["file"]
        self.assertEqual(files[0], {"path": "{install}/primerecomp.ini", "if": "{install}/primerecomp.exe"})
        self.assertEqual(files[-1], {"path": "{local}/primerecomp/primerecomp.ini"})
        for name in ("saves", "logs", "data"):
            places = m["folders"][name]
            self.assertEqual(places[0]["if"], "{install}/primerecomp.exe")
            self.assertNotIn("if", places[-1])

    def test_options_match_real_ini_keys(self):
        text = (ROOT / "runtime/settings.c").read_text()
        runtime_keys = set(re.findall(r'put\(o, "([a-z_][a-z_0-9]*) = ', text))
        # Some are written in the same format string after a newline.
        runtime_keys.update(re.findall(r'\\n([a-z_][a-z_0-9]*) = ', text))
        m = load_manifest()
        ids = [s["id"] for s in m["settings"]]
        self.assertEqual(len(ids), len(set(ids)))
        groups = {g["id"] for g in m["groups"]}
        for s in m["settings"]:
            if s.get("in") == "args": continue
            self.assertIn(s["id"], runtime_keys)
            if "slider" in s:
                self.assertLess(s["slider"]["min"], s["slider"]["max"])
                self.assertGreater(s["slider"]["step"], 0)
        self.assertTrue(groups <= {s.get("group") for s in m["settings"]})


@unittest.skipUnless(EXE, "pass --exe to run native headless smoke tests")
class RuntimeTests(unittest.TestCase):
    def run_cli(self, *args):
        with tempfile.TemporaryDirectory(prefix="prime setup α ") as d:
            root = Path(d)
            data = root / "data"
            data.mkdir()
            save = data / "save/save_0"
            save.mkdir(parents=True)
            sentinel = save / "keep.bin"
            sentinel.write_bytes(b"existing synthetic save")
            env = os.environ.copy()
            env.update(LARECOMP_DATA_ROOT=str(data), LARECOMP_SAVE_DIR=str(data / "save"),
                       APPIMAGE_EXTRACT_AND_RUN="1", SDL_VIDEODRIVER="invalid-driver", SDL_AUDIODRIVER="invalid-driver")
            for key in ("DISPLAY", "WAYLAND_DISPLAY", "LARECOMP_LAUNCHER_SETUP", "LARECOMP_LAUNCHER_EXTRACT"):
                env.pop(key, None)
            source = root / "game files β"
            source.mkdir()
            if "package-only" in args:  # a game package, which isn't read: refused as unusable
                (source / "synthetic.nsp").write_bytes(bytes(512))
                args = ()
            if "wrong-version" in args:
                (source / "exefs").mkdir()
                (source / "romfs/Worlds").mkdir(parents=True)
                (source / "exefs/main").write_bytes(bytes(128))
                (source / "exefs/main.npdm").write_bytes(b"fixture")
                args = ()
            r = subprocess.run([str(EXE), "--prepare", "--game=" + str(source), *args], cwd=root,
                               env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(sentinel.read_bytes(), b"existing synthetic save")
            self.assertNotIn("video", r.stderr.lower())  # setup never attempts a window
            progress = [line for line in r.stdout.splitlines() if line.startswith("progress ")]
            self.assertTrue(progress, r.stderr)
            values = [float(line.split(" ", 2)[1]) for line in progress]
            self.assertEqual(values, sorted(values))
            self.assertTrue(all(0 <= x < 1 for x in values))
            log = data / "primerecomp-setup.log"
            self.assertTrue(log.is_file())
            self.assertFalse((data / ".primerecomp-setup-source").exists())
            return log.read_text()

    def test_empty_folder_fails_without_window_or_save_changes(self):
        self.assertIn("No game files", self.run_cli())

    def test_wrong_game_version_fails_without_window(self):
        self.assertIn("Wrong version", self.run_cli("wrong-version"))

    def test_folder_of_packages_is_refused(self):
        self.assertIn("can't be used", self.run_cli("package-only"))

    def test_package_file_and_key_options_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="prime package ") as d:
            package = Path(d) / "synthetic.nsp"
            package.write_bytes(bytes(512))
            for args in (["--prepare", "--game=" + str(package)], ["--prepare", "--game=" + d, "--keys=" + str(package)]):
                r = subprocess.run([str(EXE), *args], capture_output=True, text=True, timeout=15)
                self.assertEqual(r.returncode, 2, args)

    def test_help_without_display(self):
        r = subprocess.run([str(EXE), "--help"], capture_output=True, text=True, timeout=15)
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("keys", r.stdout.lower())


class ParserTests(unittest.TestCase):
    def test_native_parser(self):
        if not PARSER: self.skipTest("pass --parser to run compiled parser tests")
        with tempfile.TemporaryDirectory(prefix="prime args α ") as d:
            subprocess.run([str(PARSER), d], check=True)


if __name__ == "__main__":
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--schema", type=Path)
    args.add_argument("--exe", type=lambda s: Path(s).resolve())
    args.add_argument("--parser", type=lambda s: Path(s).resolve())
    o, remaining = args.parse_known_args()
    SCHEMA, EXE, PARSER = o.schema, o.exe, o.parser
    # unittest decorators are evaluated before CLI arguments are parsed.
    if EXE:
        RuntimeTests.__unittest_skip__ = False
    unittest.main(argv=[os.sys.argv[0], *remaining])
