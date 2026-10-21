#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <map>
#include <memory>
#include <limits>
#include <vector>
#include <stdexcept>
#include <string>
#include <system_error>

namespace GuestArena {

class WindowsMappings {
public:
    static WindowsMappings& Get() {
        static WindowsMappings mappings;
        return mappings;
    }

    bool Reserve(void* address, std::size_t bytes) {
        Lock lock(*this);
        return reserve(reinterpret_cast<std::uintptr_t>(address), bytes);
    }

    // Protect the free parts of a guest window before host drivers initialize.
    // Existing foreign mappings remain foreign and cannot be reset or replaced.
    bool ReserveAvailable(std::uintptr_t first, std::uintptr_t end) {
        Lock lock(*this);
        for (auto cursor = first; cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_FREE && !reserve(cursor, stop - cursor)) return false;
            cursor = stop;
        }
        return true;
    }
    // NVIDIA's Vulkan/CUDA interop needs low virtual addresses of its own.
    // Search both guest-owned placeholders and currently free host ranges.
    void* FindAndReserve(std::uintptr_t first, std::uintptr_t end, std::size_t bytes, std::size_t alignment) {
        Lock lock(*this);
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
        unsigned retries = 0;
        auto candidate = alignUp(first, alignment);
        while (candidate < end && bytes <= end - candidate) {
            const auto stop = candidate + bytes;
            auto cursor = candidate;
            while (cursor < stop) {
                const auto ownedEnd = reservationEnd(cursor);
                if (ownedEnd > cursor) {
                    cursor = std::min(stop, ownedEnd);
                    continue;
                }
                const auto memory = query(cursor);
                const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
                if (memory.State != MEM_FREE) {
                    candidate = alignUp(regionEnd, alignment);
                    break;
                }
                cursor = std::min(stop, regionEnd);
            }
            if (cursor < stop) continue;
            if (reserve(candidate, bytes)) return reinterpret_cast<void*>(candidate);
            if (++retries == 8) return nullptr;
            candidate = alignUp(candidate + allocationGranularity, alignment);
        }
        return nullptr;
    }

