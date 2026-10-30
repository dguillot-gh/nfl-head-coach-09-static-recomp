package com.hc09.recomp;

import android.content.res.AssetManager;
import android.view.Surface;

/** JNI entry points of libhc09.so (src/android/jni_main.cpp). */
final class NativeBridge {
    static {
        System.loadLibrary("hc09");
    }

    private NativeBridge() {}

    /** Gives the renderer the window to present to (null when it goes away). */
    static native void setSurface(Surface surface);

    /** Gives the native side the APK's assets (the renderer's shaders). Call before startGame. */
    static native void setAssetManager(AssetManager assets);

    // XInput button bits (XINPUT_GAMEPAD_*).
    static final int UP = 0x0001, DOWN = 0x0002, LEFT = 0x0004, RIGHT = 0x0008, START = 0x0010, BACK = 0x0020,
            LEFT_THUMB = 0x0040, RIGHT_THUMB = 0x0080, LB = 0x0100, RB = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000,
            Y = 0x8000;

    /** Player 1's controller: XInput button bits, triggers 0-255, sticks -32768..32767 (y up). */
    static native void setPad(int buttons, int leftTrigger, int rightTrigger, int leftX, int leftY, int rightX, int rightY);

    /** The shader pre-compile's progress as done << 16 | total, or -1 when it is not running. */
    static native int shaderProgress();

    /** Frames the game has presented so far (0 before it starts). */
    static native int frameNumber();

    /** Boots the game on its own thread (once per process). */
    static native void startGame(String imagePath, String xexPath, String isoPath, String userDataPath);
}
