"""Name guest addresses in Metroid Prime Remastered's main: mprsym.py [--exefs DIR] <addr>...

Prints the function containing each address (from main's dynamic symbol table) and the offset into it.
"""
import argparse
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import Nso  # noqa: E402

MAIN_BASE = 0x7000000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs', default=os.path.join(os.path.dirname(__file__), '..', 'private', 'exefs'))
    ap.add_argument('addr', nargs='+')
    a = ap.parse_args()
    n = Nso(os.path.join(a.exefs, 'main'))
    fns = sorted((v, s, name) for name, info, sh, v, s in n.syms if sh and (info & 0xF) == 2)
    starts = [f[0] for f in fns]
    for x in a.addr:
        off = int(x, 16) - MAIN_BASE
        i = bisect.bisect_right(starts, off) - 1
        if i < 0:
            print(f'{x} ?')
            continue
        v, s, name = fns[i]
        print(f'{x} {name}+{off - v:#x}' + ('' if off < v + max(s, 4) else ' (past its end)'))


if __name__ == '__main__':
    main()
