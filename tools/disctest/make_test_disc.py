#!/usr/bin/env python3
"""Writes a synthetic 2-track test disc (MODE1/2352 data + audio) as BIN/CUE
and ISO, and a CHD too if MAME's chdman is installed."""
import os, shutil, struct, subprocess, sys

out = sys.argv[1] if len(sys.argv) > 1 else 'testdisc'
os.makedirs(out, exist_ok=True)
bcd = lambda v: ((v // 10) << 4) | (v % 10)

def data_sector(lba):
    m, s, f = (lba + 150) // 4500, ((lba + 150) // 75) % 60, (lba + 150) % 75
    user = struct.pack('>I', lba) + bytes(((lba * 7 + i) & 0xff) for i in range(4, 2048))
    return bytes([0] + [0xff] * 10 + [0]) + bytes([bcd(m), bcd(s), bcd(f), 1]) + user + bytes(288)

with open(f'{out}/data.bin', 'wb') as f, open(f'{out}/test.iso', 'wb') as iso:
    for l in range(300):
        sec = data_sector(l); f.write(sec); iso.write(sec[16:16 + 2048])
with open(f'{out}/audio.bin', 'wb') as f:          # 2s INDEX 00 pregap + 2s audio
    for l in range(300):
        f.write(bytes(((l * 3 + i) & 0xff) for i in range(2352)))
with open(f'{out}/test.cue', 'w') as f:
    f.write('FILE "data.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
            'FILE "audio.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')
if shutil.which('chdman'):
    subprocess.run(['chdman', 'createcd', '-f', '-i', f'{out}/test.cue', '-o', f'{out}/test.chd'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
