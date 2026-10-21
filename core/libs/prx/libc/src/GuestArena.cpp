#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/WindowsMappings.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace GuestArena {
namespace {

constexpr std::uintptr_t PreferredBase = 0x0000000200000000ull;
constexpr std::uintptr_t SystemReservedStart = 0x00000007FFFFC000ull;
constexpr std::uintptr_t SystemReservedEnd = 0x0000001000000000ull;
constexpr std::uintptr_t InteropStart = 0x000000E000000000ull;
constexpr std::uintptr_t InteropEnd = 0x000000F000000000ull;
constexpr std::uintptr_t MapAreaEnd = 0x000000FC00000000ull;
// Keep low virtual addresses available for NVIDIA interop. Leave a 64 GiB host window
// below the high fixed guest mappings (GT Sport uses 0xf3ffe00000). Protect the rest
// before loading drivers so unconstrained host reservations cannot steal guest pools.

std::uintptr_t alignUp(std::uintptr_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
}

bool WriteWatchRequested() {
    const char* enabled = std::getenv("APS5_WRITE_WATCH");
    return std::getenv("APS5_NO_WRITE_WATCH") == nullptr && enabled && std::strcmp(enabled, "1") == 0;
}

class Arena {
public:
    static Arena& Get() {
        static Arena arena;
        return arena;
    }

    bool Available() const {
        return _base != 0;
    }

    bool Contains(const void* pointer, std::size_t bytes) const {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return _base != 0 && address >= _base && bytes <= _end - _base && address - _base <= (_end - _base) - bytes;
    }

    void* Allocate(std::size_t bytes, std::size_t alignment) {
        if (_base == 0) throw std::runtime_error("guest address space arena is unavailable");
        if (bytes == 0) throw std::invalid_argument("invalid guest allocation size");
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::invalid_argument("invalid guest arena alignment");
        std::lock_guard lock(_lock);
        std::uintptr_t candidate = alignUp(_base, alignment);
        for (const auto& [start, end] : _used) {
            if (candidate < start && bytes <= start - candidate) {
#ifdef _WIN32
                if (void* reserved = WindowsMappings::Get().FindAndReserve(candidate, start, bytes, alignment)) {
                    const auto address = reinterpret_cast<std::uintptr_t>(reserved);
                    _used.emplace(address, address + bytes);
                    return reserved;
                }
#endif
            }
            candidate = std::max(candidate, alignUp(end, alignment));
        }
#ifdef _WIN32
        if (candidate < _end) {
            if (void* reserved = WindowsMappings::Get().FindAndReserve(candidate, _end, bytes, alignment)) {
                const auto address = reinterpret_cast<std::uintptr_t>(reserved);
                _used.emplace(address, address + bytes);
                return reserved;
            }
        }
#endif
        throw std::runtime_error("guest address space arena exhausted");
    }

    void MarkUsed(const void* pointer, std::size_t bytes) {
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        if (!Contains(pointer, bytes) || bytes == 0) throw std::invalid_argument("fixed guest mapping is outside the arena");
        const auto next = _used.lower_bound(start);
        if ((next != _used.end() && next->first < start + bytes) || (next != _used.begin() && std::prev(next)->second > start))
            throw std::runtime_error("fixed guest mapping overlaps a guest arena range");
#ifdef _WIN32
        if (!WindowsMappings::Get().Reserve(const_cast<void*>(pointer), bytes)) {
            char message[160];
            std::snprintf(message, sizeof(message), "fixed guest mapping overlaps host address space: 0x%llx+0x%llx (Windows error %lu)",
                static_cast<unsigned long long>(start), static_cast<unsigned long long>(bytes), GetLastError());
            throw std::runtime_error(message);
        }
#endif
        _used.emplace(start, start + bytes);
    }

    void Release(const void* pointer, std::size_t bytes) {
        std::lock_guard lock(_lock);
        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        const auto end = start + bytes;
        auto it = _used.upper_bound(start);
        if (it != _used.begin()) --it;
        while (it != _used.end() && it->first < end) {
            const auto rangeStart = it->first;
            const auto rangeEnd = it->second;
            if (rangeEnd <= start) {
                ++it;
                continue;
            }
            it = _used.erase(it);
            if (rangeStart < start) _used.emplace(rangeStart, start);
            if (rangeEnd > end) _used.emplace(end, rangeEnd);
        }
    }

private:
    Arena() {
#ifdef _WIN32
        _base = PreferredBase;
        _end = MapAreaEnd;
        _writeWatched = WriteWatchRequested();
        _used.emplace(SystemReservedStart, SystemReservedEnd);
        _used.emplace(InteropStart, InteropEnd);
        auto& mappings = WindowsMappings::Get();
        if (!mappings.ReserveAvailable(PreferredBase, InteropStart) ||
            !mappings.ReserveAvailable(InteropEnd, MapAreaEnd))
            throw std::runtime_error("cannot reserve guest mapping windows around host interop space");
#endif
    }

