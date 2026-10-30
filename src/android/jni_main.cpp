// JNI entry points of libhc09.so (com.hc09.recomp.NativeBridge) and the render thread.
//
// stdout/stderr go to logcat (tag HC09), so the host layer's existing logging shows up there.
// Until the Vulkan renderer draws the game, the render thread clears the window to a colour that
// cycles with the game's frame counter (or its own, when the game is not running): a visible sign
// that presentation works and the game is alive.

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "gpu/draw.h"
#include "gpu/vulkan_context.h"

namespace hc09
{
    int RunGame(const char* imagePath, const char* xexPath, const char* isoPath, const char* userData);
}

namespace
{
    constexpr const char* kTag = "HC09";

    // ---- stdout/stderr to logcat -------------------------------------------------------------

    void RedirectOutputToLogcat()
    {
        int pipeFds[2];
        if (pipe(pipeFds) != 0)
            return;
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IOLBF, 0);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[1]);
        std::thread([fd = pipeFds[0]] {
            std::string line;
            char buffer[512];
            ssize_t count;
            while ((count = read(fd, buffer, sizeof(buffer))) > 0)
            {
                for (ssize_t i = 0; i < count; i++)
                {
                    if (buffer[i] == '\n')
                    {
                        __android_log_write(ANDROID_LOG_INFO, kTag, line.c_str());
                        line.clear();
                    }
                    else
                    {
                        line.push_back(buffer[i]);
                    }
                }
            }
        }).detach();
    }

    // ---- Render thread ---------------------------------------------------------------------------

    std::mutex g_windowMutex;
    std::condition_variable g_windowChanged;
    ANativeWindow* g_pendingWindow = nullptr;  // owned reference, handed to the render thread
    bool g_windowUpdate = false;
    bool g_renderThreadStarted = false;
    std::atomic<bool> g_gameStarted{ false };

    void HueToRgb(float hue, float& r, float& g, float& b)
    {
        const float h = hue * 6.0f;
        const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
        const int sector = int(h) % 6;
        const float table[6][3] = { { 1, x, 0 }, { x, 1, 0 }, { 0, 1, x }, { 0, x, 1 }, { x, 0, 1 }, { 1, 0, x } };
        // Dimmed so the test screen is not glaring.
        r = table[sector][0] * 0.6f;
        g = table[sector][1] * 0.6f;
        b = table[sector][2] * 0.6f;
    }

    void RenderThread()
    {
        hc09::gpu::VulkanContext context;
        ANativeWindow* window = nullptr;
        uint64_t frames = 0;
        for (;;)
        {
            {
                std::unique_lock lock(g_windowMutex);
                if (!window && !g_windowUpdate)
                    g_windowChanged.wait(lock, [] { return g_windowUpdate; });
                if (g_windowUpdate)
                {
                    g_windowUpdate = false;
                    if (window)
                    {
                        context.ReleaseWindow();
                        ANativeWindow_release(window);
                    }
                    window = g_pendingWindow;
                    g_pendingWindow = nullptr;
                    if (window && !context.Initialize(window))
                        __android_log_write(ANDROID_LOG_ERROR, kTag, "Vulkan initialization failed");
                }
            }
            if (!window)
                continue;
            const int gameFrame = g_gameStarted ? hc09::gpu::FrameNumber() : 0;
            const uint64_t tick = g_gameStarted ? uint64_t(gameFrame) : frames;
            float r, g, b;
            HueToRgb(float(tick % 360) / 360.0f, r, g, b);
            if (!context.ClearAndPresent(r, g, b))
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            if (++frames % 600 == 0)
                std::fprintf(stderr, "[render] %llu frames presented (game frame %d)\n", (unsigned long long)frames, gameFrame);
        }
    }

    std::string JavaString(JNIEnv* env, jstring text)
    {
        if (!text)
            return {};
        const char* chars = env->GetStringUTFChars(text, nullptr);
        std::string result(chars);
        env->ReleaseStringUTFChars(text, chars);
        return result;
    }
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/)
{
    RedirectOutputToLogcat();
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL Java_com_hc09_recomp_NativeBridge_setSurface(JNIEnv* env, jclass, jobject surface)
{
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::lock_guard lock(g_windowMutex);
    if (g_pendingWindow)
        ANativeWindow_release(g_pendingWindow);
    g_pendingWindow = window;
    g_windowUpdate = true;
    if (!g_renderThreadStarted)
    {
        g_renderThreadStarted = true;
        std::thread(RenderThread).detach();
    }
    g_windowChanged.notify_all();
}

extern "C" JNIEXPORT void JNICALL Java_com_hc09_recomp_NativeBridge_startGame(JNIEnv* env, jclass, jstring image, jstring xex,
    jstring iso, jstring userData)
{
    if (g_gameStarted.exchange(true))
        return;
    struct Args
    {
        std::string image, xex, iso, userData;
    };
    auto* args = new Args{ JavaString(env, image), JavaString(env, xex), JavaString(env, iso), JavaString(env, userData) };
    // The recompiled code runs deep on the guest main thread; give it the stack a desktop main thread has.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16 << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    pthread_create(&thread, &attributes, [](void* argument) -> void* {
        auto* a = static_cast<Args*>(argument);
        const int result = hc09::RunGame(a->image.c_str(), a->xex.c_str(), a->iso.c_str(), a->userData.c_str());
        std::fprintf(stderr, "[boot] game exited with %d\n", result);
        delete a;
        return nullptr;
    }, args);
    pthread_attr_destroy(&attributes);
}
