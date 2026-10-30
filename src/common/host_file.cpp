#include "common/host_file.h"

#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hc09::host
{
    FileHandle::~FileHandle()
    {
        Close();
    }

    FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_)
    {
        other.handle_ = -1;
    }

    FileHandle& FileHandle::operator=(FileHandle&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            handle_ = other.handle_;
            other.handle_ = -1;
        }
        return *this;
    }

    bool FileHandle::IsOpen() const
    {
        return handle_ != -1;
    }

#ifdef _WIN32
    namespace
    {
        HANDLE Native(intptr_t handle)
        {
            return reinterpret_cast<HANDLE>(handle);
        }

        uint64_t FileTimeValue(const FILETIME& ft)
        {
            return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        }
    }

    FileHandle FileHandle::Open(const std::filesystem::path& path, bool write, Creation creation, bool randomAccess,
        OpenError* error)
    {
        DWORD disposition = OPEN_EXISTING;
        switch (creation)
        {
        case Creation::OpenExisting: disposition = OPEN_EXISTING; break;
        case Creation::CreateNew: disposition = CREATE_NEW; break;
        case Creation::OpenAlways: disposition = OPEN_ALWAYS; break;
        case Creation::TruncateExisting: disposition = TRUNCATE_EXISTING; break;
        case Creation::CreateAlways: disposition = CREATE_ALWAYS; break;
        }
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
            FILE_SHARE_READ | (randomAccess ? 0 : FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, disposition,
            FILE_ATTRIBUTE_NORMAL | (randomAccess ? FILE_FLAG_RANDOM_ACCESS : 0), nullptr);
        FileHandle file;
        if (handle == INVALID_HANDLE_VALUE)
        {
            if (error)
                *error = GetLastError() == ERROR_PATH_NOT_FOUND ? OpenError::PathNotFound : OpenError::Other;
            return file;
        }
        file.handle_ = reinterpret_cast<intptr_t>(handle);
        if (error)
            *error = OpenError::None;
        return file;
    }

    void FileHandle::Close()
    {
        if (handle_ != -1)
            CloseHandle(Native(handle_));
        handle_ = -1;
    }

    uint32_t FileHandle::ReadAt(uint64_t offset, void* buffer, uint32_t length) const
    {
        OVERLAPPED overlapped{};
        overlapped.Offset = DWORD(offset);
        overlapped.OffsetHigh = DWORD(offset >> 32);
        DWORD read = 0;
        return ReadFile(Native(handle_), buffer, length, &read, &overlapped) ? read : 0;
    }

    uint32_t FileHandle::WriteAt(uint64_t offset, const void* buffer, uint32_t length) const
    {
        OVERLAPPED overlapped{};
        overlapped.Offset = DWORD(offset);
        overlapped.OffsetHigh = DWORD(offset >> 32);
        DWORD written = 0;
        return WriteFile(Native(handle_), buffer, length, &written, &overlapped) ? written : 0;
    }

    bool FileHandle::SetSize(uint64_t size) const
    {
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = LONGLONG(size);
        return SetFileInformationByHandle(Native(handle_), FileEndOfFileInfo, &info, sizeof(info)) != 0;
    }

    FileStat Stat(const std::filesystem::path& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        FileStat stat;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return stat;
        stat.exists = true;
        stat.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        stat.size = stat.directory ? 0 : (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        stat.creationTime = FileTimeValue(data.ftCreationTime);
        stat.writeTime = FileTimeValue(data.ftLastWriteTime);
        return stat;
    }

    FILE* OpenStdioFile(const std::filesystem::path& path, const char* mode)
    {
        const std::wstring wideMode(mode, mode + std::char_traits<char>::length(mode));
        return _wfopen(path.c_str(), wideMode.c_str());
    }
#else
    namespace
    {
        // Days from 1601-01-01 to 1970-01-01, in FILETIME units.
        constexpr uint64_t kUnixEpoch = 116'444'736'000'000'000ull;

        uint64_t FileTimeValue(const timespec& time)
        {
            return kUnixEpoch + uint64_t(time.tv_sec) * 10'000'000 + uint64_t(time.tv_nsec) / 100;
        }
    }

    FileHandle FileHandle::Open(const std::filesystem::path& path, bool write, Creation creation, bool /*randomAccess*/,
        OpenError* error)
    {
        int flags = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC;
        switch (creation)
        {
        case Creation::OpenExisting: break;
        case Creation::CreateNew: flags |= O_CREAT | O_EXCL; break;
        case Creation::OpenAlways: flags |= O_CREAT; break;
        case Creation::TruncateExisting: flags |= O_TRUNC; break;
        case Creation::CreateAlways: flags |= O_CREAT | O_TRUNC; break;
        }
        // Creating or truncating needs write access even when the caller only asked to read.
        if ((flags & (O_CREAT | O_TRUNC)) && !write)
            flags = (flags & ~O_RDONLY) | O_RDWR;
        FileHandle file;
        const int fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0)
        {
            if (error)
                *error = errno == ENOENT && !std::filesystem::exists(path.parent_path()) ? OpenError::PathNotFound : OpenError::Other;
            return file;
        }
        file.handle_ = fd;
        if (error)
            *error = OpenError::None;
        return file;
    }

    void FileHandle::Close()
    {
        if (handle_ != -1)
            ::close(int(handle_));
        handle_ = -1;
    }

    uint32_t FileHandle::ReadAt(uint64_t offset, void* buffer, uint32_t length) const
    {
        uint32_t total = 0;
        while (total < length)
        {
            const ssize_t n = ::pread(int(handle_), static_cast<uint8_t*>(buffer) + total, length - total, off_t(offset + total));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            total += uint32_t(n);
        }
        return total;
    }

    uint32_t FileHandle::WriteAt(uint64_t offset, const void* buffer, uint32_t length) const
    {
        uint32_t total = 0;
        while (total < length)
        {
            const ssize_t n = ::pwrite(int(handle_), static_cast<const uint8_t*>(buffer) + total, length - total, off_t(offset + total));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            total += uint32_t(n);
        }
        return total;
    }

    bool FileHandle::SetSize(uint64_t size) const
    {
        return ::ftruncate(int(handle_), off_t(size)) == 0;
    }

    FileStat Stat(const std::filesystem::path& path)
    {
        struct stat data{};
        FileStat stat;
        if (::stat(path.c_str(), &data) != 0)
            return stat;
        stat.exists = true;
        stat.directory = S_ISDIR(data.st_mode);
        stat.size = stat.directory ? 0 : uint64_t(data.st_size);
        // POSIX keeps no creation time; the last status change is the closest.
        stat.creationTime = FileTimeValue(data.st_ctim);
        stat.writeTime = FileTimeValue(data.st_mtim);
        return stat;
    }

    FILE* OpenStdioFile(const std::filesystem::path& path, const char* mode)
    {
        return std::fopen(path.c_str(), mode);
    }
#endif
}
