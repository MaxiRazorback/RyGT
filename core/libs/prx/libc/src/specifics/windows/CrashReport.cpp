#ifdef _WIN32
#include <windows.h>
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/LogFiles.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <typeinfo>
#include <cstring>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <utility>
#include <mutex>
#include "Sse4aEmulation.hpp"
#include <io.h>

namespace {

// RyGT: GTSport.exe is a Windows (GUI) program, so a double-click starts it without standard input,
// output or error. Then input reads NUL; stdout.log and stderr.log go under logs next to
// the executable, archiving previous sessions. The CRT descriptors 0-2 and Win32 handles follow,
// because the game's own
// writes reach fd 1 through sceKernelWrite (which failed with EBADF and threw) and crash reports
// write to the stderr handle. Runs when libc.prx loads, before the game starts.
void RedirectDetachedStream(DWORD id, FILE* stream, int fd, const char* name, const char* mode) {
    HANDLE handle = GetStdHandle(id);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) return;
    char path[MAX_PATH] = "NUL";
    if (std::strcmp(name, "NUL") != 0) {
        const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return;
        const auto log = RuntimeLogFiles::Prepare(std::filesystem::path(path).parent_path(), name, true);
        if (log.empty() || log.string().size() >= MAX_PATH) return;
        std::strcpy(path, log.string().c_str());
    }
    if (!std::freopen(path, mode, stream)) return;
    if (_fileno(stream) != fd) _dup2(_fileno(stream), fd);
    if (stream == stdout || stream == stderr) std::setvbuf(stream, nullptr, _IONBF, 0);
    SetStdHandle(id, reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
}

[[maybe_unused]] const bool g_detachedOutput = [] {
    // In order, so each stream gets the lowest free descriptor: its own.
    RedirectDetachedStream(STD_INPUT_HANDLE, stdin, 0, "NUL", "r");
    RedirectDetachedStream(STD_OUTPUT_HANDLE, stdout, 1, "stdout.log", "w");
    RedirectDetachedStream(STD_ERROR_HANDLE, stderr, 2, "stderr.log", "w");
    return true;
}();

// The reporter runs on a faulting thread that may itself hold the CRT stream lock (a fault inside
// printf), so it writes straight to the stderr handle.
void Report(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length <= 0) return;
    if (length > static_cast<int>(sizeof(buffer)) - 1) length = sizeof(buffer) - 1;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer, static_cast<DWORD>(length), &written, nullptr);
}

void DescribeAddress(std::uint64_t address, char* buffer, std::size_t size) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) && module) {
        GetModuleFileNameA(module, path, sizeof(path));
        const char* name = path;
        for (const char* cursor = path; *cursor; ++cursor) {
            if (*cursor == '\\' || *cursor == '/') name = cursor + 1;
        }
        std::snprintf(buffer, size, "0x%016llx %s+0x%llx", static_cast<unsigned long long>(address), name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)));
        return;
    }
    std::snprintf(buffer, size, "0x%016llx", static_cast<unsigned long long>(address));
}

bool IsExecutable(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)) return false;
    if (memory.State != MEM_COMMIT) return false;
    const DWORD protection = memory.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsReadable(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)) return false;
    return memory.State == MEM_COMMIT && (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

bool IsFatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_IN_PAGE_ERROR:
        return true;
    default:
        return false;
    }
}

// Debug aid: APS5_WATCH_WRITE=<guest ELF vaddr> reports every write to that 8-byte location by
// making its page read-only and single-stepping each trapped write.
std::uintptr_t g_watchAddress = 0;
std::uintptr_t g_watchPage = 0;
thread_local bool t_watchStepping = false;
thread_local bool t_watchHit = false;
thread_local unsigned long long t_watchBefore = 0;
thread_local std::uintptr_t t_watchFault = 0;
thread_local char t_watchWhere[MAX_PATH + 64];

void ProtectWatchPage(DWORD protection) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_watchPage), 0x1000, protection, &old);
}

