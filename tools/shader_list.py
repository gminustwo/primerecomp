#!/usr/bin/env python3
"""shader_list.py: the shipped shader list (runtime/nvn/shader_list.inc) from shader records and the game's files.

  py tools/shader_list.py --romfs private/game/romfs <cache/shaders.bin>... [-o runtime/nvn/shader_list.inc]

A shader record (cache/shaders.bin, runtime/nvn/vulkan/exec.c) holds the inputs of every program translated in a
player's runs - each stage's code as the game had it, and the shader keys - and every pipeline built for one. The list
keeps all of that but the code: for each stage, where its code is in the game's own files instead. Metroid Prime
Remastered's shaders are in its materials: each MTRL asset in a .pak (RFRM > MTRL form, zlib from 0x20) holds zlib
streams with the shader binaries in them (an NVN header, 78 56 34 12, before each), and MaterialArchive.arc holds such
streams as they are. The runtime inflates the streams the list names from the player's files, takes the code, and
checks it against the program's hash (its ckey: keys and code), so another version of the game file just finds
nothing. No code of the game's is in the list: the program records (keys, lengths, ckey), the locations, and the
pipeline records (render state: formats, vertex layout, blend).

List format (little-endian; zlib, then base64 in the .inc):
  u32 magic "PRSL", version, key_size, prog_size, pipe_size, nfiles, nprogs, npipes
  nfiles x { u16 length, the file's romfs path (UTF-8) }
  nprogs x { prog_size bytes: the program's record; for each of its stages a location:
             u16 file, u16 kind (1: an MTRL asset at outer_off, outer_size bytes; 0: the stream right in the file),
             u32 outer_size, u64 outer_off, u32 inner_off (the zlib stream: in the asset's data, or in the file),
             u32 code_off (where the stage's code starts in that stream's data) }
  npipes x { pipe_size bytes: the pipeline's record }
"""
import argparse, base64, collections, hashlib, os, struct, sys, zlib

REC_MAGIC, REC_VERSION = 0x31435250, 1
LIST_MAGIC, LIST_VERSION = 0x4C535250, 1
NVN_HDR = bytes.fromhex("78563412")


def read_record(path):
    d = open(path, "rb").read()
    magic, version, key_size, pipe_size = struct.unpack_from("<4I", d, 0)
    if magic != REC_MAGIC or version != REC_VERSION:
        sys.exit("%s: not a shader record of this version" % path)
    off, progs, pipes = 16, [], []
    while off + 16 <= len(d):
        typ, size, _ = struct.unpack_from("<IIQ", d, off)
        off += 16
        if off + size > len(d):
            break
        p = d[off:off + size]
        if typ == ord("P"):
            nst = p[8 + 2 * key_size + 3]
            lens = struct.unpack_from("<3I", p, (8 + 2 * key_size + 4 + 3) & ~3)
            na = size - sum(lens[:nst])
            code, at = [], na
            for i in range(nst):
                code.append(p[at:at + lens[i]])
                at += lens[i]
            progs.append((p[:na], code))
        elif typ == ord("L"):
            pipes.append(p)
        off += size
    return key_size, pipe_size, progs, pipes


def canon_pipe(p):
    """a pipeline record's key as draw_ makes it (exec.c pipe_key_canon): the colour slots past ncolor zero - an
    earlier build left an earlier pass's formats there (RecPipe: ckey, then PipeKey: prog, color[8], depth, stencil,
    ncolor)"""
    p = bytearray(p)
    nc = min(struct.unpack_from("<I", p, 56)[0], 8)
    for i in range(nc, 8):
        struct.pack_into("<I", p, 16 + 4 * i, 0)
    return bytes(p)


