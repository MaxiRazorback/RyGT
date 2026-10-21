#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/Time.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::_wopen(p.wstring().c_str(), nativeFlags, static_cast<int>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::_lseeki64(fd, offset, whence);
}
static int NativeRead(int fd, void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelRead: nbytes exceeds platform limit");
    }
    return ::_read(fd, buf, static_cast<unsigned int>(n));
}
static int NativeWrite(int fd, const void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelWrite: nbytes exceeds platform limit");
    }
    return ::_write(fd, buf, static_cast<unsigned int>(n));
}
static int NativeClose(int fd) { return ::_close(fd); }
static int NativeUnlink(const std::filesystem::path& p) {
    return ::_wunlink(p.wstring().c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= _O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= _O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= _O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= _O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= _O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= _O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= _O_EXCL;
    f |= _O_BINARY;
    return f;
}
#else
#include <fcntl.h>
#include <unistd.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::open(p.c_str(), nativeFlags, static_cast<mode_t>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::lseek(fd, static_cast<off_t>(offset), whence);
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) {
    return ::read(fd, buf, n);
}
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) {
    return ::write(fd, buf, n);
}
static int NativeClose(int fd) { return ::close(fd); }
static int NativeUnlink(const std::filesystem::path& p) {
    return ::unlink(p.c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= O_EXCL;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_DIRECTORY) f |= O_DIRECTORY;
    return f;
}
#endif

void PrepareGuestWrite(void* buffer, std::size_t bytes) {
#ifdef _WIN32
    using RangeFunction = void (*)(std::uint64_t, std::uint64_t);
    static RangeFunction invalidate = nullptr;
    if (invalidate == nullptr) {
        if (HMODULE renderer = GetModuleHandleA("libSceVideoOut.prx")) {
            invalidate = reinterpret_cast<RangeFunction>(reinterpret_cast<void*>(GetProcAddress(renderer, "RytyGpuInvalidateMemory")));
        }
    }
    if (invalidate != nullptr && buffer != nullptr && bytes != 0) invalidate(reinterpret_cast<std::uint64_t>(buffer), bytes);
#else
    (void)buffer;
    (void)bytes;
#endif
}

std::int64_t ReadIntoGuest(void* buffer, std::size_t bytes, const std::function<std::int64_t(void*)>& read) {
    PrepareGuestWrite(buffer, bytes);
    const std::int64_t n = read(buffer);
    if (n >= 0 || buffer == nullptr || bytes == 0) return n;
    const int error = errno;
    std::vector<char> bounce(bytes);
    const std::int64_t retried = read(bounce.data());
    if (retried < 0) {
        errno = error;
        return n;
    }
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1) < 10) {
        std::fprintf(stderr, "[file] read into %p (%zu bytes) failed (errno %d); read again through a host buffer\n",
                     buffer, bytes, error);
    }
    std::memcpy(buffer, bounce.data(), static_cast<std::size_t>(retried));
    return retried;
}

static int SceErrorFromErrno(int error) {
    constexpr int GuestEio = 5;
    const int guest = error > 0 && error <= 34 ? error : GuestEio;
    return static_cast<int>(0x80020000u | static_cast<unsigned>(guest));
}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    APS5_LOG_TRACE("path=%s flags=0x%X nativeFlags=0x%X mode=0%o", path, flags, MapFlags(flags), mode);
    auto native = ResolvePath_nid_no_patch(path);
    int fd = NativeOpen(native, MapFlags(flags), mode);
#ifdef _WIN32
    if (fd < 0 && errno != ENOENT) {
        std::error_code error;
        if (std::filesystem::is_directory(native, error)) fd = File::OpenDirectoryDescriptor(native);
    }
#endif
    if (fd < 0) {
        return SceErrorFromErrno(errno);
    }
    return fd;
}

int APS5_VABI sceKernelClose(int d) {
#ifdef _WIN32
    File::ForgetDirectoryDescriptor(d);
#endif
    if (NativeClose(d) != 0) {
        throw std::runtime_error(std::string(__func__) + ": close failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return 0;
}

std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes) {
    if (buf == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": buf is null");
    }
    KernelDiagScope diag(KernelDiagZone::FileRead, __builtin_return_address(0));
    auto n = ReadIntoGuest(buf, nbytes, [&](void* target) -> std::int64_t { return NativeRead(d, target, nbytes); });
    if (n < 0) {
        throw std::runtime_error(std::string(__func__) + ": read failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes) {
    if (buf == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": buf is null");
    }
    auto n = NativeWrite(d, buf, nbytes);
    if (n < 0) {
        throw std::runtime_error(std::string(__func__) + ": write failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (whence < 0 || whence > 2) {
        throw std::invalid_argument(std::string(__func__) + ": invalid whence=" + std::to_string(whence));
    }
    std::int64_t result = NativeLseek(d, offset, whence);
    if (result < 0) {
        throw std::runtime_error(std::string(__func__) + ": lseek failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return result;
}

// DIAG-164 (RyGT): GT Sport checks for app0\gt.idx at start; without it the game runs without its data
// and closes right after the splash screen with nothing else in the logs. The first 20 app0 files
// a title looks for and can't find are reported, with the folder that was searched.
static void ReportMissingAppFile(const char* path, const std::filesystem::path& native, const std::error_code& error) {
    static std::atomic<unsigned> reported{0};
    if (std::strncmp(path, "/app0/", 6) != 0 || reported.fetch_add(1) >= 20) return;
#ifdef _WIN32
    const auto wide = native.wstring();
    std::string text;
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
        text.resize(static_cast<std::size_t>(size - 1));
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, text.data(), size, nullptr, nullptr);
    }
#else
    const std::string text = native.string();
#endif
    std::fprintf(stderr, "[file] %s is missing (looked for %s%s%s)\n", path, text.c_str(), error ? ": " : "",
                 error ? error.message().c_str() : "");
}

int APS5_VABI sceKernelStat(const char* path, FileStat* sb) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    if (sb == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": sb is null");
    }
    const auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!std::filesystem::exists(native, error)) {
        ReportMissingAppFile(path, native, error);
        return SceErrorFromErrno(2);
    }
    File::FillFileStat(native, sb);
    return 0;
}

int APS5_VABI sceKernelUnlink(const char* path) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    auto native = ResolvePath_nid_no_patch(path);
    if (NativeUnlink(native) != 0) {
        return SceErrorFromErrno(errno);
    }
    return 0;
}

}
