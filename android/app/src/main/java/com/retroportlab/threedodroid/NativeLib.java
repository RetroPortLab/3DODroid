package com.retroportlab.threedodroid;

import android.view.Surface;

/** Thin JNI wrapper around the native emulator core. */
public final class NativeLib {
    static { System.loadLibrary("threedodroid"); }
    private NativeLib() {}

    /** Where the console's battery-backed memory (game saves) is kept. Set before loadGame. */
    public static native void setNvramPath(String path);
    /** For a .cue disc: the track files it names and descriptors opened for them (kept open by the caller). */
    public static native void setCueFiles(String[] names, int[] fds);
    /**
     * Boots the console with the given BIOS and disc. fd is the disc image, opened by the
     * caller and kept open while the game runs (-1 for no disc); type is chd, cue, iso or bin.
     */
    public static native boolean loadGame(byte[] bios, int fd, String type);
    public static native String lastError();
    public static native void setSurface(Surface surface);
    public static native void start();
    public static native void stop();
    public static native void reset();
    public static native void setPaused(boolean paused);
    public static native void setButtons(int mask);
    public static native float getFps();

    // 3DO control pad bits, in the order the console's event broker uses.
    public static final int DOWN = 0x80000000, UP = 0x40000000, RIGHT = 0x20000000, LEFT = 0x10000000;
    public static final int A = 0x08000000, B = 0x04000000, C = 0x02000000, P = 0x01000000;
    public static final int X = 0x00800000, R = 0x00400000, L = 0x00200000;
}
