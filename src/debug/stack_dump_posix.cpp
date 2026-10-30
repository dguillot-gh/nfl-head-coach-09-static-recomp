// Stack dumps on POSIX hosts: the calling thread's stack, unwound with the C++ runtime's unwinder
// and named with dladdr (the executable is linked with --export-dynamic so recompiled functions
// show as sub_<guest address>). Dumping other threads and hardware watchpoints are Windows-only.

#include "debug/stack_dump.h"

#include <dlfcn.h>
#include <unwind.h>

#include <cstdio>
#include <cstdlib>

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
    }

    void RegisterThread(uint32_t /*guestThreadId*/) {}

    void DumpAllStacks()
    {
        std::fprintf(stderr, "[stack] dumping other threads is not supported on this host\n");
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
        if (std::getenv("HC09_DUMP_STACKS") || std::getenv("HC09_WATCH"))
            std::fprintf(stderr, "[stack] HC09_DUMP_STACKS and HC09_WATCH are Windows-only\n");
    }
}
