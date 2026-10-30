// Stack dumps on POSIX hosts: a thread's stack, unwound with the C++ runtime's unwinder and named
// with dladdr (recompiled functions show as sub_<guest address>). Other threads are dumped by
// signalling each registered thread, which prints its own stack. Hardware watchpoints are
// Windows-only.
//
// HC09_DUMP_STACKS=<seconds>: dump every thread after that delay.
// HC09_DUMP_ON_STALL=1: dump every thread once when no frame has been presented for 5 seconds.

#include "debug/stack_dump.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "gpu/draw.h"

namespace hc09::debug
{
    namespace
    {
        struct Walk
        {
            int depth = 0;
        };

        _Unwind_Reason_Code PrintFrame(_Unwind_Context* context, void* argument)
        {
            auto* walk = static_cast<Walk*>(argument);
            const uintptr_t pc = _Unwind_GetIP(context);
            if (pc == 0)
                return _URC_END_OF_STACK;
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname)
                std::fprintf(stderr, "  #%02d %s+0x%zx\n", walk->depth, info.dli_sname,
                    size_t(pc - reinterpret_cast<uintptr_t>(info.dli_saddr)));
            else if (info.dli_fname)
                std::fprintf(stderr, "  #%02d %s+0x%zx\n", walk->depth, info.dli_fname,
                    size_t(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)));
            else
                std::fprintf(stderr, "  #%02d 0x%zx\n", walk->depth, size_t(pc));
            return ++walk->depth < 64 ? _URC_NO_REASON : _URC_END_OF_STACK;
        }

        std::mutex g_threadsMutex;
        std::vector<std::pair<pid_t, uint32_t>> g_threads;  // host thread id, guest thread id
        std::mutex g_dumpMutex;
        std::atomic<int> g_dumped{ 0 };
        constexpr int kDumpSignal = SIGUSR2;

        void DumpSignal(int, siginfo_t*, void*)
        {
            // Not async-signal-safe (stdio), but only used to look at a hung process.
            std::lock_guard lock(g_dumpMutex);
            uint32_t guest = 0;
            const pid_t self = pid_t(syscall(SYS_gettid));
            for (const auto& [tid, id] : g_threads)
                if (tid == self)
                    guest = id;
            std::fprintf(stderr, "[stack] thread 0x%X (tid %d):\n", guest, int(self));
            PrintCurrentStack();
            g_dumped++;
        }
    }

    void RegisterThread(uint32_t guestThreadId)
    {
        std::lock_guard lock(g_threadsMutex);
        g_threads.emplace_back(pid_t(syscall(SYS_gettid)), guestThreadId);
        // Named by guest id ("hc09 0xC0" is the command processor) for ps/top/perf.
        char name[16];
        std::snprintf(name, sizeof(name), "hc09 0x%X", guestThreadId);
        pthread_setname_np(pthread_self(), name);
    }

    void DumpAllStacks()
    {
        static std::once_flag installed;
        std::call_once(installed, [] {
            struct sigaction action{};
            action.sa_sigaction = DumpSignal;
            action.sa_flags = SA_SIGINFO | SA_RESTART;
            sigaction(kDumpSignal, &action, nullptr);
        });
        std::vector<std::pair<pid_t, uint32_t>> threads;
        {
            std::lock_guard lock(g_threadsMutex);
            threads = g_threads;
        }
        std::fprintf(stderr, "[stack] dumping %zu threads\n", threads.size());
        for (const auto& [tid, id] : threads)
        {
            const int before = g_dumped;
            if (syscall(SYS_tgkill, getpid(), tid, kDumpSignal) != 0)
                continue;  // (the thread has exited)
            for (int i = 0; i < 200 && g_dumped == before; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr, "[stack] done\n");
    }

    void PrintStackFromContext(const void* /*context*/)
    {
        // From a signal handler the unwinder walks through the signal frame into the faulting code.
        PrintCurrentStack();
    }

    void PrintCurrentStack()
    {
        Walk walk;
        _Unwind_Backtrace(PrintFrame, &walk);
        std::fflush(stderr);
    }

    void StartWatchdogFromEnvironment()
    {
        if (std::getenv("HC09_WATCH"))
            std::fprintf(stderr, "[stack] HC09_WATCH is Windows-only\n");
        if (const char* delay = std::getenv("HC09_DUMP_STACKS"))
        {
            const int seconds = std::atoi(delay);
            std::thread([seconds] {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                DumpAllStacks();
            }).detach();
        }
        if (std::getenv("HC09_DUMP_ON_STALL"))
        {
            std::thread([] {
                int lastFrame = -1, still = 0;
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    const int frame = gpu::FrameNumber();
                    still = frame == lastFrame && frame > 0 ? still + 1 : 0;
                    lastFrame = frame;
                    if (still == 5)
                    {
                        std::fprintf(stderr, "[stack] no frame for 5 s (frame %d)\n", frame);
                        DumpAllStacks();
                    }
                }
            }).detach();
        }
    }
}
