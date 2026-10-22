"""Disassemble guest code of Metroid Prime Remastered: mprdis.py <addr> [--before N] [--after N] [--fn]

--fn: the whole function holding <addr> (bounds from the unwind info), marking indirect branches.
Addresses are absolute (main at 0x7000000000, sdk at 0x7006000000).
"""
import argparse
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone  # noqa: E402
from analyze import eh_frame_functions  # noqa: E402
from nso import Nso  # noqa: E402

MODS = [('main', 0x7000000000), ('sdk', 0x7006000000)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addr')
    ap.add_argument('--before', type=int, default=16)
    ap.add_argument('--after', type=int, default=8)
    ap.add_argument('--fn', action='store_true')
    ap.add_argument('--exefs', default=os.path.join(os.path.dirname(__file__), '..', 'private', 'exefs'))
    a = ap.parse_args()
    addr = int(a.addr, 16)
    name, base = [m for m in MODS if m[1] <= addr][-1]
    n = Nso(os.path.join(a.exefs, name))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    if a.fn:
        starts = sorted(eh_frame_functions(n))
        i = bisect.bisect_right(starts, addr - base) - 1
        lo, hi = base + starts[i], base + (starts[i + 1] if i + 1 < len(starts) else n.text[1])
    else:
        lo, hi = addr - 4 * a.before, addr + 4 * a.after
    print(f'{name} {lo:#x}..{hi:#x}')
    for ins in md.disasm(bytes(n.image[lo - base:hi - base]), lo):
        mark = '>>' if ins.address == addr else ('br' if ins.mnemonic in ('br', 'blr') else '  ')
        print(f'{mark} {ins.address:x}: {ins.mnemonic} {ins.op_str}')


if __name__ == '__main__':
    main()