bool HandleWatch(EXCEPTION_POINTERS* info) {
    if (g_watchPage == 0) return false;
    const auto* record = info->ExceptionRecord;
    auto* context = info->ContextRecord;
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watchStepping) {
        t_watchStepping = false;
        ProtectWatchPage(PAGE_READONLY);
        const auto current = *reinterpret_cast<const unsigned long long*>(g_watchAddress);
        if (t_watchHit || current != t_watchBefore) {
            if (!t_watchHit) Report("[watch] thread %lu changed the value with a write starting at 0x%llx from %s\n", GetCurrentThreadId(), static_cast<unsigned long long>(t_watchFault), t_watchWhere);
            Report("[watch]   new value 0x%016llx at %lld ms\n", current, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
            t_watchHit = false;
        }
        return true;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1) return false;
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    if (address < g_watchPage || address >= g_watchPage + 0x1000) return false;
    // Block writes (memset, memcpy) fault on their first byte, so compare the value after the step
    // as well as matching the fault address.
    t_watchBefore = *reinterpret_cast<const unsigned long long*>(g_watchAddress);
    t_watchFault = address;
    DescribeAddress(context->Rip, t_watchWhere, sizeof(t_watchWhere));
    if (address + 32 > g_watchAddress && address < g_watchAddress + 8) {
        char line[MAX_PATH + 64];
        DescribeAddress(context->Rip, line, sizeof(line));
        Report("[watch] thread %lu writes 0x%llx at %s rcx=%llx rdx=%llx r8=%llx\n", GetCurrentThreadId(), static_cast<unsigned long long>(address), line, context->Rcx, context->Rdx, context->R8);
        for (std::uint64_t slot = context->Rsp; slot < context->Rsp + 0x80 && IsReadable(slot); slot += 8) {
            const auto value = *reinterpret_cast<const std::uint64_t*>(slot);
            if (!IsExecutable(value)) continue;
            DescribeAddress(value, line, sizeof(line));
            Report("[watch]     [rsp+0x%llx] %s\n", static_cast<unsigned long long>(slot - context->Rsp), line);
        }
        std::uint64_t frame = context->Rbp;
        for (int depth = 0; depth < 6 && frame != 0 && (frame & 7) == 0 && IsReadable(frame) && IsReadable(frame + 8); ++depth) {
            const auto returnAddress = reinterpret_cast<const std::uint64_t*>(frame)[1];
            if (!IsExecutable(returnAddress)) break;
            DescribeAddress(returnAddress, line, sizeof(line));
            Report("[watch]     #%d %s\n", depth, line);
            const auto next = reinterpret_cast<const std::uint64_t*>(frame)[0];
            if (next <= frame) break;
            frame = next;
        }
        t_watchHit = true;
    }
    ProtectWatchPage(PAGE_READWRITE);
    context->EFlags |= 0x100;
    t_watchStepping = true;
    return true;
}

// Debug aid: APS5_BREAK_DUMP=<guest ELF vaddr> reports the registers, and the memory they and the
// first words of rbx's object point at, each time that instruction runs (first APS5_BREAK_LIMIT
// times, default 4). With APS5_BREAK_MATCH set, only hits whose text at [rbx+0x28] contains it are
// reported; APS5_BREAK_FILE=<bytes> also writes that much memory from rsi to break_<n>.bin.
std::uintptr_t g_breakAddress = 0;
std::uint8_t g_breakOriginal = 0;
std::atomic<int> g_breakCount{0};
int g_breakLimit = 4;
thread_local bool t_breakStepping = false;
std::mutex g_captureMutex;
std::uint64_t g_returnAddress = 0;
std::uint8_t g_returnOriginal = 0;
DWORD g_returnThread = 0;
thread_local std::uint64_t t_captureDestination = 0;
thread_local std::uint64_t t_captureSize = 0;

void WriteCodeByte(std::uintptr_t address, std::uint8_t value) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile std::uint8_t*>(address) = value;
    VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 1);
}

void ReportWords(const char* name, std::uint64_t value, int count) {
    if (value < 0x10000 || !IsReadable(value) || !IsReadable(value + count * 8 - 1)) return;
    const auto* words = reinterpret_cast<const std::uint64_t*>(value);
    for (int i = 0; i < count; i += 4) {
        Report("[break]   %s+0x%02x: %016llx %016llx %016llx %016llx\n", name, i * 8, words[i], words[i + 1], words[i + 2], words[i + 3]);
    }
}

