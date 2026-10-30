// Write watch on guest physical memory for the Android renderer: see write_watch.h.

#include "gpu/write_watch.h"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "memory.h"

namespace hc09::gpu::write_watch
{
    namespace
    {
        constexpr uint32_t kPhysicalSize = 0x20000000;

        uint32_t g_pageShift = 12;
        uint32_t g_pages = 0;
        uint8_t* g_windows[3]{};
        int g_windowCount = 0;
        // Per host page: watched (write-protected) bits, and the clock value of the last write seen.
        std::unique_ptr<std::atomic<uint64_t>[]> g_watched;
        std::unique_ptr<std::atomic<uint32_t>[]> g_written;
        std::atomic<uint32_t> g_clock{ 1 };
        std::atomic<uint64_t> g_faults{ 0 };
        // Held while a page's watched bit and its protection change together (Watch on the command
        // processor thread, the handler on the writing thread), so the two always agree.
        std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
        struct sigaction g_previous{};

        void Lock()
        {
            while (g_lock.test_and_set(std::memory_order_acquire))
            {
            }
        }
        void Unlock() { g_lock.clear(std::memory_order_release); }

        bool IsWatched(uint32_t page) { return (g_watched[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1; }

        void Protect(uint32_t firstPage, uint32_t count, int protection)
        {
            for (int i = 0; i < g_windowCount; i++)
                mprotect(g_windows[i] + (uint64_t(firstPage) << g_pageShift), uint64_t(count) << g_pageShift, protection);
        }

        // Physical page of a host address in one of the windows, or -1.
        int64_t PageOf(uintptr_t address)
        {
            for (int i = 0; i < g_windowCount; i++)
            {
                const auto window = reinterpret_cast<uintptr_t>(g_windows[i]);
                if (address >= window && address < window + kPhysicalSize)
                    return int64_t((address - window) >> g_pageShift);
            }
            return -1;
        }

        void OnFault(int signal, siginfo_t* info, void* context)
        {
            const int64_t page = signal == SIGSEGV && info->si_code == SEGV_ACCERR ? PageOf(reinterpret_cast<uintptr_t>(info->si_addr)) : -1;
            if (page >= 0)
            {
                Lock();
                if (IsWatched(uint32_t(page)))
                {
                    g_watched[page >> 6].fetch_and(~(1ull << (page & 63)), std::memory_order_relaxed);
                    g_written[page].store(g_clock.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_release);
                    Protect(uint32_t(page), 1, PROT_READ | PROT_WRITE);
                    g_faults.fetch_add(1, std::memory_order_relaxed);
                }
                Unlock();
                return;  // the write is retried (and succeeds once the protection is lifted)
            }
            // Not ours: the handler that was there before (the fault report).
            if (g_previous.sa_flags & SA_SIGINFO)
            {
                g_previous.sa_sigaction(signal, info, context);
            }
            else if (g_previous.sa_handler == SIG_DFL)
            {
                ::signal(signal, SIG_DFL);
                ::raise(signal);
            }
            else if (g_previous.sa_handler != SIG_IGN)
            {
                g_previous.sa_handler(signal);
            }
        }

        bool SetUp()
        {
            // Off unless HC09_WRITE_WATCH=1: with buffers watched, the menus' 3D background and some textures
            // (uniforms) drop out at times, although HC09_WATCH_VERIFY finds no change it missed (2026-10-30).
            if (const char* value = std::getenv("HC09_WRITE_WATCH"); !value || std::atoi(value) == 0)
                return false;
            if (!g_base)
                return false;
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize <= 0 || (pageSize & (pageSize - 1)) != 0)
                return false;
            g_pageShift = uint32_t(__builtin_ctzl(uint64_t(pageSize)));
            g_pages = kPhysicalSize >> g_pageShift;
            g_watched = std::make_unique<std::atomic<uint64_t>[]>((g_pages + 63) / 64);
            g_written = std::make_unique<std::atomic<uint32_t>[]>(g_pages);
            for (uint32_t window : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
                if (window != 0xE0000000u || !std::getenv("HC09_NO_E_WINDOW"))
                    g_windows[g_windowCount++] = GuestToHost(window);
            struct sigaction action{};
            action.sa_sigaction = OnFault;
            action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGSEGV, &action, &g_previous) != 0)
                return false;
            std::fprintf(stderr, "[gpu] write watch on guest physical memory: on (%ld-byte pages)\n", pageSize);
            return true;
        }

        // Physical page range of guest [address, address + length).
        void Pages(uint32_t address, uint32_t length, uint32_t& first, uint32_t& last)
        {
            const uint32_t offset = address & (kPhysicalSize - 1);
            first = offset >> g_pageShift;
            last = uint32_t(std::min<uint64_t>(uint64_t(offset) + length - 1, kPhysicalSize - 1) >> g_pageShift);
        }
    }

    bool Enabled()
    {
        static const bool enabled = SetUp();
        return enabled;
    }

    bool Watchable(uint32_t address, uint32_t length)
    {
        return Enabled() && length > 0 && address >= kPhysicalHeapBase && uint64_t(address & (kPhysicalSize - 1)) + length <= kPhysicalSize;
    }

    uint32_t Now()
    {
        return g_clock.load(std::memory_order_acquire);
    }

    void Watch(uint32_t address, uint32_t length)
    {
        if (!Watchable(address, length))
            return;
        uint32_t first, last;
        Pages(address, length, first, last);
        Lock();
        for (uint32_t page = first; page <= last;)
        {
            if (IsWatched(page))
            {
                page++;
                continue;
            }
            uint32_t end = page;
            while (end <= last && !IsWatched(end))
            {
                g_watched[end >> 6].fetch_or(1ull << (end & 63), std::memory_order_relaxed);
                end++;
            }
            Protect(page, end - page, PROT_READ);
            page = end;
        }
        Unlock();
    }

    bool WrittenSince(uint32_t address, uint32_t length, uint32_t stamp)
    {
        if (!Watchable(address, length))
            return true;
        uint32_t first, last;
        Pages(address, length, first, last);
        for (uint32_t page = first; page <= last; page++)
            if (!IsWatched(page) || g_written[page].load(std::memory_order_acquire) > stamp)
                return true;
        return false;
    }

    uint64_t TakeFaults()
    {
        return g_faults.exchange(0, std::memory_order_relaxed);
    }
}
