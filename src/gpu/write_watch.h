#pragma once

// Notices the title's writes to guest physical memory (Android), so the renderer can keep data it
// converted from that memory (vertices, indices, textures) without hashing it every frame.
//
// Watched pages are write-protected in each of the three windows that alias physical memory (see
// memory_posix.cpp). The title's first write to one raises SIGSEGV; the handler lifts the protection
// and stamps the page with a new clock value. A user takes Now(), calls Watch() on its range, then
// reads the memory; later, WrittenSince(range, that stamp) says whether anything in it was written.
// File reads into physical memory go through a host buffer (xboxkrnl_io.cpp), since the kernel cannot
// write a protected page.
//
// Off by default; HC09_WRITE_WATCH=1 turns it on (users otherwise hash, as before). Known problem with it on:
// the menus' 3D background and some textures drop out at times (see docs ANDROID-PERF-HANDOFF.md).

#include <cstdint>

namespace hc09::gpu::write_watch
{
    // False when off or not set up: users fall back to hashing.
    bool Enabled();

    // Whether [address, address + length) (guest addresses) can be watched: physical memory only.
    bool Watchable(uint32_t address, uint32_t length);

    uint32_t Now();

    // Write-protects the range's pages that are not watched yet.
    void Watch(uint32_t address, uint32_t length);

    // True when a page of the range was written after `stamp` (a Now() taken before Watch).
    bool WrittenSince(uint32_t address, uint32_t length, uint32_t stamp);

    // HC09_GPU_STATS: write faults handled since the last call.
    uint64_t TakeFaults();
}