    std::mutex _lock;
    std::map<std::uintptr_t, std::uintptr_t> _used;
    std::uintptr_t _base = 0;
    std::uintptr_t _end = 0;
    bool _writeWatched = false;

public:
    std::uintptr_t Base() const { return _base; }
    std::size_t Size() const { return _end - _base; }
    bool WriteWatched() const { return _writeWatched; }
};

const bool g_reserved = (Arena::Get(), true);

}

bool GuestArenaAvailable_nid_postfix() {
    return Arena::Get().Available();
}

bool GuestArenaContains_nid_postfix(const void* pointer, std::size_t bytes) {
    return Arena::Get().Contains(pointer, bytes);
}

void* GuestArenaAllocate_nid_postfix(std::size_t bytes, std::size_t alignment) {
    return Arena::Get().Allocate(bytes, alignment);
}

void GuestArenaMarkUsed_nid_postfix(const void* pointer, std::size_t bytes) {
    Arena::Get().MarkUsed(pointer, bytes);
}

void GuestArenaRelease_nid_postfix(const void* pointer, std::size_t bytes) {
    Arena::Get().Release(pointer, bytes);
}

void GuestArenaRange_nid_postfix(std::uintptr_t* base, std::size_t* bytes) {
    *base = Arena::Get().Base();
    *bytes = Arena::Get().Size();
}

#ifdef _WIN32
void GuestArenaSetProtection_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint32_t protection) {
    WindowsMappings::Get().SetProtection(address, bytes, protection);
}

bool GuestArenaHandleWrite_nid_postfix(std::uintptr_t address) {
    return WindowsMappings::Get().HandleWrite(address);
}

bool GuestArenaAwaitRemap_nid_postfix(std::uintptr_t address, bool write) {
    return WindowsMappings::Get().AwaitRemap(address, write);
}

bool GuestArenaProtection_nid_postfix(std::uintptr_t address, std::uint32_t* protection) {
    return WindowsMappings::Get().Protection(address, protection);
}

bool GuestArenaCollectWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
    return WindowsMappings::Get().Collect(address, bytes, pages, count, clear);
}

void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule) {
    if (!Arena::Get().Contains(pointer, bytes)) throw std::invalid_argument("commit outside the guest arena");
    WindowsMappings::Get().Commit(pointer, bytes, protection, granule, Arena::Get().WriteWatched());
}

void GuestArenaReset_nid_postfix(void* pointer, std::size_t bytes) {
    if (!Arena::Get().Contains(pointer, bytes)) throw std::invalid_argument("reset outside the guest arena");
    WindowsMappings::Get().Reset(pointer, bytes);
}

void GuestArenaMap_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection) {
    if (!Arena::Get().Contains(pointer, bytes)) {
        char message[160];
        std::snprintf(message, sizeof(message), "shared mapping outside the guest arena: 0x%llx+0x%llx (arena 0x%llx+0x%llx)",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer)), static_cast<unsigned long long>(bytes),
            static_cast<unsigned long long>(Arena::Get().Base()), static_cast<unsigned long long>(Arena::Get().Size()));
        throw std::invalid_argument(message);
    }
    WindowsMappings::Get().Map(pointer, bytes, section, offset, protection);
}

// Called from other runtimes (the MSVC-built renderer), so no C++ exception may leave it: a
// range that cannot be aliased returns null.
void* GuestArenaMapAlias_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    try {
        return WindowsMappings::Get().MapAlias(address, bytes);
    } catch (const std::exception&) {
        return nullptr;
    }
}

void GuestArenaUnmapAlias_nid_postfix(void* alias) {
    WindowsMappings::Get().UnmapAlias(alias);
}
#endif

bool GuestArenaWriteWatched_nid_postfix() {
    return Arena::Get().WriteWatched();
}

}
