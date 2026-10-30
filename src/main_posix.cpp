// Host entry point on POSIX hosts (the Android headless build for now): same start-up as main.cpp
// without the Windows-only parts (window, message boxes, update check, project-folder search).
//
//   hc09 [image.pe] [default.xex] [disc.iso] [userdata folder]
//
// Defaults are the developer layout (build/default.pe, build/extracted/default.xex,
// iso/HC09.iso, build/userdata) relative to the current directory.

#include <ppc_recomp_shared.h>

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "memory.h"
#include "common/edition.h"
#include "debug/stack_dump.h"
#include "kernel/guest.h"
#include "kernel/heap.h"
#include "kernel/threads.h"
#include "kernel/variables.h"
#include "kernel/vfs.h"
#include "kernel/xex.h"

namespace hc09
{
    uint8_t* g_base = nullptr;

    void ReportStubCall(const char* name, const char* library, const PPCContext& ctx, bool& reported)
    {
        if (reported)
            return;
        reported = true;
        std::fprintf(stderr, "[stub] %s (%s) called from 0x%08X\n", name, library, uint32_t(ctx.lr));
    }
}

namespace
{
    constexpr uint32_t kEntryPoint = 0x828A9CB0;
    constexpr uint32_t kMainStackSize = 0x100000;  // XEX asks for 0x20000; leave headroom

    bool LoadImage(const char* path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        const auto size = size_t(file.tellg());
        if (size != PPC_IMAGE_SIZE)
        {
            std::fprintf(stderr, "%s: expected 0x%llX bytes, got 0x%zX\n", path, PPC_IMAGE_SIZE, size);
            return false;
        }
        file.seekg(0);
        file.read(reinterpret_cast<char*>(hc09::g_base + PPC_IMAGE_BASE), std::streamsize(size));
        return bool(file);
    }

    // Target of every indirect call to code the recompiler did not emit as a function.
    PPC_FUNC(MissingFunction)
    {
        (void)base;
        hc09::GuestFatal(ctx, "indirect call to 0x%08X, which is not a recompiled function (add it to config/HC09.base.toml)",
            ctx.ctr.u32);
    }

    void PopulateFunctionTable()
    {
        for (uint64_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
            PPC_LOOKUP_FUNC(hc09::g_base, address) = MissingFunction;
        for (auto* mapping = PPCFuncMappings; mapping->host != nullptr; ++mapping)
            PPC_LOOKUP_FUNC(hc09::g_base, mapping->guest) = mapping->host;
    }

    // Faults: name the guest address when it is one, print the stack once, then let the default
    // action end the process (and leave a tombstone on Android).
    void ReportFault(int signal, siginfo_t* info, void* context)
    {
        static std::atomic<bool> reported{ false };
        if (!reported.exchange(true))
        {
            const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
            const auto guest = reinterpret_cast<uintptr_t>(hc09::g_base);
            if (signal == SIGSEGV || signal == SIGBUS)
            {
                if (address >= guest && address < guest + PPC_MEMORY_SIZE)
                    std::fprintf(stderr, "[fault] access to guest address 0x%08llX\n", (unsigned long long)(address - guest));
                else
                    std::fprintf(stderr, "[fault] access to host address %p\n", info->si_addr);
            }
            else if (signal == SIGTRAP)
            {
                std::fprintf(stderr, "[fault] guest trap instruction (assert/debug break)\n");
            }
            else
            {
                std::fprintf(stderr, "[fault] signal %d at %p\n", signal, info->si_addr);
            }
            hc09::debug::PrintStackFromContext(context);
        }
        ::signal(signal, SIG_DFL);
        ::raise(signal);
    }

    void InstallFaultHandlers()
    {
        struct sigaction action{};
        action.sa_sigaction = ReportFault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (int signal : { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE })
            sigaction(signal, &action, nullptr);
    }

    int StartupFailure(const char* message)
    {
        std::fprintf(stderr, "%s\n", message);
        return EXIT_FAILURE;
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::fprintf(stderr, "[boot] %s recomp " HC09_VERSION " (posix)\n", hc09::edition::kName);
    const char* imagePath = argc > 1 ? argv[1] : "build/default.pe";
    const char* xexPath = argc > 2 ? argv[2] : "build/extracted/default.xex";
    const std::filesystem::path isoPath = argc > 3 ? std::filesystem::path(argv[3]) : std::filesystem::path("iso/HC09.iso");
    const char* userData = argc > 4 ? argv[4] : std::getenv("HC09_USERDATA") ? std::getenv("HC09_USERDATA") : hc09::edition::kUserData;
    char message[512];

    if (!hc09::AllocateGuestMemory())
        return StartupFailure("Failed to reserve the 4 GiB guest address space.");
    if (!LoadImage(imagePath))
    {
        std::snprintf(message, sizeof(message), "Failed to load %s (decrypt default.xex with tools/xexdecrypt.py).", imagePath);
        return StartupFailure(message);
    }
    if (!hc09::LoadXexHeader(xexPath))
    {
        std::snprintf(message, sizeof(message), "Failed to load the XEX header from %s.", xexPath);
        return StartupFailure(message);
    }
    if (!hc09::InitializeVfs(isoPath, userData))
    {
        std::snprintf(message, sizeof(message), "Failed to open the game disc image %s.", isoPath.string().c_str());
        return StartupFailure(message);
    }
    if (std::string error; !hc09::edition::Apply(error))
        return StartupFailure(error.c_str());
    PopulateFunctionTable();
    hc09::PatchImportSlots();
    InstallFaultHandlers();
    hc09::debug::StartWatchdogFromEnvironment();

    PPCContext ctx{};
    hc09::EnterGuestThread(ctx, hc09::CreateGuestThread(kMainStackSize));

    std::fprintf(stderr, "[boot] entering 0x%08X\n", kEntryPoint);
    uint8_t* base = hc09::g_base;  // PPC_CALL_INDIRECT_FUNC expects `ctx` and `base`
    PPC_CALL_INDIRECT_FUNC(kEntryPoint);
    std::fprintf(stderr, "[boot] entry point returned\n");
    return EXIT_SUCCESS;
}
