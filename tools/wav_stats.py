"""Levels of a LARECOMP_AUDIO_WAV recording: wav_stats.py <wav> [<from frame> <to frame> [<step>]]

Frames are the audio renderer's (240 samples, 5 ms). The recording has the Speakers setting's channels (stereo, 5.1
or 7.1, in SDL's order). Prints RMS / peak / NaN / clipped samples and sudden steps (clicks) of the range, each
channel's RMS, then the RMS per <step> frames when a step is given. Needs numpy.
"""
import struct
import sys

import numpy as np

NAMES = {2: 'FL FR', 6: 'FL FR FC LFE BL BR', 8: 'FL FR FC LFE BL BR SL SR'}


def main():
    with open(sys.argv[1], 'rb') as f:
        ch = struct.unpack('<H', f.read(24)[22:24])[0]
    d = np.fromfile(sys.argv[1], dtype=np.float32, offset=44)
    d = d[:len(d) // ch * ch].reshape(-1, ch)
    a = int(sys.argv[2]) * 240 if len(sys.argv) > 2 else 0
    b = int(sys.argv[3]) * 240 if len(sys.argv) > 3 else len(d)
    seg = d[a:b]
    x = seg.mean(axis=1)
    dd = np.abs(np.diff(x, 2))
    lvl = np.convolve(dd, np.ones(481) / 481, mode='same') + 1e-4
    clicks = int(np.count_nonzero((dd > 0.02) & (dd > 12 * lvl)))
    print('frames %d-%d, %d channels: rms %.4f peak %.4f nan %d clipped %d clicks %d' % (
        a // 240, b // 240, ch, float(np.sqrt(np.mean(seg ** 2))), float(np.abs(seg).max()),
        int(np.isnan(seg).sum()), int((np.abs(seg) >= 0.999).sum()), clicks))
    names = NAMES.get(ch, ' '.join('ch%d' % i for i in range(ch))).split()
    print('  ' + '  '.join('%s %.4f' % (n, float(np.sqrt(np.mean(seg[:, i] ** 2)))) for i, n in enumerate(names)))
    if len(sys.argv) > 4:
        step = int(sys.argv[4]) * 240
        for i in range(0, len(seg) - step + 1, step):
            r = float(np.sqrt(np.mean(seg[i:i + step] ** 2)))
            print((a + i) // 240, '%.4f' % r, '#' * int(r * 400))


if __name__ == '__main__':
    main()
