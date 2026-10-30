package com.hc09.recomp;

import android.view.Surface;

/** JNI entry points of libhc09.so (src/android/jni_main.cpp). */
final class NativeBridge {
    static {
        System.loadLibrary("hc09");
    }

    private NativeBridge() {}

    /** Gives the renderer the window to present to (null when it goes away). */
    static native void setSurface(Surface surface);

    /** Boots the game on its own thread (once per process). */
    static native void startGame(String imagePath, String xexPath, String isoPath, String userDataPath);
}
