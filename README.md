# 3DODroid

A from-scratch 3DO Interactive Multiplayer emulator for Android, targeting the Retroid Pocket Nero.

Bring your own BIOS (e.g. `panafz10.bin`); none is included.

## Install
Get the latest APK from the [Releases page](https://github.com/RetroPortLab/3DODroid/releases).
On first launch choose your BIOS file and your games folder. After that the app
checks GitHub for new versions itself.

**Controls (Retroid layout):** X or Y = 3DO A, A = B, B = C, Start = P (play/pause),
Select = X (stop), L1/L2 = L, R1/R2 = R, d-pad or left stick to move.
Select + Start (or Back) opens the in-game menu.

**Status:** experimental. Star Fighter runs from its intro into 3D gameplay.
There's no sound yet (the DSP isn't emulated).

## Layout
- `core/` – shared C core: ARM60 CPU, bus/memory map, MADAM, CLIO (more chips to come)
- `desktop/run3do` – headless test runner: `run3do bios.bin [frames] [--trace] [--shot out.ppm]`
- `tools/cputest` – CPU self-test (compares an ARM-compiled program's hash against a native run)
- `tools/disctest` – disc backend self-test
- `android/` – the Android app (library, settings, in-game menu, GitHub updater)
- `dist/` – signed release APKs; pushing one publishes a GitHub release

## Disc images
ISO, BIN/CUE (MODE1/2048, MODE1/2352, MODE2/2352, AUDIO; multi-file sheets) and
CHD (MAME CD images, via [libchdr](https://github.com/rtissera/libchdr), BSD
licence, vendored in `third_party/libchdr`).

## Build (desktop)
    make            # builds desktop/run3do and the disc self-test
    make test       # synthetic ISO / BIN+CUE / CHD round-trip (CHD needs chdman)
    desktop/run3do bios.bin 600 --disc game.chd --shot out.ppm

## Build (Android)
    cd android && gradle assembleRelease
Needs the Android SDK, NDK 27 and CMake 3.22. The release key is read from
`android/keystore.properties` (not in the repo); without it the build falls back
to the debug key.