bool HandleBreak(EXCEPTION_POINTERS* info) {
    if (g_breakAddress == 0) return false;
    const auto* record = info->ExceptionRecord;
    auto* context = info->ContextRecord;
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_breakStepping) {
        t_breakStepping = false;
        if (g_breakCount.load() < g_breakLimit) WriteCodeByte(g_breakAddress, 0xCC);
        return true;
    }
    if (record->ExceptionCode == EXCEPTION_BREAKPOINT && g_returnAddress != 0 && reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) == g_returnAddress) {
        std::lock_guard lock(g_captureMutex);
        if (GetCurrentThreadId() == g_returnThread && t_captureSize != 0 && IsReadable(t_captureDestination) && IsReadable(t_captureDestination + t_captureSize - 1)) {
            if (FILE* out = std::fopen("capture.bin", "ab")) {
                std::fwrite(reinterpret_cast<const void*>(t_captureDestination), 1, t_captureSize, out);
                std::fclose(out);
            }
        }
        WriteCodeByte(g_returnAddress, g_returnOriginal);
        context->Rip = g_returnAddress;
        g_returnAddress = 0;
        return true;
    }
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT || reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) != g_breakAddress) return false;
    // APS5_BREAK_CAPTURE=<text>: the breakpoint is a read(stream, destination, size) function whose
    // stream names its file at [stream+0x28]; for streams whose name contains the text, every read
    // is appended to capture.bin once the call returns.
    if (const char* capture = std::getenv("APS5_BREAK_CAPTURE")) {
        const auto name = context->Rdi >= 0x10000 && IsReadable(context->Rdi + 0x28) ? *reinterpret_cast<const std::uint64_t*>(context->Rdi + 0x28) : 0;
        const bool named = name >= 0x10000 && IsReadable(name) && IsReadable(name + 0x7f);
        if (named && std::strstr(reinterpret_cast<const char*>(name), capture) != nullptr && IsReadable(context->Rsp)) {
            std::lock_guard lock(g_captureMutex);
            if (g_returnAddress == 0) {
                t_captureDestination = context->Rsi;
                t_captureSize = context->Rdx & 0xffffffff;
                g_returnAddress = *reinterpret_cast<const std::uint64_t*>(context->Rsp);
                g_returnOriginal = *reinterpret_cast<const std::uint8_t*>(g_returnAddress);
                g_returnThread = GetCurrentThreadId();
                WriteCodeByte(g_returnAddress, 0xCC);
            }
        }
        WriteCodeByte(g_breakAddress, g_breakOriginal);
        context->Rip = g_breakAddress;
        context->EFlags |= 0x100;
        t_breakStepping = true;
        return true;
    }
    // APS5_BREAK_LOG: the breakpoint is a printf-style logging function taking the source file in
    // r8, the line in r9 and the format as its first stack argument; print those.
    if (std::getenv("APS5_BREAK_LOG") != nullptr) {
        g_breakCount.fetch_add(1);
        const auto format = IsReadable(context->Rsp + 8) ? *reinterpret_cast<const std::uint64_t*>(context->Rsp + 8) : 0;
        const auto source = context->R8;
        // Expand the integer and string conversions from the stack arguments that follow the format.
        char text[1024];
        std::size_t used = 0;
        if (format >= 0x10000 && IsReadable(format) && IsReadable(format + 0xff)) {
            const char* f = reinterpret_cast<const char*>(format);
            std::uint64_t slot = context->Rsp + 16;
            for (; *f != 0 && used + 1 < sizeof(text); ++f) {
                if (*f != '%' || f[1] == 0) { text[used++] = *f; continue; }
                const char* spec = f + 1;
                while (*spec != 0 && std::strchr("-+ #0123456789.lhzjt", *spec) != nullptr) ++spec;
                if (*spec == '%') { text[used++] = '%'; f = spec; continue; }
                const auto value = IsReadable(slot) ? *reinterpret_cast<const std::uint64_t*>(slot) : 0;
                if (*spec != 'f' && *spec != 'g' && *spec != 'e') slot += 8;
                int wrote = 0;
                if (*spec == 's') {
                    const bool ok = value >= 0x10000 && IsReadable(value) && IsReadable(value + 0x3f);
                    wrote = std::snprintf(text + used, sizeof(text) - used, "%.120s", ok ? reinterpret_cast<const char*>(value) : "(?)");
                } else if (*spec == 'f' || *spec == 'g' || *spec == 'e') {
                    wrote = std::snprintf(text + used, sizeof(text) - used, "<float>");
                } else {
                    wrote = std::snprintf(text + used, sizeof(text) - used, "%llx", static_cast<unsigned long long>(value));
                }
                if (wrote > 0) used = std::min(sizeof(text) - 1, used + static_cast<std::size_t>(wrote));
                f = spec;
            }
        }
        text[used] = 0;
        static const ULONGLONG start = GetTickCount64();
        Report("[log %llu] %d %.80s:%d %s\n", static_cast<unsigned long long>(GetTickCount64() - start), static_cast<int>(context->Rsi & 0xff),
               source >= 0x10000 && IsReadable(source) && IsReadable(source + 0x7f) ? reinterpret_cast<const char*>(source) : "?",
               static_cast<int>(context->R9 & 0xffffffff), text);
        WriteCodeByte(g_breakAddress, g_breakOriginal);
        context->Rip = g_breakAddress;
        context->EFlags |= 0x100;
        t_breakStepping = true;
        return true;
    }
    const char* match = std::getenv("APS5_BREAK_MATCH");
    const auto label = context->Rbx >= 0x10000 && IsReadable(context->Rbx + 0x28) ? *reinterpret_cast<const std::uint64_t*>(context->Rbx + 0x28) : 0;
    const bool labelled = label >= 0x10000 && IsReadable(label) && IsReadable(label + 0xff);
    if (match != nullptr && !(labelled && std::strstr(reinterpret_cast<const char*>(label), match) != nullptr)) {
        WriteCodeByte(g_breakAddress, g_breakOriginal);
        context->Rip = g_breakAddress;
        context->EFlags |= 0x100;
        t_breakStepping = true;
        return true;
    }
    const int hit = g_breakCount.fetch_add(1);
    if (std::getenv("APS5_BREAK_QUIET") != nullptr) {
        Report("[break] hit %d rbx %llx rsi %llx label %.120s\n", hit, context->Rbx, context->Rsi, labelled ? reinterpret_cast<const char*>(label) : "?");
        WriteCodeByte(g_breakAddress, g_breakOriginal);
        context->Rip = g_breakAddress;
        context->EFlags |= 0x100;
        t_breakStepping = true;
        return true;
    }
    Report("[break] hit %d on thread %lu: rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r12 %llx r13 %llx r14 %llx r15 %llx rbp %llx rsp %llx\n", hit, GetCurrentThreadId(), context->Rax, context->Rbx, context->Rcx, context->Rdx, context->Rsi, context->Rdi, context->R12, context->R13, context->R14, context->R15, context->Rbp, context->Rsp);
    const std::pair<const char*, std::uint64_t> registers[] = {{"rax", context->Rax}, {"rbx", context->Rbx}, {"rcx", context->Rcx}, {"rdx", context->Rdx}, {"rsi", context->Rsi}, {"rdi", context->Rdi}, {"r12", context->R12}, {"r14", context->R14}, {"r15", context->R15}};
    for (const auto& [name, value] : registers) ReportWords(name, value, 16);
    ReportWords("rbx-object", context->Rbx, 64);
    if (context->Rbx >= 0x10000 && IsReadable(context->Rbx) && IsReadable(context->Rbx + 0x1ff)) {
        const auto* words = reinterpret_cast<const std::uint64_t*>(context->Rbx);
        for (int i = 0; i < 64; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "[rbx+0x%02x]", i * 8);
            ReportWords(name, words[i], 8);
        }
        // APS5_BREAK_FILE=<bytes>: write that many bytes from rsi (fewer if memory ends) to
        // break_<n>.bin, but only when the text at [rbx+0x28] contains APS5_BREAK_MATCH.
        if (const char* file = std::getenv("APS5_BREAK_FILE")) {
            const auto name = label;
            const bool named = labelled;
            {
                std::uint64_t size = 0;
                const auto limit = std::strtoull(file, nullptr, 0);
                while (size < limit && IsReadable(context->Rsi + size)) size += 0x100;
                char path[64];
                std::snprintf(path, sizeof(path), "break_%d.bin", hit);
                if (FILE* out = std::fopen(path, "wb")) {
                    std::fwrite(reinterpret_cast<const void*>(context->Rsi), 1, size, out);
                    std::fclose(out);
                    Report("[break]   wrote %llu bytes to %s (%s)\n", static_cast<unsigned long long>(size), path, named ? reinterpret_cast<const char*>(name) : "?");
                }
            }
        }
    }
    WriteCodeByte(g_breakAddress, g_breakOriginal);
    context->Rip = g_breakAddress;
    context->EFlags |= 0x100;
    t_breakStepping = true;
    return true;
}

