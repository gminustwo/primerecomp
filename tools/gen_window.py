"""gen.py for a guest window other than the computer's 0x70_0000_0000 (Android's 0x10_0000_0000: runtime/rt.h).

Usage as gen.py's: gen_window.py <name> <nso> <base> <outdir> [...], with <base> the module's address in that window
(main at the window, sdk 0x600_0000 past it). gen.py lifts at any base, but its hand-written patches (patches.py) are
keyed by their address with main at 0x70_0000_0000: they are moved here, with the labels they jump to, by as much as
the window is. (Kept out of gen.py so that the computers' game code id - a hash of gen.py and patches.py, Makefile
GAMELIB_ID - stays as it is. The start screen's build runs gen.py itself: the computer's window only.)
"""
import re
import sys

import gen

PC_BASES = {'main': 0x7000000000, 'sdk': 0x7006000000}


def moved(patches, delta):
    def at(f, a):
        def patch(g, _a):
            return re.sub(r'\bL_([0-9a-f]+)\b', lambda m: f'L_{int(m.group(1), 16) + delta:x}', f(g, a))
        return patch
    return {a + delta: at(f, a) for a, f in patches.items()}


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[1] not in PC_BASES:
        sys.exit(__doc__)
    delta = int(sys.argv[3], 0) - PC_BASES[sys.argv[1]]
    if delta % 0x1000:
        sys.exit('gen_window.py: the base must move by whole pages')
    gen.PATCHES = moved(gen.PATCHES, delta)
    gen.main()
