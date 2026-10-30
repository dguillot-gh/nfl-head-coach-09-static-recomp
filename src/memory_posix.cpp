// Guest address space on POSIX hosts (Android, Linux): the same layout as memory.cpp.
//
// The 4 GiB space is reserved inaccessible, the ordinary guest memory below 0xA0000000 is
// mapped read/write, and the 512 MiB of physical memory is one shared-memory object mapped into
// each of the three windows (0xA0000000, 0xC0000000, 0xE0000000) so they alias. Everything is
// MAP_NORESERVE: a page costs RAM only once the guest touches it, which matters on 4 GB phones.
// As on Windows, the 0xE0000000 window's 4 KiB offset is not reproduced.

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <android/sharedmem.h>
#endif

#include "memory.h"

namespace hc09
{
    namespace
    {
        constexpr uint64_t kPhysicalSize = 0x20000000;
        constexpr uint64_t kPhysicalViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

        // A file descriptor for kPhysicalSize bytes of shared memory, or -1.
        int CreatePhysicalMemory()
        {
#if defined(SYS_memfd_create)
            // memfd_create has no libc wrapper below Android API 30; the system call is older (Linux 3.17).
            const int fd = int(syscall(SYS_memfd_create, "hc09-physical", 0));
            if (fd >= 0)
            {
                if (ftruncate(fd, off_t(kPhysicalSize)) == 0)
                    return fd;
                close(fd);
            }
#endif
#if defined(__ANDROID__)
            return ASharedMemory_create("hc09-physical", kPhysicalSize);
#else
            return -1;
#endif
        }
    }

    bool AllocateGuestMemory()
    {
        void* reserved = mmap(nullptr, PPC_MEMORY_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (reserved == MAP_FAILED)
        {
            std::fprintf(stderr, "[memory] reserving 4 GiB failed (%s)\n", std::strerror(errno));
            return false;
        }
        auto* base = static_cast<uint8_t*>(reserved);

        // Ordinary guest memory: everything below the physical windows.
        if (mmap(base, kPhysicalViews[0], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) ==
            MAP_FAILED)
        {
            std::fprintf(stderr, "[memory] mapping low guest memory failed (%s)\n", std::strerror(errno));
            return false;
        }

        const int physical = CreatePhysicalMemory();
        if (physical < 0)
        {
            std::fprintf(stderr, "[memory] creating 512 MiB of physical memory failed (%s)\n", std::strerror(errno));
            return false;
        }
        // HC09_NO_E_WINDOW=1 leaves 0xE0000000 unmapped, so any use of it faults (debugging).
        const bool skipEWindow = std::getenv("HC09_NO_E_WINDOW") != nullptr;
        for (uint64_t view : kPhysicalViews)
        {
            if (skipEWindow && view == 0xE0000000)
                continue;
            if (mmap(base + view, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_NORESERVE, physical, 0) ==
                MAP_FAILED)
            {
                std::fprintf(stderr, "[memory] mapping physical view at 0x%llX failed (%s)\n", (unsigned long long)view,
                    std::strerror(errno));
                close(physical);
                return false;
            }
        }
        close(physical);  // the mappings keep the memory alive
        g_base = base;
        return true;
    }
}