void InstallBreak() {
    const char* value = std::getenv("APS5_BREAK_DUMP");
    if (!value) return;
    if (const char* limit = std::getenv("APS5_BREAK_LIMIT")) g_breakLimit = std::atoi(limit);
    g_breakAddress = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + 0x10000 + std::strtoull(value, nullptr, 0);
    g_breakOriginal = *reinterpret_cast<const std::uint8_t*>(g_breakAddress);
    WriteCodeByte(g_breakAddress, 0xCC);
    Report("[break] breakpoint at 0x%llx\n", static_cast<unsigned long long>(g_breakAddress));
}

void InstallWatch() {
    // APS5_WATCH_ADDR=<absolute hex address> watches guest data that is mapped later: the page is
    // protected once it has been committed.
    if (const char* absolute = std::getenv("APS5_WATCH_ADDR")) {
        g_watchAddress = std::strtoull(absolute, nullptr, 16);
        const auto page = g_watchAddress & ~static_cast<std::uintptr_t>(0xfff);
        CreateThread(nullptr, 0, [](void* parameter) -> DWORD {
            const auto page = reinterpret_cast<std::uintptr_t>(parameter);
            for (;;) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(reinterpret_cast<const void*>(page), &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT) break;
                Sleep(1);
            }
            g_watchPage = page;
            ProtectWatchPage(PAGE_READONLY);
            Report("[watch] watching 0x%llx (page 0x%llx)\n", static_cast<unsigned long long>(g_watchAddress), static_cast<unsigned long long>(page));
            return 0;
        }, reinterpret_cast<void*>(page), 0, nullptr);
        return;
    }
    const char* value = std::getenv("APS5_WATCH_WRITE");
    if (!value) return;
    const auto elfAddress = std::strtoull(value, nullptr, 0);
    g_watchAddress = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + 0x10000 + elfAddress;
    g_watchPage = g_watchAddress & ~static_cast<std::uintptr_t>(0xfff);
    ProtectWatchPage(PAGE_READONLY);
    Report("[watch] watching 0x%llx (page 0x%llx)\n", static_cast<unsigned long long>(g_watchAddress), static_cast<unsigned long long>(g_watchPage));
}

// SSE4a (EXTRQ / INSERTQ) is AMD-only: the PS5's Zen 2 has it and titles use it, so on an Intel host the
// instruction raises STATUS_ILLEGAL_INSTRUCTION and is emulated on the faulting thread's CONTEXT.
// APS5_NO_SSE4A_EMULATION=1 disables this (the fault is then reported as fatal as before) and
// APS5_TRACE_SSE4A=1 prints each emulated instruction once per rip.
bool g_sse4aEmulation = true;
bool g_sse4aTrace = false;
std::atomic<unsigned long long> g_sse4aEmulated{0};

