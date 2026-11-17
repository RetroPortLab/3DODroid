# 3DODroid

A from-scratch 3DO Interactive Multiplayer emulator for Android, targeting the Retroid Pocket Nero.

Bring your own BIOS (e.g. `panafz10.bin`); none is included.

## Layout
- `core/` – shared C core: ARM60 CPU, bus/memory map, MADAM, CLIO (more chips to come)
- `desktop/run3do` – headless test runner: `run3do bios.bin [frames] [--trace] [--shot out.ppm]`
- `tools/cputest` – CPU self-test (compares an ARM-compiled program's hash against a native run)
- `android/` – app shell (in progress)

## Build (desktop)
    gcc -O2 -o desktop/run3do desktop/run3do.c core/*.c
