"""Levels of a LARECOMP_AUDIO_WAV recording: wav_stats.py <wav> [<from frame> <to frame> [<step>]]

Frames are the audio renderer's (240 stereo samples, 5 ms). Prints RMS / peak / NaN / clipped samples and sudden
steps (clicks) of the range, then the RMS per <step> frames when a step is given. Needs numpy.
"""
import sys

import numpy as np


def main():
    d = np.fromfile(sys.argv[1], dtype=np.float32, offset=44).reshape(-1, 2)
    a = int(sys.argv[2]) * 240 if len(sys.argv) > 2 else 0
    b = int(sys.argv[3]) * 240 if len(sys.argv) > 3 else len(d)
    seg = d[a:b]
    x = seg.mean(axis=1)
    dd = np.abs(np.diff(x, 2))
    lvl = np.convolve(dd, np.ones(481) / 481, mode='same') + 1e-4
    clicks = int(np.count_nonzero((dd > 0.02) & (dd > 12 * lvl)))
    print('frames %d-%d: rms %.4f peak %.4f nan %d clipped %d clicks %d' % (
        a // 240, b // 240, float(np.sqrt(np.mean(seg ** 2))), float(np.abs(seg).max()),
        int(np.isnan(seg).sum()), int((np.abs(seg) >= 0.999).sum()), clicks))
    if len(sys.argv) > 4:
        step = int(sys.argv[4]) * 240
        for i in range(0, len(seg) - step + 1, step):
            r = float(np.sqrt(np.mean(seg[i:i + step] ** 2)))
            print((a + i) // 240, '%.4f' % r, '#' * int(r * 400))


if __name__ == '__main__':
    main()