bool IsEnvironmentSet(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && !(value[0] == '0' && value[1] == '\0');
}

// Copies the bytes at `address` that are mapped; the read stays within readable pages.
std::size_t ReadCode(std::uint64_t address, std::uint8_t* buffer, std::size_t size) {
    std::size_t count = 0;
    while (count < size) {
        const auto cursor = address + count;
        if ((cursor & 0xfff) == 0 || count == 0) {
            if (!IsReadable(cursor)) break;
        }
        buffer[count++] = *reinterpret_cast<const std::uint8_t*>(cursor);
    }
    return count;
}

void TraceSse4a(std::uint64_t rip, const sse4a::Instruction& instruction, const sse4a::Field& field, unsigned long long count) {
    static std::mutex mutex;
    static std::uint64_t seen[128];
    static std::size_t seenCount = 0;
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < seenCount; ++i) {
            if (seen[i] == rip) return;
        }
        if (seenCount < sizeof(seen) / sizeof(seen[0])) seen[seenCount++] = rip;
    }
    char line[MAX_PATH + 64];
    DescribeAddress(rip, line, sizeof(line));
    const char* mnemonic = instruction.op == sse4a::Op::Extrq ? "extrq" : "insertq";
    char operands[64];
    if (instruction.op == sse4a::Op::Extrq && !instruction.registerForm) {
        std::snprintf(operands, sizeof(operands), "xmm%u, %u, %u", instruction.destination, instruction.length, instruction.index);
    } else if (!instruction.registerForm) {
        std::snprintf(operands, sizeof(operands), "xmm%u, xmm%u, %u, %u", instruction.destination, instruction.source, instruction.length, instruction.index);
    } else {
        std::snprintf(operands, sizeof(operands), "xmm%u, xmm%u", instruction.destination, instruction.source);
    }
    Report("[sse4a] #%llu %s %s (length %u, index %u, %u bytes) at %s on thread %lu\n", count, mnemonic, operands, field.length, field.index, static_cast<unsigned>(instruction.size), line, GetCurrentThreadId());
}

bool HandleSse4a(EXCEPTION_POINTERS* info) {
    if (!g_sse4aEmulation) return false;
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return false;
    auto* context = info->ContextRecord;
    const auto rip = context->Rip;
    std::uint8_t bytes[sse4a::kMaxInstructionSize];
    const std::size_t available = ReadCode(rip, bytes, sizeof(bytes));
    sse4a::Instruction instruction;
    sse4a::Field field;
    if (!sse4a::Emulate(bytes, available, *context, &instruction, &field)) return false;
    const auto count = ++g_sse4aEmulated;
    if (g_sse4aTrace) TraceSse4a(rip, instruction, field, count);
    return true;
}

// A renderer that write-protects guest pages it caches on the GPU (the shadGT-based
// libSceVideoOut.prx) gets each access fault first: it owns those pages, drops its copy and
// unprotects them. Ryty's own shared-memory write handling would otherwise unprotect them unseen.
// 1: handled, retry; 2: an expected fault in one of the renderer's guarded reads, which its own
// structured handler takes; 0: not the renderer's.
int RendererHandlesFault(EXCEPTION_POINTERS* info) {
    using FaultFunction = int (*)(void*);
    static FaultFunction handler = nullptr;
    if (handler == nullptr) {
        if (HMODULE renderer = GetModuleHandleA("libSceVideoOut.prx")) {
            handler = reinterpret_cast<FaultFunction>(reinterpret_cast<void*>(GetProcAddress(renderer, "RytyGpuHandleWriteFault")));
        }
    }
    return handler != nullptr ? handler(info) : 0;
}

