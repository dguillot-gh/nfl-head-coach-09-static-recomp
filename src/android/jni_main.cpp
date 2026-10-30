// JNI entry points of libhc09.so (com.hc09.recomp.NativeBridge), the render thread's test loop, and
// the app services the host layer uses (android/app_glue.h).
//
// stdout/stderr go to logcat (tag HC09), so the host layer's existing logging shows up there.
// Until the game's renderer takes the window over (at its first draw), the render thread clears it to
// a colour that cycles with the game's frame counter (or its own, when the game is not running): a
// visible sign that presentation works and the game is alive.

#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "android/app_glue.h"
#include "gpu/draw.h"
#include "gpu/vulkan_context.h"
#include "kernel/host_input.h"

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
    ANativeWindow* g_window = nullptr;  // owned reference to the current window (null: none)
    uint64_t g_windowGeneration = 0;    // bumped at every setSurface
    bool g_renderThreadStarted = false;
    bool g_takeOver = false;            // the renderer wants the window
    bool g_renderThreadDone = false;    // ... and the test loop has let go of it
    std::atomic<bool> g_gameStarted{ false };
    AAssetManager* g_assets = nullptr;
    jobject g_assetsObject = nullptr;   // global reference keeping g_assets alive

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
        uint64_t generation = 0;
        bool haveWindow = false;
        uint64_t frames = 0;
        for (;;)
        {
            {
                std::unique_lock lock(g_windowMutex);
                if (!haveWindow)
                    g_windowChanged.wait(lock, [&] { return g_takeOver || g_windowGeneration != generation; });
                if (g_takeOver)
                {
                    // Black for whatever shows until the renderer's first frame (the shader pre-compile).
                    if (haveWindow)
                        context.ClearAndPresent(0.0f, 0.0f, 0.0f);
                    context.Shutdown();
                    g_renderThreadDone = true;
                    g_windowChanged.notify_all();
                    return;
                }
                if (g_windowGeneration != generation)
                {
                    generation = g_windowGeneration;
                    haveWindow = g_window != nullptr;
                    if (!context.Initialize(g_window))
                        __android_log_write(ANDROID_LOG_ERROR, kTag, "Vulkan initialization failed");
                }
            }
            if (!haveWindow)
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

    // KEY=VALUE lines of env.txt next to the game files: debug and test options (HC09_PRESS, ...).
    void LoadEnvironment(const std::string& imagePath)
    {
        const std::string folder = imagePath.substr(0, imagePath.find_last_of('/') + 1);
        std::ifstream file(folder + "env.txt");
        std::string line;
        while (std::getline(file, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            const size_t equals = line.find('=');
            if (line.empty() || line[0] == '#' || equals == std::string::npos)
                continue;
            setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
            std::fprintf(stderr, "[boot] env.txt: %s\n", line.c_str());
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
    if (g_window)
        ANativeWindow_release(g_window);
    g_window = window;
    g_windowGeneration++;
    if (!g_renderThreadStarted && !g_takeOver)
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
    LoadEnvironment(args->image);
    // The renderer keeps the driver's compiled pipelines next to the user data (the app's files folder).
    const std::string files = args->userData.substr(0, args->userData.find_last_of('/'));
    setenv("HC09_PIPELINE_CACHE", (files + "/vulkan_pipelines.bin").c_str(), 0);
    setenv("HC09_PIPELINE_LIST", (files + "/pipelines.txt").c_str(), 0);
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

extern "C" JNIEXPORT void JNICALL Java_com_hc09_recomp_NativeBridge_setAssetManager(JNIEnv* env, jclass, jobject assets)
{
    if (g_assetsObject)
        return;
    g_assetsObject = env->NewGlobalRef(assets);
    g_assets = AAssetManager_fromJava(env, g_assetsObject);
}

// Player 1's controller: the on-screen pad merged with any real gamepad (GameActivity does the merging).
extern "C" JNIEXPORT void JNICALL Java_com_hc09_recomp_NativeBridge_setPad(JNIEnv*, jclass, jint buttons, jint leftTrigger,
    jint rightTrigger, jint leftX, jint leftY, jint rightX, jint rightY)
{
    XINPUT_GAMEPAD pad{};
    pad.wButtons = WORD(buttons);
    pad.bLeftTrigger = BYTE(leftTrigger);
    pad.bRightTrigger = BYTE(rightTrigger);
    pad.sThumbLX = SHORT(leftX);
    pad.sThumbLY = SHORT(leftY);
    pad.sThumbRX = SHORT(rightX);
    pad.sThumbRY = SHORT(rightY);
    hc09::host::SetHostPad(true, pad);
}

// The pipeline pre-compile's progress as done << 16 | total, or -1 when it is not running.
extern "C" JNIEXPORT jint JNICALL Java_com_hc09_recomp_NativeBridge_shaderProgress(JNIEnv*, jclass)
{
    int done = 0, total = 0;
    if (!hc09::gpu::PrecompileProgress(done, total))
        return -1;
    return (std::min(done, 0x7FFF) << 16) | std::min(total, 0xFFFF);
}

// Frames the game has presented so far (the menu's frame-rate counter).
extern "C" JNIEXPORT jint JNICALL Java_com_hc09_recomp_NativeBridge_frameNumber(JNIEnv*, jclass)
{
    return g_gameStarted ? hc09::gpu::FrameNumber() : 0;
}

namespace hc09::android
{
    AAssetManager* Assets()
    {
        return g_assets;
    }

    void TakeOverWindow()
    {
        std::unique_lock lock(g_windowMutex);
        g_takeOver = true;
        g_windowChanged.notify_all();
        if (g_renderThreadStarted)
            g_windowChanged.wait(lock, [] { return g_renderThreadDone; });
    }

    bool PollWindow(uint64_t& generation, ANativeWindow*& window)
    {
        std::lock_guard lock(g_windowMutex);
        if (generation == g_windowGeneration)
            return false;
        generation = g_windowGeneration;
        window = g_window;
        if (window)
            ANativeWindow_acquire(window);
        return true;
    }
}
