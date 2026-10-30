#pragma once

// Host file access, one implementation per OS (host_file.cpp): positional reads and writes on an
// open file, and file metadata. Times are FILETIME units (see common/host_time.h).

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace hc09::host
{
    enum class Creation
    {
        OpenExisting,      // must exist
        CreateNew,         // must not exist
        OpenAlways,        // open, or create if missing
        TruncateExisting,  // must exist, emptied
        CreateAlways,      // create, or empty if it exists
    };

    enum class OpenError
    {
        None,
        PathNotFound,  // a parent directory is missing
        Other,
    };

    class FileHandle
    {
    public:
        FileHandle() = default;
        ~FileHandle();
        FileHandle(FileHandle&& other) noexcept;
        FileHandle& operator=(FileHandle&& other) noexcept;
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;

        // `randomAccess` hints that reads jump around (the disc image).
        static FileHandle Open(const std::filesystem::path& path, bool write, Creation creation, bool randomAccess,
            OpenError* error = nullptr);

        bool IsOpen() const;
        void Close();
        // Bytes transferred; 0 on error or end of file.
        uint32_t ReadAt(uint64_t offset, void* buffer, uint32_t length) const;
        uint32_t WriteAt(uint64_t offset, const void* buffer, uint32_t length) const;
        bool SetSize(uint64_t size) const;

    private:
        intptr_t handle_ = -1;
    };

    struct FileStat
    {
        bool exists = false;
        bool directory = false;
        uint64_t size = 0;
        uint64_t creationTime = 0;
        uint64_t writeTime = 0;
    };
    FileStat Stat(const std::filesystem::path& path);

    // fopen with a path that may hold non-ASCII characters on every host.
    FILE* OpenStdioFile(const std::filesystem::path& path, const char* mode);
}