// APS5_TRACE_THROW=1 reports each C++ exception thrown by Ryty's (GCC-built) libraries as it is
// raised: its type, its message when it is a standard exception, and the code addresses on the
// stack. An exception that escapes into guest frames cannot be unwound (STATUS_BAD_FUNCTION_TABLE),
// so this is the only trace of it.
void TraceThrow(EXCEPTION_POINTERS* info) {
    constexpr DWORD GccThrow = 0x20474343;
    // On by default for the first 40 throws (a throw that escapes into game code ends the
    // process with no other trace); APS5_TRACE_THROW=1 reports every one, =0 none.
    static const char* setting = std::getenv("APS5_TRACE_THROW");
    static const bool all = setting != nullptr && setting[0] == '1';
    static const bool none = setting != nullptr && setting[0] == '0';
    static std::atomic<unsigned> reported{0};
    const auto* record = info->ExceptionRecord;
    if (none || record->ExceptionCode != GccThrow || record->NumberParameters < 1) return;
    if (!all && reported.fetch_add(1) >= 40) return;
    // libgcc raises it with the _Unwind_Exception (64 bytes under SEH) that directly precedes the
    // thrown object; the object's vtable is preceded by its type_info.
    const auto unwind = static_cast<std::uintptr_t>(record->ExceptionInformation[0]);
    const auto object = unwind + 64;
    char type[160] = "?";
    char what[256] = "";
    if (IsReadable(object) && IsReadable(object + 7)) {
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
        if (vtable >= 0x10000 && IsReadable(vtable - 8) && IsReadable(vtable + 23)) {
            const auto* typeInfo = *reinterpret_cast<const std::type_info* const*>(vtable - 8);
            if (typeInfo != nullptr && IsReadable(reinterpret_cast<std::uintptr_t>(typeInfo) + 15)) {
                std::snprintf(type, sizeof(type), "%s", typeInfo->name());
                if (std::strstr(type, "St") == type || std::strstr(type, "error") != nullptr || std::strstr(type, "exception") != nullptr) {
                    std::snprintf(what, sizeof(what), "%s", reinterpret_cast<const std::exception*>(object)->what());
                }
            }
        }
    }
    Report("[throw] thread %lu %s: %s\n", GetCurrentThreadId(), type, what);
    const auto* context = info->ContextRecord;
    char line[MAX_PATH + 64];
    int printed = 0;
    for (std::uint64_t slot = context->Rsp; printed < 10 && slot < context->Rsp + 0x1000; slot += 8) {
        if (!IsReadable(slot)) break;
        const auto value = *reinterpret_cast<const std::uint64_t*>(slot);
        if (!IsExecutable(value)) continue;
        DescribeAddress(value, line, sizeof(line));
        Report("[throw]   %s\n", line);
        ++printed;
    }
}