def inner_streams(buf):
    """every zlib stream in buf that unpacks cleanly: (offset, data)"""
    out, pos = [], 0
    while True:
        j = buf.find(b"\x78", pos)
        if j < 0 or j + 2 > len(buf):
            break
        if buf[j + 1] in (0x01, 0x5E, 0x9C, 0xDA):
            dobj = zlib.decompressobj()
            try:
                part = dobj.decompress(buf[j:j + (8 << 20)])
                if dobj.eof and len(part) > 0x40:
                    out.append((j, part))
                    pos = j + (len(buf[j:j + (8 << 20)]) - len(dobj.unused_data))
                    continue
            except zlib.error:
                pass
        pos = j + 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", required=True)
    ap.add_argument("-o", default=os.path.join(os.path.dirname(__file__), "..", "runtime", "nvn", "shader_list.inc"))
    ap.add_argument("records", nargs="+")
    a = ap.parse_args()

    # the programs and pipelines of every record (once each)
    key_size = prog_size = pipe_size = None
    progs, pipes, seen_p, seen_l = [], [], set(), set()
    for r in a.records:
        ks, ps, pr, pl = read_record(r)
        if key_size is None:
            key_size, pipe_size = ks, ps
        elif (ks, ps) != (key_size, pipe_size):
            sys.exit("%s: another build's layout" % r)
        n0, l0 = len(progs), len(pipes)
        for rec, code in pr:
            ckey = rec[:8]
            if ckey not in seen_p:
                seen_p.add(ckey)
                progs.append((rec, code))
                prog_size = len(rec) if prog_size is None else prog_size
        for p in pl:
            p = canon_pipe(p)
            if p not in seen_l:
                seen_l.add(p)
                pipes.append(p)
        print("%s: %d programs, %d pipelines (%d / %d new)" % (r, len(pr), len(pl), len(progs) - n0, len(pipes) - l0))

    # every shader binary in the game's materials, by its first code bytes
    files, where = [], collections.defaultdict(list)  # key -> [(file, kind, outer_off, outer_size, inner_off, pos, data)]

    def index(fi, kind, outer_off, outer_size, streams):
        for inner_off, data in streams:
            p = data.find(NVN_HDR)
            while p >= 0:
                where[data[p + 0x30:p + 0x70]].append((fi, kind, outer_off, outer_size, inner_off, p, data))
                p = data.find(NVN_HDR, p + 4)

    for root, _, fs in sorted(os.walk(a.romfs)):
        for f in sorted(fs):
            full = os.path.join(root, f)
            rel = os.path.relpath(full, a.romfs).replace(os.sep, "/")
            if f.endswith(".arc"):
                files.append(rel)
                index(len(files) - 1, 0, 0, 0, inner_streams(open(full, "rb").read()))
            elif f.endswith(".pak"):
                pd = open(full, "rb").read()
                i = pd.find(b"ADIR")
                if i < 0:
                    continue
                body, fi = i + 0x18, None
                for k in range(struct.unpack_from("<I", pd, body)[0]):
                    e = body + 4 + k * 52
                    if pd[e:e + 4] != b"MTRL":
                        continue
                    _, _, aoff, s1, _ = struct.unpack_from("<IIQQQ", pd, e + 20)
                    try:
                        outer = zlib.decompress(pd[aoff + 0x20:aoff + s1])
                    except zlib.error:
                        continue
                    if fi is None:
                        files.append(rel)
                        fi = len(files) - 1
                    index(fi, 1, aoff, s1, inner_streams(outer))
    print("game files: %d with shaders, %d shader binaries indexed" % (len(files), sum(map(len, where.values()))))

    # each stage's candidates; then each stage from the asset that can supply the most stages (fewer to inflate)
    cand = []
    for rec, code in progs:
        cs = []
        for c in code:
            ok = [w for w in where.get(c[0x30:0x70], []) if w[6][w[5] + 0x30:w[5] + len(c)] == c[0x30:]]
            cs.append(ok)
        cand.append(cs)
    supply = collections.Counter()
    for cs in cand:
        for ok in cs:
            for w in {(w[0], w[1], w[2]) for w in ok}:
                supply[w] += 1
    out_progs, missing = [], 0
    for (rec, code), cs in zip(progs, cand):
        if not all(cs):
            missing += 1
            continue
        locs = []
        for c, ok in zip(code, cs):
            w = max(ok, key=lambda w: supply[(w[0], w[1], w[2])])
            locs.append(struct.pack("<HHIQII", w[0], w[1], w[3], w[2], w[4], w[5]))
        out_progs.append(rec + b"".join(locs))
    kept = {p[:8] for p in out_progs}
    out_pipes = [p for p in pipes if p[:8] in kept]
    used = sorted({struct.unpack_from("<H", p, prog_size + 24 * i)[0] for p in out_progs
                   for i in range(p[8 + 2 * key_size + 3])})
    print("list: %d programs (%d not in the game's files), %d pipelines, from %d assets in %d files" %
          (len(out_progs), missing, len(out_pipes), len({(w[0], w[2]) for cs in cand for ok in cs for w in ok[:1]}), len(used)))

    blob = struct.pack("<8I", LIST_MAGIC, LIST_VERSION, key_size, prog_size, len(pipes[0]) if pipes else 0, len(files),
                       len(out_progs), len(out_pipes))
    for f in files:
        b = f.encode("utf-8")
        blob += struct.pack("<H", len(b)) + b
    blob += b"".join(out_progs) + b"".join(out_pipes)
    z = zlib.compress(blob, 9)
    b64 = base64.b64encode(z).decode()
    with open(a.o, "w", newline="\n") as o:
        o.write("// The shipped shader list (tools/shader_list.py, from shader records; runtime/nvn/vulkan/exec.c\n")
        o.write("// shader_list_seed): %d programs, %d pipelines - their keys, render state and where each one's code is\n"
                % (len(out_progs), len(out_pipes)))
        o.write("// in the game's own files. No game code. %d bytes, zlib %d, sha256 %s.\n" %
                (len(blob), len(z), hashlib.sha256(blob).hexdigest()[:16]))
        for i in range(0, len(b64), 120):
            o.write('"%s"\n' % b64[i:i + 120])
    print("%s: %d bytes (list %d, zlib %d)" % (a.o, os.path.getsize(a.o), len(blob), len(z)))


if __name__ == "__main__":
    main()