    void Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched) {
        Lock lock(*this);
        requireOwned(reinterpret_cast<std::uintptr_t>(address), bytes);
        const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
        for (auto cursor = reinterpret_cast<std::uintptr_t>(address); cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE) {
                const auto limit = std::min(end, cursor + granule);
                auto placeholderEnd = stop;
                while (placeholderEnd < limit) {
                    const auto next = query(placeholderEnd);
                    if (next.State != MEM_RESERVE) break;
                    placeholderEnd = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                }
                const auto size = std::min(limit, placeholderEnd) - cursor;
                reset(cursor, size);
                const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watched ? MEM_WRITE_WATCH : 0);
                if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(cursor), size, flags, protection, nullptr, 0)) fail("replace guest placeholder with private memory");
                cursor += size;
            } else {
                if (memory.State != MEM_COMMIT) throw std::runtime_error("guest memory is not committed");
                const auto mapped = views.find(cursor & ~(pageBytes - 1));
                if (mapped != views.end()) {
                    mapped->second.protection = protection;
                    mapped->second.armed = false;
                    invalidate(*mapped->second.page);
                }
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
                cursor = stop;
            }
        }
    }

    void Reset(void* address, std::size_t bytes) {
        Lock lock(*this);
        reset(reinterpret_cast<std::uintptr_t>(address), bytes);
    }

    void Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection) {
        Lock lock(*this);
        auto cursor = reinterpret_cast<std::uintptr_t>(address);
        reset(cursor, bytes);
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) fail("keep shared guest section");
        const auto owned = std::make_shared<Section>(duplicate);
        // RyGT: the whole range as one view (one placeholder replaced, one protection call). Mapping
        // 16 KB at a time took five or six Windows calls per page, and GT Sport maps hundreds of MB
        // while it streams a track: up to seconds of stalls on Nurburgring 24h. reset() splits a
        // view again when only part of it is released or remapped later.
        bool whole = false;
        if (bytes > pageBytes) {
            split(cursor, bytes);
            whole = map(section, GetCurrentProcess(), address, offset, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0) != nullptr;
            DWORD previous;
            if (whole && !VirtualProtect(address, bytes, protection, &previous)) fail("protect shared guest pages");
        }
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            void* page = reinterpret_cast<void*>(cursor + done);
            if (!whole) {
                split(cursor + done, pageBytes);
                if (!map(section, GetCurrentProcess(), page, offset + done, pageBytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest page");
                DWORD previous;
                if (!VirtualProtect(page, pageBytes, protection, &previous)) fail("protect shared guest page");
            }
            const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(section), offset + done);
            auto shared = physical[key].lock();
            if (!shared) {
                shared = std::make_shared<SharedPage>();
                physical[key] = shared;
            }
            const auto base = cursor + done;
            shared->aliases.push_back(base);
            views.emplace(base, View{shared, protection, 0, false, owned, offset + done});
            invalidate(*shared);
        }
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        Lock lock(*this);
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.armed = false;
            invalidate(*it->second.page);
        }
    }

    bool HandleWrite(std::uintptr_t address) {
        Lock lock(*this);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || !writable(found->second.protection)) return false;
        auto& view = found->second;
        invalidate(*view.page);
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(base), pageBytes, view.protection, &previous)) fail("resume shared memory write");
        view.armed = false;
        return true;
    }

    /// RyGT: a fault in guest memory while another thread remaps it. reset() and splitView() unmap
    /// a view and map the parts that stay, so for a moment those pages are not there, and a thread
    /// that reads one then (the renderer's shader translation reading shader code, for instance)
    /// faults. Under Wine these moments last far longer than on Windows. This waits for the remap
    /// to finish; true when the page is back and allows the access, so the access can be retried.
    bool AwaitRemap(std::uintptr_t address, bool write) {
        if (owner.load(std::memory_order_acquire) == GetCurrentThreadId()) return false;
        const auto deadline = GetTickCount64() + 5000;
        while (!mutex.try_lock()) {
            if (GetTickCount64() > deadline) return false;
            SwitchToThread();
        }
        std::lock_guard lock(mutex, std::adopt_lock);
        if (reservationEnd(address) <= address) return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) return false;
        if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const DWORD protection = memory.Protect & 0xff;
        if (!write) return true;
        return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE ||
               protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_WRITECOPY;
    }

    void* MapAlias(std::uintptr_t address, std::size_t bytes) {
        Lock lock(*this);
        const auto refuse = [&](const char* reason) {
            char text[192];
            std::snprintf(text, sizeof(text), "read-write alias of shared guest memory 0x%llx+0x%llx: %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), reason);
            return std::runtime_error(text);
        };
        if (address % pageBytes != 0 || bytes % pageBytes != 0 || bytes == 0) throw refuse("the range is not made of whole shared pages");
        auto view = views.find(address);
        if (view == views.end()) throw refuse("the range does not start at a shared view");
        const auto section = view->second.section;
        const auto offset = view->second.offset;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        for (std::size_t done = 0; done < bytes; done += pageBytes, ++view) {
            if (view == views.end() || view->first != address + done || view->second.offset != offset + done) throw refuse("the range is not one contiguous run of views of a section");
            if (view->second.section != section && !sameSection(view->second.section->handle, section->handle)) throw refuse("the range spans several sections");
        }
        const auto lead = offset % system.dwAllocationGranularity;
        void* alias = map(section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
        if (alias == nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "MapViewOfFile3 of a read-write alias of shared guest memory 0x%llx+0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), text);
        }
        return static_cast<char*>(alias) + lead;
    }

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto base = reinterpret_cast<std::uintptr_t>(alias) & ~(static_cast<std::uintptr_t>(system.dwAllocationGranularity) - 1);
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), 0)) fail("unmap shared guest alias");
    }

    bool Protection(std::uintptr_t address, std::uint32_t* protection) {
        Lock lock(*this);
        const auto found = views.find(address & ~(pageBytes - 1));
        if (found == views.end()) return false;
        *protection = found->second.protection;
        return true;
    }

    bool Collect(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
        Lock lock(*this);
        const auto capacity = *count;
        *count = 0;
        const auto end = address + bytes;
        for (auto cursor = address; cursor < end;) {
            const auto nextClean = cleanRanges.upper_bound(cursor);
            if (nextClean != cleanRanges.begin()) {
                const auto clean = std::prev(nextClean);
                if (cursor < clean->second) {
                    cursor = std::min(end, clean->second);
                    continue;
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            const auto found = views.find(base);
            if (found != views.end()) {
                auto& view = found->second;
                const auto stop = std::min(end, base + pageBytes);
                if (view.protection == PAGE_NOACCESS) return false;
                if (view.seen != view.page->generation) {
                    const auto needed = (stop - cursor + 4095) / 4096;
                    if (needed > capacity - *count) {
                        for (auto at = cursor; *count < capacity; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                        return true;
                    }
                    for (auto at = cursor; at < stop; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                }
                if (clear) {
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        if (!writable(other.protection) || other.armed) continue;
                        DWORD previous;
                        const DWORD protection = other.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
                        if (!VirtualProtect(reinterpret_cast<void*>(alias), pageBytes, protection, &previous)) fail("arm shared memory write tracking");
                        other.armed = true;
                    }
                    view.seen = view.page->generation;
                    rememberClean(base, base + pageBytes);
                }
                cursor = stop;
            } else {
                const auto memory = query(cursor);
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                ULONG_PTR available = capacity - *count;
                if (available == 0) return true;
                DWORD granularity = 0;
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), stop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                *count += available;
                if (*count == capacity) return true;
                cursor = stop;
            }
        }
        return true;
    }

private:
    static constexpr std::size_t pageBytes = 0x4000;
    struct SharedPage {
        std::uint64_t generation = 1;
        std::vector<std::uintptr_t> aliases;
    };
    struct Section {
        HANDLE handle;
        explicit Section(HANDLE handle) : handle(handle) {}
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        ~Section() { CloseHandle(handle); }
    };
    struct View {
        std::shared_ptr<SharedPage> page;
        DWORD protection;
        std::uint64_t seen;
        bool armed;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
    };
    void forgetClean(std::uintptr_t start, std::uintptr_t end) {
        auto it = cleanRanges.lower_bound(start);
        if (it != cleanRanges.begin() && std::prev(it)->second > start) --it;
        while (it != cleanRanges.end() && it->first < end) {
            const auto first = it->first;
            const auto last = it->second;
            it = cleanRanges.erase(it);
            if (first < start) cleanRanges.emplace(first, start);
            if (last > end) it = cleanRanges.emplace(end, last).first;
        }
    }

    void rememberClean(std::uintptr_t start, std::uintptr_t end) {
        auto it = cleanRanges.lower_bound(start);
        if (it != cleanRanges.begin() && std::prev(it)->second >= start) --it;
        while (it != cleanRanges.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = cleanRanges.erase(it);
        }
        cleanRanges.emplace(start, end);
    }

    void invalidate(SharedPage& page) {
        ++page.generation;
        for (const auto alias : page.aliases) forgetClean(alias, alias + pageBytes);
    }

    bool sameSection(HANDLE first, HANDLE second) const {
        return compare != nullptr && compare(first, second);
    }

    static bool writable(DWORD protection) {
        return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
    }
    using AllocateFunction = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapFunction = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using UnmapFunction = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);
    using CompareFunction = BOOL (WINAPI*)(HANDLE, HANDLE);

    WindowsMappings() {
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        if (!module) fail("load Windows memory API");
        allocate = reinterpret_cast<AllocateFunction>(GetProcAddress(module, "VirtualAlloc2"));
        map = reinterpret_cast<MapFunction>(GetProcAddress(module, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapFunction>(GetProcAddress(module, "UnmapViewOfFile2"));
        if (!allocate || !map || !unmap) throw std::runtime_error("Windows placeholder memory APIs are required");
        compare = reinterpret_cast<CompareFunction>(GetProcAddress(module, "CompareObjectHandles"));
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        allocationGranularity = system.dwAllocationGranularity;
    }

    static std::uintptr_t alignUp(std::uintptr_t value, std::size_t alignment) {
        if (value > std::numeric_limits<std::uintptr_t>::max() - (alignment - 1)) return std::numeric_limits<std::uintptr_t>::max();
        return (value + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    }

    std::uintptr_t reservationEnd(std::uintptr_t address) const {
        auto it = reservations.upper_bound(address);
        if (it == reservations.begin()) return address;
        --it;
        return it->second > address ? it->second : address;
    }

    bool reserve(std::uintptr_t address, std::size_t bytes) {
        if (bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
        const auto first = address & ~(static_cast<std::uintptr_t>(allocationGranularity) - 1);
        const auto end = alignUp(address + bytes, allocationGranularity);
        if (end == std::numeric_limits<std::uintptr_t>::max()) return false;
        // A driver's MEM_RESERVE allocation is not a replaceable guest placeholder.
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> added;
        for (auto cursor = first; cursor < end;) {
            const auto ownedEnd = reservationEnd(cursor);
            if (ownedEnd > cursor) {
                cursor = std::min(end, ownedEnd);
                continue;
            }
            const auto next = reservations.lower_bound(cursor);
            const auto stop = next == reservations.end() ? end : std::min(end, next->first);
            if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(cursor), stop - cursor,
                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0)) {
                for (const auto& [base, limit] : added) VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
                return false;
            }
            added.emplace_back(cursor, stop);
            cursor = stop;
        }
        for (const auto& range : added) reservations.emplace(range);
        return true;
    }

    void requireOwned(std::uintptr_t address, std::size_t bytes) const {
        if (bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) throw std::invalid_argument("invalid guest mapping range");
        const auto end = address + bytes;
        while (address < end) {
            const auto next = reservationEnd(address);
            if (next == address) throw std::runtime_error("guest mapping overlaps host address space");
            address = std::min(end, next);
        }
    }

    // The mutex, remembering which thread holds it (AwaitRemap must not wait for itself).
    struct Lock {
        explicit Lock(WindowsMappings& mappings) : mappings(mappings) {
            mappings.mutex.lock();
            mappings.owner.store(GetCurrentThreadId(), std::memory_order_release);
        }
        ~Lock() {
            mappings.owner.store(0, std::memory_order_release);
            mappings.mutex.unlock();
        }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        WindowsMappings& mappings;
    };

    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }

    static MEMORY_BASIC_INFORMATION query(std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) fail("query guest memory");
        return memory;
    }

    static void split(std::uintptr_t address, std::size_t bytes) {
        auto memory = query(address);
        memory = query(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
        if (memory.State != MEM_RESERVE) throw std::runtime_error("guest mapping requires a placeholder");
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address != base) {
            if (!VirtualFree(reinterpret_cast<void*>(base), address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder prefix");
            memory = query(address);
        }
        if (memory.RegionSize < bytes) throw std::runtime_error("guest placeholder is too small");
        if (memory.RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder suffix");
    }

    void reset(std::uintptr_t address, std::size_t bytes) {
        requireOwned(address, bytes);
        const auto end = address + bytes;
        forgetClean(address, end);
        for (auto cursor = address; cursor < end;) {
            const auto memory = query(cursor);
            if (memory.State == MEM_RESERVE) {
                cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                continue;
            }
            if (memory.Type == MEM_MAPPED && splitView(cursor, end)) continue;
            if (reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != cursor) throw std::runtime_error("cannot release part of a host allocation");
            auto allocationEnd = cursor;
            do {
                const auto part = query(allocationEnd);
                if (part.AllocationBase != memory.AllocationBase) break;
                allocationEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
            } while (allocationEnd < end);
            if (allocationEnd > end || query(allocationEnd).AllocationBase == memory.AllocationBase) throw std::runtime_error("guest release truncates a host allocation");
            if (memory.Type == MEM_MAPPED) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(cursor), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest page");
                for (auto page = cursor; page < allocationEnd; page += pageBytes) {
                    const auto found = views.find(page);
                    if (found != views.end()) {
                        std::erase(found->second.page->aliases, page);
                        views.erase(found);
                    }
                }
            } else if (memory.Type == MEM_PRIVATE) {
                if (!VirtualFree(reinterpret_cast<void*>(cursor), allocationEnd - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory");
            } else {
                throw std::runtime_error("unsupported guest mapping type");
            }
            cursor = allocationEnd;
        }
        const auto last = query(reinterpret_cast<std::uintptr_t>(query(end - 1).AllocationBase));
        const auto lastBase = reinterpret_cast<std::uintptr_t>(last.BaseAddress);
        if (lastBase + last.RegionSize > end) split(lastBase, end - lastBase);
        const auto first = query(address);
        split(address, std::min(bytes, reinterpret_cast<std::uintptr_t>(first.BaseAddress) + first.RegionSize - address));
        if (query(address).RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) fail("coalesce guest placeholders");
    }

    /// RyGT: a view that `[start, end)` releases only part of (it starts before `start` or ends after
    /// `end`) is unmapped whole and the parts of it outside the range are mapped again, page
    /// bookkeeping and protections included. False when the view lies inside the range.
    bool splitView(std::uintptr_t start, std::uintptr_t end) {
        const auto memory = query(start);
        const auto viewBase = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
        auto viewEnd = viewBase;
        for (;;) {
            const auto part = query(viewEnd);
            if (part.AllocationBase != memory.AllocationBase || part.State == MEM_FREE) break;
            viewEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
        }
        if (viewBase >= start && viewEnd <= end) return false;
        std::vector<std::pair<std::uintptr_t, View>> keep;
        for (auto page = viewBase; page < viewEnd; page += pageBytes) {
            const auto found = views.find(page);
            if (found == views.end()) continue;
            if (page < start || page >= end) keep.emplace_back(page, found->second);
            std::erase(found->second.page->aliases, page);
            views.erase(found);
        }
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(viewBase), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest view");
        for (std::size_t i = 0; i < keep.size();) {
            // One view per run of consecutive pages of one section at consecutive offsets.
            std::size_t j = i + 1;
            while (j < keep.size() && keep[j].first == keep[j - 1].first + pageBytes &&
                   keep[j].second.section == keep[i].second.section &&
                   keep[j].second.offset == keep[j - 1].second.offset + pageBytes) {
                ++j;
            }
            const auto runBase = keep[i].first;
            const auto runBytes = (j - i) * pageBytes;
            split(runBase, runBytes);
            if (!map(keep[i].second.section->handle, GetCurrentProcess(), reinterpret_cast<void*>(runBase), keep[i].second.offset, runBytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest view again");
            // One protection call per run of pages that end up alike: per page, a remap of a large
            // view took thousands of calls, and the kept pages are missing until it is done.
            const auto effective = [](const View& view) {
                if (view.armed && writable(view.protection)) return view.protection == PAGE_EXECUTE_READWRITE ? DWORD{PAGE_EXECUTE_READ} : DWORD{PAGE_READONLY};
                return view.protection;
            };
            for (std::size_t k = i; k < j;) {
                const DWORD protection = effective(keep[k].second);
                std::size_t l = k + 1;
                while (l < j && effective(keep[l].second) == protection) ++l;
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(keep[k].first), (l - k) * pageBytes, protection, &previous)) fail("protect shared guest page again");
                k = l;
            }
            for (std::size_t k = i; k < j; ++k) {
                auto& [page, view] = keep[k];
                view.page->aliases.push_back(page);
                views.emplace(page, view);
            }
            i = j;
        }
        return true;
    }

    std::map<std::uintptr_t, std::uintptr_t> reservations;
    std::size_t allocationGranularity = 0;
    std::map<std::uintptr_t, std::uintptr_t> cleanRanges;
    std::map<std::uintptr_t, View> views;
    std::map<std::pair<std::uintptr_t, std::uint64_t>, std::weak_ptr<SharedPage>> physical;
    std::mutex mutex;
    std::atomic<DWORD> owner{0};
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
};

}
#endif

#endif
