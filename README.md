# 3DODroid

A from-scratch 3DO Interactive Multiplayer emulator for Android, targeting the Retroid Pocket Nero.

Bring your own BIOS (e.g. `panafz10.bin`); none is included.

## Layout
- `core/` – shared C core: ARM60 CPU, bus/memory map, MADAM, CLIO (more chips to come)
- `desktop/run3do` – headless test runner: `run3do bios.bin [frames] [--trace] [--shot out.ppm]`
- `tools/cputest` – CPU self-test (compares an ARM-compiled program's hash against a native run)
- `tools/disctest` – disc backend self-test
- `android/` – app shell (in progress)

## Disc images
ISO, BIN/CUE (MODE1/2048, MODE1/2352, MODE2/2352, AUDIO; multi-file sheets) and
CHD (MAME CD images, via [libchdr](https://github.com/rtissera/libchdr), BSD
licence, vendored in `third_party/libchdr`).

## Build (desktop)
    make            # builds desktop/run3do and the disc self-test
    make test       # synthetic ISO / BIN+CUE / CHD round-trip (CHD needs chdman)
    desktop/run3do bios.bin 600 --disc game.chd --shot out.ppm
