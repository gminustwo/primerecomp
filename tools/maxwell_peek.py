"""List a Maxwell shader's instructions around a given one, with names for the opcodes that matter here.

maxwell_peek.py <code.bin> <hex instruction word> [context]  -- raw code as dumped by LARECOMP_DUMP_FAILED_SHADERS
(NVN graphics stages start with a 0x50-byte header; every fourth 64-bit word is a scheduling word).
"""
import struct
import sys

NAMES = [  # (value, mask, name) on the top 16 bits
    (0xEFA0, 0xFFF8, 'AL2P'), (0xEFD8, 0xFFF8, 'ALD'), (0xEFF0, 0xFFF8, 'AST'), (0xE000, 0xFC00, 'IPA'),
    (0xEB00, 0xFFE0, 'SULD'), (0xEB20, 0xFFE0, 'SUST'), (0xDF00, 0xFF40, 'TLD4S'), (0xC000, 0xFC00, 'TEX'),
    (0xD000, 0xF600, 'TEXS'), (0xDA00, 0xFE00, 'TLDS'), (0xE300, 0xFFFF, 'EXIT'), (0xE240, 0xFFF0, 'BRA'),
    (0xF0C8, 0xFFF8, 'S2R'), (0x5C98, 0xFFF8, 'MOV'), (0x4C98, 0xFFF8, 'MOV(c)'), (0x0100, 0xFFF0, 'MOV32I'),
    (0xEF90, 0xFFF8, 'LDC'), (0x5980, 0xFF80, 'FFMA'), (0x5C68, 0xFFF8, 'FMUL'), (0x5C58, 0xFFF8, 'FADD'),
    (0x4C68, 0xFFF8, 'FMUL(c)'), (0x4C58, 0xFFF8, 'FADD(c)'), (0x4980, 0xFF80, 'FFMA(c)'), (0xEF10, 0xFFF8, 'SHFL'),
]


def name(w):
    top = w >> 48
    for v, m, n in NAMES:
        if top & m == v:
            return n
    return '?'


def main():
    data = open(sys.argv[1], 'rb').read()
    target = int(sys.argv[2], 16)
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    words = struct.unpack(f'<{len(data) // 8}Q', data[:len(data) // 8 * 8])
    for i, w in enumerate(words):
        if w == target:
            for j in range(max(0, i - ctx), min(len(words), i + ctx)):
                sched = (j * 8 - 0x50) % 32 == 0
                print(f'{"->" if j == i else "  "} {j * 8:#06x} {words[j]:016x} {"(sched)" if sched else name(words[j])}'
                      f'  rd=r{words[j] & 0xff} ra=r{(words[j] >> 8) & 0xff} imm20={(words[j] >> 20) & 0x7ff:#x}')
            return
    print('not found')


if __name__ == '__main__':
    main()