LONG WINAPI ReportCrash(EXCEPTION_POINTERS* info) {
    static std::atomic<bool> reported{false};
    const auto* fault = info->ExceptionRecord;
    TraceThrow(info);
    if (fault->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        const int renderer = RendererHandlesFault(info);
        if (renderer == 1) return EXCEPTION_CONTINUE_EXECUTION;
        if (renderer == 2) {
            static std::atomic<unsigned> passed{0};
            if (passed.fetch_add(1) < 20) {
                std::fprintf(stderr, "[fault] passed to the renderer's guarded read: %p (rip %p)\n",
                             reinterpret_cast<void*>(fault->ExceptionInformation[1]), fault->ExceptionAddress);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (fault->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && fault->NumberParameters >= 2 && fault->ExceptionInformation[0] == 1 && GuestArena::GuestArenaHandleWrite_nid_postfix(fault->ExceptionInformation[1])) {
        // This unprotects the whole 16 KB view, including pages the renderer still tracks, so
        // it should only happen for writes the renderer has nothing to do with.
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 50) {
            std::fprintf(stderr, "[fault] Ryty resumed a write to %p the renderer did not handle (rip %p)\n",
                         reinterpret_cast<void*>(fault->ExceptionInformation[1]), fault->ExceptionAddress);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (fault->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && fault->NumberParameters >= 2) {
        // RyGT: guest memory another thread was remapping (its pages are missing for a moment;
        // much longer under Wine). Retry once the remap is done, but not forever at one address.
        thread_local std::uintptr_t lastAddress = 0;
        thread_local unsigned retries = 0;
        const auto address = static_cast<std::uintptr_t>(fault->ExceptionInformation[1]);
        retries = address == lastAddress ? retries + 1 : 0;
        lastAddress = address;
        if (retries < 64 && GuestArena::GuestArenaAwaitRemap_nid_postfix(address, fault->ExceptionInformation[0] == 1)) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1) < 20) {
                std::fprintf(stderr, "[fault] waited for a guest remap at %p (rip %p)\n",
                             reinterpret_cast<void*>(address), fault->ExceptionAddress);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (HandleBreak(info)) return EXCEPTION_CONTINUE_EXECUTION;
    if (HandleWatch(info)) return EXCEPTION_CONTINUE_EXECUTION;
    if (HandleSse4a(info)) return EXCEPTION_CONTINUE_EXECUTION;
    const auto* record = info->ExceptionRecord;
    if (!IsFatal(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (reported.exchange(true)) {
        // Another thread is already reporting; let it finish before this fault ends the process.
        Sleep(INFINITE);
    }
    const auto* context = info->ContextRecord;
    char line[MAX_PATH + 64];
    char threadName[128] = "";
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, threadName, sizeof(threadName), nullptr, nullptr);
        LocalFree(description);
    }
    Report("\nFATAL: unhandled exception 0x%08lx on thread %lu '%s'\n", record->ExceptionCode, GetCurrentThreadId(), threadName);
    DescribeAddress(context->Rip, line, sizeof(line));
    Report("  rip %s\n", line);
    if (record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        std::uint8_t bytes[8];
        const std::size_t available = ReadCode(context->Rip, bytes, sizeof(bytes));
        char hex[3 * sizeof(bytes) + 1] = "";
        for (std::size_t i = 0; i < available; ++i) std::snprintf(hex + 3 * i, sizeof(hex) - 3 * i, "%02x ", bytes[i]);
        Report("  bytes %s(sse4a emulation %s, %llu emulated so far)\n", hex, g_sse4aEmulation ? "on" : "off", g_sse4aEmulated.load());
    }
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const char* kind = record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute";
        Report("  %s of 0x%016llx\n", kind, static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    if (record->ExceptionCode == 0xc0000006u && record->NumberParameters >= 3) {
        Report("  in-page error at 0x%016llx, underlying status 0x%08llx\n",
            static_cast<unsigned long long>(record->ExceptionInformation[1]),
            static_cast<unsigned long long>(record->ExceptionInformation[2]));
    }
    Report("  rax %016llx rbx %016llx rcx %016llx rdx %016llx\n", context->Rax, context->Rbx, context->Rcx, context->Rdx);
    Report("  rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n", context->Rsi, context->Rdi, context->Rbp, context->Rsp);
    Report("  r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n", context->R8, context->R9, context->R10, context->R11);
    Report("  r12 %016llx r13 %016llx r14 %016llx r15 %016llx\n", context->R12, context->R13, context->R14, context->R15);
    // The words each register points at, to recognise which structure a bad pointer came from.
    {
        const std::pair<const char*, std::uint64_t> pointers[] = {{"rax", context->Rax}, {"rbx", context->Rbx}, {"rcx", context->Rcx}, {"rdx", context->Rdx}, {"rsi", context->Rsi}, {"rdi", context->Rdi}, {"r12", context->R12}, {"r13", context->R13}, {"r14", context->R14}, {"r15", context->R15}};
        for (const auto& [name, value] : pointers) {
            if (value < 0x10000 || (value & 7) != 0 || !IsReadable(value) || !IsReadable(value + 0x38)) continue;
            const auto* words = reinterpret_cast<const std::uint64_t*>(value);
            Report("  [%s] %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n", name, words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
        }
    }
    // APS5_CRASH_STRING=<ELF address> prints the text the title keeps there (a last error message,
    // for instance), addressed like APS5_WATCH_WRITE.
    if (const char* stringAddress = std::getenv("APS5_CRASH_STRING")) {
        const auto address = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + 0x10000 + std::strtoull(stringAddress, nullptr, 0);
        if (IsReadable(address) && IsReadable(address + 0x7ff)) {
            Report("  [string] %.2047s\n", reinterpret_cast<const char*>(address));
        }
    }
    Report("  frame pointer chain:\n");
    std::uint64_t frame = context->Rbp;
    for (int depth = 0; depth < 32 && frame != 0 && (frame & 7) == 0 && IsReadable(frame) && IsReadable(frame + 8); ++depth) {
        const auto returnAddress = reinterpret_cast<const std::uint64_t*>(frame)[1];
        if (!IsExecutable(returnAddress)) break;
        DescribeAddress(returnAddress, line, sizeof(line));
        Report("    #%d %s\n", depth, line);
        const auto next = reinterpret_cast<const std::uint64_t*>(frame)[0];
        if (next <= frame) break;
        frame = next;
    }
    Report("  stack return address candidates:\n");
    int printed = 0;
    for (std::uint64_t slot = context->Rsp; printed < 16 && slot < context->Rsp + 0x2000; slot += 8) {
        if (!IsReadable(slot)) break;
        const auto value = *reinterpret_cast<const std::uint64_t*>(slot);
        if (!IsExecutable(value)) continue;
        DescribeAddress(value, line, sizeof(line));
        Report("    [rsp+0x%llx] %s\n", static_cast<unsigned long long>(slot - context->Rsp), line);
        ++printed;
    }
    if (context->Rip == 0 || !IsExecutable(context->Rip)) {
        // A jump into nothing usually follows a return through a clobbered frame; the frames that just
        // returned are still below rsp.
        Report("  recently popped return address candidates:\n");
        for (std::uint64_t slot = context->Rsp - 8; slot >= context->Rsp - 0x800; slot -= 8) {
            if (!IsReadable(slot)) break;
            const auto value = *reinterpret_cast<const std::uint64_t*>(slot);
            if (!IsExecutable(value)) continue;
            DescribeAddress(value, line, sizeof(line));
            Report("    [rsp-0x%llx] %s\n", static_cast<unsigned long long>(context->Rsp - slot), line);
        }
        // Registers often still point into the stack the code ran on before the bad return.
        const std::pair<const char*, std::uint64_t> registers[] = {{"rdi", context->Rdi}, {"rsi", context->Rsi}, {"rbx", context->Rbx}, {"r12", context->R12}, {"r13", context->R13}, {"r14", context->R14}, {"r15", context->R15}};
        for (const auto& [name, value] : registers) {
            if (value < 0x10000 || (value & 7) != 0 || !IsReadable(value)) continue;
            Report("  code addresses above %s (0x%llx):\n", name, static_cast<unsigned long long>(value));
            int found = 0;
            for (std::uint64_t slot = value; found < 24 && slot < value + 0x1000; slot += 8) {
                if (!IsReadable(slot)) break;
                const auto candidate = *reinterpret_cast<const std::uint64_t*>(slot);
                if (!IsExecutable(candidate)) continue;
                DescribeAddress(candidate, line, sizeof(line));
                Report("    [+0x%llx] %s\n", static_cast<unsigned long long>(slot - value), line);
                ++found;
            }
        }
    }
    std::fflush(stderr);
    // Ending the process here skips DLL_PROCESS_DETACH: an exit through the normal path from a faulting
    // thread can deadlock in the GPU driver's teardown (seen with NVIDIA Vulkan), leaving a process that
    // holds the run folder's files for minutes. APS5_CRASH_SEARCH keeps the previous behaviour.
    // APS5_CRASH_PAUSE=<seconds> keeps the faulting process alive that long so its memory can be
    // inspected from outside.
    if (const char* pause = std::getenv("APS5_CRASH_PAUSE")) {
        Report("  pausing %s s for inspection (pid %lu)\n", pause, GetCurrentProcessId());
        std::fflush(stderr);
        Sleep(static_cast<DWORD>(std::strtoul(pause, nullptr, 10) * 1000));
    }
    if (std::getenv("APS5_CRASH_SEARCH") == nullptr) TerminateProcess(GetCurrentProcess(), record->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() and the UCRT's invalid-parameter path end the process with a silent fast fail
// (0xc0000409); both are reported with the calling thread's stack first.
void ReportBacktrace(const char* what) {
    void* frames[48];
    const auto count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    Report("FATAL: %s on thread %lu\n", what, static_cast<unsigned long>(GetCurrentThreadId()));
    char line[256];
    for (USHORT i = 0; i < count; ++i) {
        DescribeAddress(reinterpret_cast<std::uint64_t>(frames[i]), line, sizeof(line));
        Report("    #%u %s\n", static_cast<unsigned>(i), line);
    }
    std::fflush(stderr);
}

void AbortSignalHandler(int) {
    ReportBacktrace("abort() (SIGABRT)");
}

void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {
    ReportBacktrace("UCRT invalid parameter");
}

// Static destruction runs on the thread that ends the process (ExitProcess -> DLL_PROCESS_DETACH),
// so its stack names the exit's caller when no libc exit/abort path was taken.
struct ExitReporter {
    ~ExitReporter() {
        if (std::getenv("APS5_TRACE_EXIT") != nullptr) ReportBacktrace("process exit (static destruction)");
    }
} g_exitReporter;

const bool g_crashReportInstalled = [] {
    // The game's files (app0, _sd, user) are found relative to the working directory. Started
    // from a shortcut, the taskbar or Windows search, that is not the game's folder, and the
    // game failed at once ("neither param.json nor param.sfo found"). Use GTSport.exe's folder.
    {
        wchar_t exe[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (length != 0 && length < MAX_PATH) {
            for (DWORD i = length; i > 0; --i) {
                if (exe[i - 1] == L'\\' || exe[i - 1] == L'/') {
                    exe[i - 1] = 0;
                    SetCurrentDirectoryW(exe);
                    break;
                }
            }
        }
    }
    g_sse4aEmulation = !IsEnvironmentSet("APS5_NO_SSE4A_EMULATION");
    g_sse4aTrace = IsEnvironmentSet("APS5_TRACE_SSE4A");
    AddVectoredExceptionHandler(1, ReportCrash);
    // An exception every handler passed on (the renderer's guarded reads pass faults to their
    // own structured handler): name it before the process ends without a report.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        const auto* record = info->ExceptionRecord;
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(record->ExceptionAddress), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        std::fprintf(stderr, "FATAL (no handler took it): exception 0x%08lx at %p in %s+0x%llx, address %p, thread %lu\n",
                     record->ExceptionCode, record->ExceptionAddress, name,
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) - reinterpret_cast<std::uintptr_t>(module)),
                     record->NumberParameters >= 2 ? reinterpret_cast<void*>(record->ExceptionInformation[1]) : nullptr,
                     GetCurrentThreadId());
        // The thread's name and the code addresses on its stack, so an error raised by a
        // library (the code above is then only RaiseException) names its caller.
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
            std::fprintf(stderr, "  thread name: %ls\n", description);
            LocalFree(description);
        }
        const auto* context = info->ContextRecord;
        char line[MAX_PATH + 64];
        int printed = 0;
        for (std::uint64_t slot = context->Rsp; printed < 24 && slot < context->Rsp + 0x2000; slot += 8) {
            if (!IsReadable(slot)) break;
            const auto value = *reinterpret_cast<const std::uint64_t*>(slot);
            if (!IsExecutable(value)) continue;
            DescribeAddress(value, line, sizeof(line));
            std::fprintf(stderr, "  [rsp+0x%llx] %s\n", static_cast<unsigned long long>(slot - context->Rsp), line);
            ++printed;
        }
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    });
    std::signal(SIGABRT, AbortSignalHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);
    InstallWatch();
    InstallBreak();
    return true;
}();

}
#endif
