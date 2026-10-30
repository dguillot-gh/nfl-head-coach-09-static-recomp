// Renderer entry points for hosts without a renderer yet (the Android headless build): draws are
// counted and dropped, swaps advance the frame counter and log the frame rate now and then.

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "gpu/draw.h"

namespace hc09::gpu
{
    namespace
    {
        std::atomic<int> g_frame{ 0 };
        std::atomic<uint64_t> g_draws{ 0 };
    }

    bool IsDumpFrame()
    {
        return false;
    }

    void ExecuteDraw(const DrawInfo& /*draw*/)
    {
        g_draws++;
    }

    int FrameNumber()
    {
        return g_frame;
    }

    void ExecuteSwap(uint32_t frontBuffer, uint32_t width, uint32_t height)
    {
        static auto last = std::chrono::steady_clock::now();
        static int lastFrame = 0;
        static uint64_t lastDraws = 0;
        const int frame = ++g_frame;
        if (frame == 1)
            std::fprintf(stderr, "[frame] first swap: front buffer 0x%08X %ux%u\n", frontBuffer, width, height);
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        if (seconds >= 5.0)
        {
            const uint64_t draws = g_draws;
            std::fprintf(stderr, "[frame] %d: %.1f swaps/s, %.0f draws/frame (no renderer)\n", frame,
                (frame - lastFrame) / seconds, double(draws - lastDraws) / std::max(1, frame - lastFrame));
            last = now;
            lastFrame = frame;
            lastDraws = draws;
        }
    }
}
