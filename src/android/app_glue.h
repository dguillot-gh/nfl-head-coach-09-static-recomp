#pragma once

// Services of the Android app for the host layer (implemented in jni_main.cpp).

#include <cstdint>

struct AAssetManager;
struct ANativeWindow;

namespace hc09::android
{
    // The APK's assets (the shaders); null before NativeBridge.setAssetManager.
    AAssetManager* Assets();

    // Moves the window from the render thread's test loop to the caller (the renderer), blocking until
    // that loop has let go of it. Called once.
    void TakeOverWindow();

    // When the window changed since `generation` (which is updated): true, with `window` the new one
    // (an acquired reference the caller releases) or null when it went away.
    bool PollWindow(uint64_t& generation, ANativeWindow*& window);
}

namespace hc09::gpu
{
    // The renderer's pipeline pre-compile at start: false when it is not running.
    bool PrecompileProgress(int& done, int& total);
}
