#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"

#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <stdexcept>
#include <string>
#include <x86intrin.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <sys/time.h>
#endif

#ifdef _WIN32
namespace {
constexpr std::int64_t WindowsEpochSeconds = 11644473600LL;
struct WindowsLocalClock {
    std::int64_t utc;
    std::int64_t local;
    std::int32_t standardOffset;
    std::int32_t daylightOffset;
};

bool WindowsCalendar(std::int64_t seconds, SYSTEMTIME& calendar) {
    // Bound the multiplication and reject dates outside Windows' FILETIME range.
    if (seconds < -WindowsEpochSeconds || seconds > 922337203685LL - WindowsEpochSeconds) return false;
    const auto ticks = static_cast<std::uint64_t>(seconds + WindowsEpochSeconds) * 10000000ULL;
    const FILETIME file{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    return FileTimeToSystemTime(&file, &calendar) != FALSE;
}

bool WindowsSeconds(const SYSTEMTIME& calendar, std::int64_t& seconds) {
    FILETIME file{};
    if (!SystemTimeToFileTime(&calendar, &file)) return false;
    const auto ticks = (static_cast<std::uint64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    seconds = static_cast<std::int64_t>(ticks / 10000000ULL) - WindowsEpochSeconds;
    return true;
}

// Wine and Proton's kernel32 export neither ...Ex conversion (kernelbase only has stubs), and a
// static import of them keeps libkernel from loading there. Look them up at run time and fall back
// to the plain conversions with the local year's rules of the same time zone, which gives the same
// results (checked against the ...Ex functions for every Windows time zone, 2024-2027).
using ZoneConversion = BOOL(WINAPI*)(const DYNAMIC_TIME_ZONE_INFORMATION*, const SYSTEMTIME*, LPSYSTEMTIME);

ZoneConversion KernelZoneConversion(const char* name) {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    return kernel ? reinterpret_cast<ZoneConversion>(reinterpret_cast<void*>(GetProcAddress(kernel, name))) : nullptr;
}

// A year's rules of the zone, cached (GetTimeZoneInformationForYear reads the registry each time).
bool ZoneRules(DYNAMIC_TIME_ZONE_INFORMATION& zone, int year, TIME_ZONE_INFORMATION& rules) {
    static std::mutex lock;
    static std::wstring cachedZone;
    static std::unordered_map<int, TIME_ZONE_INFORMATION> cache;
    const std::wstring key = std::wstring(zone.TimeZoneKeyName) + (zone.DynamicDaylightTimeDisabled ? L"|fixed" : L"");
    std::lock_guard guard(lock);
    if (key != cachedZone) {
        cache.clear();
        cachedZone = key;
    }
    if (const auto found = cache.find(year); found != cache.end()) {
        rules = found->second;
        return true;
    }
    if (!GetTimeZoneInformationForYear(static_cast<USHORT>(year), &zone, &rules)) return false;
    cache.emplace(year, rules);
    return true;
}

bool UtcToZoneTime(DYNAMIC_TIME_ZONE_INFORMATION& zone, const SYSTEMTIME& universal, SYSTEMTIME& local) {
    static const auto convert = KernelZoneConversion("SystemTimeToTzSpecificLocalTimeEx");
    if (convert) return convert(&zone, &universal, &local) != FALSE;
    TIME_ZONE_INFORMATION rules{};
    if (!ZoneRules(zone, universal.wYear, rules) || !SystemTimeToTzSpecificLocalTime(&rules, &universal, &local)) return false;
    // Around New Year the local year differs from the UTC year, and its rules decide.
    return local.wYear == universal.wYear ||
           (ZoneRules(zone, local.wYear, rules) && SystemTimeToTzSpecificLocalTime(&rules, &universal, &local));
}

bool ZoneTimeToUtc(DYNAMIC_TIME_ZONE_INFORMATION& zone, const SYSTEMTIME& local, SYSTEMTIME& universal) {
    static const auto convert = KernelZoneConversion("TzSpecificLocalTimeToSystemTimeEx");
    if (convert) return convert(&zone, &local, &universal) != FALSE;
    TIME_ZONE_INFORMATION rules{};
    return ZoneRules(zone, local.wYear, rules) && TzSpecificLocalTimeToSystemTime(&rules, &local, &universal);
}

bool WindowsClockFromUtc(std::int64_t utc, WindowsLocalClock& result) {
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) return false;
    SYSTEMTIME universal{}, local{};
    if (!WindowsCalendar(utc, universal) || !UtcToZoneTime(zone, universal, local)) return false;
    TIME_ZONE_INFORMATION rules{};
    if (!ZoneRules(zone, local.wYear, rules)) return false;
    std::int64_t localSeconds{};
    if (!WindowsSeconds(local, localSeconds)) return false;
    const auto standard = static_cast<std::int32_t>(-(rules.Bias + rules.StandardBias) * 60);
    result = {utc, localSeconds, standard, static_cast<std::int32_t>(localSeconds - utc - standard)};
    return true;
}

bool WindowsClockFromLocal(std::int64_t local, WindowsLocalClock& result) {
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) return false;
    SYSTEMTIME calendar{}, universal{};
    std::int64_t utc{};
    if (!WindowsCalendar(local, calendar) || !ZoneTimeToUtc(zone, calendar, universal) ||
        !WindowsSeconds(universal, utc)) return false;
    return WindowsClockFromUtc(utc, result);
}

KernelTimezone WindowsGuestTimezone(const WindowsLocalClock& clock) {
    // The guest ABI reports signed UTC-to-local minutes, plus DST minutes separately.
    return {clock.standardOffset / 60, clock.daylightOffset / 60};
}
}
#endif
static std::uint64_t RawMonotonicNanos() {
#ifdef _WIN32
    return TimedWait::NowNanos();
#else
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

// Debug aid: APS5_TIME_SCALE=<factor> slows (or speeds) the guest's monotonic clocks and TSC, so a
// title running far below its frame rate sees plausible frame times. Sleeps still take real time.
static double TimeScale() {
    static const double scale = [] {
        const char* value = std::getenv("APS5_TIME_SCALE");
        const double parsed = value ? std::strtod(value, nullptr) : 1.0;
        return parsed > 0.0 ? parsed : 1.0;
    }();
    return scale;
}

static std::uint64_t ClockOrigin() {
    static const std::uint64_t origin = RawMonotonicNanos();
    return origin;
}

static std::uint64_t GetMonotonicNanos() {
    const auto raw = RawMonotonicNanos();
    if (TimeScale() == 1.0) return raw;
    const auto origin = ClockOrigin();
    return origin + static_cast<std::uint64_t>(static_cast<double>(raw - origin) * TimeScale());
}

#ifdef _WIN32
// The shadGT renderer (libSceVideoOut.prx) holds the guest clocks while its GPU thread compiles
// shaders or pipelines, which a PS4 never does (its shaders are precompiled). Its RytyGpuGuestTsc
// export is the host TSC with those pauses taken out. Without it a compile stall looks like a frame
// hundreds of milliseconds long to the game: GT Sport's car thumbnail script polls its shot after one
// 0.1 s sleep, finds nothing, and asserts (file_size > 10 * 1024) when a new car is bought.
using GuestTscFn = std::uint64_t (*)();

static GuestTscFn RendererGuestTsc() {
    static std::atomic<GuestTscFn> found{nullptr};
    static std::atomic<std::uint32_t> misses{0};
    GuestTscFn fn = found.load(std::memory_order_acquire);
    if (fn != nullptr) return fn;
    // Looked up again now and then until the renderer is loaded (it loads before the game starts).
    if ((misses.fetch_add(1, std::memory_order_relaxed) & 4095) != 0) return nullptr;
    if (std::getenv("APS5_NO_GUEST_CLOCK") != nullptr) return nullptr;
    if (HMODULE renderer = GetModuleHandleA("libSceVideoOut.prx")) {
        fn = reinterpret_cast<GuestTscFn>(reinterpret_cast<void*>(GetProcAddress(renderer, "RytyGpuGuestTsc")));
        if (fn != nullptr) found.store(fn, std::memory_order_release);
    }
    return fn;
}

extern "C" uint64_t APS5_VABI sceKernelGetTscFrequency(void);

// The game's monotonic nanoseconds: the renderer's guest TSC when it provides one (continuing from
// the host clock at the moment it was found), else the host clock. Never goes backwards.
static std::uint64_t GuestMonotonicNanos() {
    const GuestTscFn guestTsc = RendererGuestTsc();
    if (guestTsc == nullptr) return GetMonotonicNanos();
    struct Base {
        std::uint64_t nanos, tsc, frequency;
    };
    static const Base base = [&] {
        const std::uint64_t frequency = sceKernelGetTscFrequency();
        return Base{GetMonotonicNanos(), guestTsc(), frequency};
    }();
    const std::uint64_t ticks = guestTsc() - base.tsc;
    const std::uint64_t nanos = base.nanos + static_cast<std::uint64_t>(static_cast<unsigned __int128>(ticks) * 1000000000ULL / base.frequency);
    static std::atomic<std::uint64_t> last{0};
    std::uint64_t previous = last.load(std::memory_order_relaxed);
    while (nanos > previous && !last.compare_exchange_weak(previous, nanos, std::memory_order_relaxed)) {
    }
    return std::max(nanos, previous);
}
#else
static std::uint64_t GuestMonotonicNanos() {
    return GetMonotonicNanos();
}
#endif

static std::uint64_t GetStartNanos() {
    static const std::uint64_t start = GuestMonotonicNanos();
    return start;
}

#ifdef _WIN32
// Every relative wait in the process (winpthreads nanosleep, std::condition_variable::wait_for, the
// GPU driver's poll sleeps) rounds up to the scheduler tick, 15.6 ms by default; a console game and
// its driver hand labels between queues dozens of times per frame, so the tick is raised to 0.5 ms at
// load, and Windows 11 is told not to drop it while the window is unfocused. APS5_NO_TIMER_RESOLUTION=1
// keeps the default for comparison.
static const bool g_timerResolutionRaised = [] {
    if (std::getenv("APS5_NO_TIMER_RESOLUTION") != nullptr) return false;
    using NtSetTimerResolutionFn = LONG(WINAPI*)(ULONG, BOOLEAN, PULONG);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto setResolution = ntdll ? reinterpret_cast<NtSetTimerResolutionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetTimerResolution"))) : nullptr;
    ULONG previous = 0;
    const bool raised = setResolution != nullptr && setResolution(5000, TRUE, &previous) >= 0;
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    using NtQueryTimerResolutionFn = LONG(WINAPI*)(PULONG, PULONG, PULONG);
    const auto queryResolution = ntdll ? reinterpret_cast<NtQueryTimerResolutionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryTimerResolution"))) : nullptr;
    ULONG minimum = 0, maximum = 0, current = 0;
    if (queryResolution) queryResolution(&minimum, &maximum, &current);
    std::fprintf(stderr, "[time] scheduler tick %s (was %.2f ms, now %.2f ms, timed waits %s)\n", raised ? "raised to 0.5 ms" : "unchanged", previous / 10000.0, current / 10000.0, TimedWait::Coarse() ? "coarse" : "fine");
    return raised;
}();

extern "C" {
int APS5_VABI scePthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name);
int APS5_VABI scePthreadCondDestroy(PthreadCond* cond);
int APS5_VABI scePthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, unsigned int usec);
int APS5_VABI scePthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name);
int APS5_VABI scePthreadMutexDestroy(PthreadMutex* mutex);
int APS5_VABI scePthreadMutexLock(PthreadMutex* mutex);
int APS5_VABI scePthreadMutexUnlock(PthreadMutex* mutex);
}

static void ReportTimedWait(const char* name, const std::function<void()>& body) {
    std::vector<double> samples(200);
    for (auto& sample : samples) {
        const std::uint64_t start = TimedWait::NowNanos();
        body();
        sample = static_cast<double>(TimedWait::NowNanos() - start) / 1000.0;
    }
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (const double sample : samples) sum += sample;
    std::fprintf(stderr, "[timers] %-44s mean %8.1f  p50 %8.1f  max %8.1f us\n", name, sum / static_cast<double>(samples.size()), samples[samples.size() / 2], samples.back());
}

static const bool g_timerSelfTest = [] {
    if (std::getenv("APS5_TRACE_TIMERS") == nullptr) return false;
    std::mutex mutex;
    std::condition_variable cv;
    ReportTimedWait("std::condition_variable::wait_for(1 ms)", [&] { std::unique_lock<std::mutex> lock(mutex); cv.wait_for(lock, std::chrono::milliseconds(1)); });
    PthreadCond cond = nullptr;
    PthreadMutex guestMutex = nullptr;
    scePthreadCondInit(&cond, nullptr, nullptr);
    scePthreadMutexInit(&guestMutex, nullptr, nullptr);
    scePthreadMutexLock(&guestMutex);
    ReportTimedWait("scePthreadCondTimedwait(1000 us)", [&] { scePthreadCondTimedwait(&cond, &guestMutex, 1000); });
    ReportTimedWait("scePthreadCondTimedwait(13000 us)", [&] { scePthreadCondTimedwait(&cond, &guestMutex, 13000); });
    scePthreadMutexUnlock(&guestMutex);
    scePthreadMutexDestroy(&guestMutex);
    scePthreadCondDestroy(&cond);
    ReportTimedWait("sceKernelUsleep(1000)", [&] { sceKernelUsleep_nid_postfix(1000); });
    ReportTimedWait("Sleep(1)", [&] { Sleep(1); });
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ReportTimedWait("WaitForSingleObject(event, 1)", [&] { WaitForSingleObject(event, 1); });
    CloseHandle(event);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ReportTimedWait("high-resolution waitable timer 1 ms", [&] { LARGE_INTEGER due{}; due.QuadPart = -10000; SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, INFINITE); });
    CloseHandle(timer);
    return true;
}();
#endif

extern "C" {

std::uint64_t APS5_VABI sceKernelGetProcessTime() {
    return (GuestMonotonicNanos() - GetStartNanos()) / 1000ULL;
}

std::uint64_t APS5_VABI sceKernelGetProcessTimeCounter() {
    return GuestMonotonicNanos() - GetStartNanos();
}

std::uint64_t APS5_VABI sceKernelGetProcessTimeCounterFrequency() {
    return 1000000000ULL;
}

// Debug aid: APS5_TRACE_USLEEP reports every 2000 calls which guest call sites sleep, so a CPU
// thread polling for GPU or I/O completion can be found (offsets are relative to the executable).
static void TraceSleep(const void* caller, std::uint64_t microseconds) {
    static const bool enabled = std::getenv("APS5_TRACE_USLEEP") != nullptr;
    if (!enabled) return;
    static std::mutex mutex;
    static std::unordered_map<std::uintptr_t, std::pair<std::uint64_t, std::uint64_t>> sites;
    static std::uint64_t calls = 0;
    std::lock_guard lock(mutex);
    auto& site = sites[reinterpret_cast<std::uintptr_t>(caller)];
    ++site.first;
    site.second += microseconds;
    if (++calls % 2000 != 0) return;
#ifdef _WIN32
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#else
    const std::uintptr_t image = 0;
#endif
    std::vector<std::pair<std::uintptr_t, std::pair<std::uint64_t, std::uint64_t>>> hot(sites.begin(), sites.end());
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    std::fprintf(stderr, "[usleep] %llu calls:", static_cast<unsigned long long>(calls));
    for (std::size_t i = 0; i < hot.size() && i < 6; ++i) std::fprintf(stderr, " exe+0x%llx x%llu (%llu us)", static_cast<unsigned long long>(hot[i].first - image), static_cast<unsigned long long>(hot[i].second.first), static_cast<unsigned long long>(hot[i].second.second));
    std::fprintf(stderr, "\n");
}

extern "C++" {

#ifdef _WIN32
using DiagRecordFn = void (*)(std::uint32_t, std::uint64_t, std::uint64_t);

static DiagRecordFn RendererDiag() {
    static std::atomic<DiagRecordFn> found{nullptr};
    static std::atomic<std::uint32_t> misses{0};
    DiagRecordFn fn = found.load(std::memory_order_acquire);
    if (fn != nullptr) return fn;
    if ((misses.fetch_add(1, std::memory_order_relaxed) & 1023) != 0) return nullptr;
    if (HMODULE renderer = GetModuleHandleA("libSceVideoOut.prx")) {
        fn = reinterpret_cast<DiagRecordFn>(reinterpret_cast<void*>(GetProcAddress(renderer, "RyGtDiagRecordNs")));
        if (fn != nullptr) found.store(fn, std::memory_order_release);
    }
    return fn;
}
#endif

bool KernelDiagEnabled() {
#ifdef _WIN32
    static const bool enabled = [] {
        const char* env = std::getenv("RYGT_DIAG");
        return env != nullptr && env[0] == '1';
    }();
    return enabled;
#else
    return false;
#endif
}

void KernelDiagRecord(KernelDiagZone zone, std::uint64_t nanos, const void* caller) {
#ifdef _WIN32
    if (!KernelDiagEnabled()) return;
    if (const DiagRecordFn fn = RendererDiag()) {
        static const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto address = reinterpret_cast<std::uintptr_t>(caller);
        fn(static_cast<std::uint32_t>(zone), nanos, address > image ? address - image : address);
    }
#endif
}

void KernelDiagMark(KernelDiagZone zone, std::uint64_t arg) {
#ifdef _WIN32
    if (!KernelDiagEnabled()) return;
    if (const DiagRecordFn fn = RendererDiag()) fn(static_cast<std::uint32_t>(zone), 0, arg);
#else
    (void)zone;
    (void)arg;
#endif
}

std::uint64_t KernelDiagWaker(const void* caller) {
#ifdef _WIN32
    static const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto address = reinterpret_cast<std::uintptr_t>(caller);
    const std::uint64_t offset = address > image && address - image < 0xFFFFFFFFull ? address - image : 0;
    return (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32) | offset;
#else
    (void)caller;
    return 0;
#endif
}

}  // extern "C++"

void KernelTraceWait_nid_postfix(const char* kind, const void* caller, std::uint64_t waitedNanos, bool timedOut) {
    if (KernelDiagEnabled()) {
        const KernelDiagZone zone = kind[0] == 'e' ? (kind[1] == 'q' ? KernelDiagZone::Equeue : KernelDiagZone::EventFlag)
                                  : kind[0] == 's' ? KernelDiagZone::Sema : KernelDiagZone::Cond;
        KernelDiagRecord(zone, waitedNanos, caller);
    }
    static const bool enabled = std::getenv("APS5_TRACE_WAITS") != nullptr;
    if (!enabled) return;
    struct Site { std::uint64_t calls = 0; std::uint64_t timeouts = 0; std::uint64_t nanos = 0; std::uint64_t longest = 0; unsigned long lastThread = 0; };
    static std::mutex mutex;
    static std::unordered_map<std::string, Site> sites;
    static std::uint64_t lastReport = 0;
    char key[96];
#ifdef _WIN32
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const unsigned long thread = GetCurrentThreadId();
#else
    const std::uintptr_t image = 0;
    const unsigned long thread = 0;
#endif
    std::snprintf(key, sizeof(key), "%s exe+0x%llx", kind, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(caller) - image));
    std::lock_guard lock(mutex);
    auto& site = sites[key];
    ++site.calls;
    site.timeouts += timedOut ? 1u : 0u;
    site.nanos += waitedNanos;
    site.longest = std::max(site.longest, waitedNanos);
    site.lastThread = thread;
    const auto now = GetMonotonicNanos();
    if (lastReport == 0) lastReport = now;
    if (now - lastReport < 3000000000ULL) return;
    lastReport = now;
    std::vector<std::pair<std::string, Site>> hot(sites.begin(), sites.end());
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second.nanos > b.second.nanos; });
    std::fprintf(stderr, "[waits]");
    for (std::size_t i = 0; i < hot.size() && i < 8; ++i) {
        const auto& s = hot[i].second;
        std::fprintf(stderr, " | %s x%llu %.1fs (avg %.1f ms, max %.0f ms, %llu timeouts, tid %lu)", hot[i].first.c_str(), static_cast<unsigned long long>(s.calls), s.nanos / 1e9, s.nanos / 1e6 / static_cast<double>(s.calls), s.longest / 1e6, static_cast<unsigned long long>(s.timeouts), s.lastThread);
    }
    std::fprintf(stderr, "\n");
    for (auto& [name, s] : sites) s = Site{};
}

int APS5_VABI sceKernelUsleep_nid_postfix(KernelUseconds microseconds) {
    TraceSleep(__builtin_return_address(0), microseconds);
    KernelDiagScope diag(KernelDiagZone::Sleep, __builtin_return_address(0));
    TimedWait::SleepNanos(static_cast<std::uint64_t>(microseconds) * 1000ULL);
    return 0;
}

int APS5_VABI sceKernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp) {
    if (rqtp == nullptr) {
        return -1;
    }
    if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1000000000LL) {
        return -1;
    }
    KernelDiagScope diag(KernelDiagZone::Sleep, __builtin_return_address(0));
    TimedWait::SleepNanos(static_cast<std::uint64_t>(rqtp->tv_sec) * 1000000000ULL +
                          static_cast<std::uint64_t>(rqtp->tv_nsec));
    if (rmtp != nullptr) {
        rmtp->tv_sec = 0;
        rmtp->tv_nsec = 0;
    }
    return 0;
}

int APS5_VABI nanosleep_nid_postfix(const KernelTimespec* rqtp, KernelTimespec* rmtp) {
    return sceKernelNanosleep(rqtp, rmtp);
}

int APS5_VABI _nanosleep_nid_postfix(const KernelTimespec* rqtp, KernelTimespec* rmtp) {
    return sceKernelNanosleep(rqtp, rmtp);
}

int APS5_VABI usleep_nid_postfix(KernelUseconds microseconds) {
    KernelDiagScope diag(KernelDiagZone::Sleep, __builtin_return_address(0));
    TimedWait::SleepNanos(static_cast<std::uint64_t>(microseconds) * 1000ULL);
    return 0;
}

int APS5_VABI clock_gettime_nid_postfix(int clockId, KernelTimespec* tp) {
    if (tp == nullptr) {
        APS5_INVALID_ARG_EX;
    }
#ifdef _WIN32
    if (clockId == 0 || clockId == 9) {
        FILETIME ft{};
        GetSystemTimePreciseAsFileTime(&ft);
        std::uint64_t t = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        t -= 116444736000000000ULL;
        t *= 100ULL;
        tp->tv_sec = static_cast<std::int64_t>(t / 1000000000ULL);
        tp->tv_nsec = static_cast<std::int64_t>(t % 1000000000ULL);
        return 0;
    }
    if (clockId == 10 || clockId == 13) {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        std::uint64_t t = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        t -= 116444736000000000ULL;
        t *= 100ULL;
        tp->tv_sec = static_cast<std::int64_t>(t / 1000000000ULL);
        tp->tv_nsec = static_cast<std::int64_t>(t % 1000000000ULL);
        return 0;
    }
    if (clockId == 4 || clockId == 1 || clockId == 5 || clockId == 7 || clockId == 8 || clockId == 11 || clockId == 12) {
        std::uint64_t nanos = GuestMonotonicNanos();
        tp->tv_sec = static_cast<std::int64_t>(nanos / 1000000000ULL);
        tp->tv_nsec = static_cast<std::int64_t>(nanos % 1000000000ULL);
        return 0;
    }
    throw std::runtime_error(std::string(__func__) + ": unsupported clock_id " + std::to_string(clockId));
#else
    clockid_t nativeId;
    switch (clockId) {
        case 0:
        case 9:
            nativeId = CLOCK_REALTIME;
            break;
        case 10:
        case 13:
#ifdef CLOCK_REALTIME_COARSE
            nativeId = CLOCK_REALTIME_COARSE;
#else
            nativeId = CLOCK_REALTIME;
#endif
            break;
        case 4:
        case 7:
        case 11:
            nativeId = CLOCK_MONOTONIC;
            break;
        case 5:
        case 8:
        case 12:
            nativeId = CLOCK_MONOTONIC;
            break;
        case 14:
            nativeId = CLOCK_THREAD_CPUTIME_ID;
            break;
        case 15:
            nativeId = CLOCK_PROCESS_CPUTIME_ID;
            break;
        default:
            throw std::runtime_error(std::string(__func__) + ": unsupported clock_id " + std::to_string(clockId));
    }
    struct timespec ts{};
    if (clock_gettime(nativeId, &ts) != 0) {
        return -1;
    }
    tp->tv_sec = static_cast<std::int64_t>(ts.tv_sec);
    tp->tv_nsec = static_cast<std::int64_t>(ts.tv_nsec);
    return 0;
#endif
}

int APS5_VABI gettimeofday_nid_postfix(KernelTimeval* tv, KernelTimezone* tz) {
    if (tv == nullptr) {
        APS5_INVALID_ARG_EX;
    }
#ifdef _WIN32
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    std::uint64_t t = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    tv->tv_sec = static_cast<std::int64_t>(t / 10000000ULL);
    tv->tv_usec = static_cast<std::int64_t>((t % 10000000ULL) / 10ULL);
#else
    struct timeval native{};
    if (gettimeofday(&native, nullptr) != 0) {
        return -1;
    }
    tv->tv_sec = static_cast<std::int64_t>(native.tv_sec);
    tv->tv_usec = static_cast<std::int64_t>(native.tv_usec);
#endif
    if (tz != nullptr) {
#ifdef _WIN32
        WindowsLocalClock clock{};
        if (!WindowsClockFromUtc(tv->tv_sec, clock)) { errno = EINVAL; return -1; }
        *tz = WindowsGuestTimezone(clock);
#else
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
#endif
    }
    return 0;
}

int APS5_VABI clock_getres_nid_postfix(int clockId, KernelTimespec* res) {
    if (res == nullptr) {
        APS5_INVALID_ARG_EX;
    }
#ifdef _WIN32
    if (clockId == 0 || clockId == 9) {
        res->tv_sec = 0;
        res->tv_nsec = 100LL;
        return 0;
    }
    if (clockId == 4 || clockId == 1 || clockId == 5 || clockId == 7 || clockId == 8 || clockId == 10 || clockId == 11 || clockId == 12 || clockId == 13) {
        static const std::uint64_t freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return static_cast<std::uint64_t>(f.QuadPart);
        }();
        std::uint64_t nsPerTick = (1000000000ULL + freq - 1ULL) / freq;
        res->tv_sec = 0;
        res->tv_nsec = static_cast<std::int64_t>(nsPerTick);
        return 0;
    }
    throw std::runtime_error(std::string(__func__) + ": unsupported clock_id " + std::to_string(clockId));
#else
    clockid_t nativeId;
    switch (clockId) {
        case 0:
        case 9:
            nativeId = CLOCK_REALTIME;
            break;
        case 10:
        case 13:
#ifdef CLOCK_REALTIME_COARSE
            nativeId = CLOCK_REALTIME_COARSE;
#else
            nativeId = CLOCK_REALTIME;
#endif
            break;
        case 4:
        case 7:
        case 11:
            nativeId = CLOCK_MONOTONIC;
            break;
        case 5:
        case 8:
        case 12:
            nativeId = CLOCK_MONOTONIC;
            break;
        case 14:
            nativeId = CLOCK_THREAD_CPUTIME_ID;
            break;
        case 15:
            nativeId = CLOCK_PROCESS_CPUTIME_ID;
            break;
        default:
            throw std::runtime_error(std::string(__func__) + ": unsupported clock_id " + std::to_string(clockId));
    }
    struct timespec ts{};
    if (clock_getres(nativeId, &ts) != 0) {
        return -1;
    }
    res->tv_sec = static_cast<std::int64_t>(ts.tv_sec);
    res->tv_nsec = static_cast<std::int64_t>(ts.tv_nsec);
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Moved as-is (not yet implemented) from the monolithic libkernel/Export.cpp.
// ---------------------------------------------------------------------------

// Wall-clock timestamps remain UTC. Windows local-time conversions follow the host time zone.
static constexpr int SceKernelErrorEfault = static_cast<int>(0x8002000e);

int APS5_VABI sceKernelClockGetres(KernelClockid clock_id, KernelTimespec* tp) {
    if (tp == nullptr) return SceKernelErrorEfault;
    return clock_getres_nid_postfix(static_cast<int>(clock_id), tp);
}

int APS5_VABI sceKernelClockGettime(KernelClockid clock_id, KernelTimespec* tp) {
    if (tp == nullptr) return SceKernelErrorEfault;
    return clock_gettime_nid_postfix(static_cast<int>(clock_id), tp);
}

int APS5_VABI sceKernelConvertLocaltimeToUtc(int64_t local_time, int64_t reserved, int64_t* utc_time, KernelTimezone* timezone, int32_t* dst_seconds) {
    (void)reserved;
#ifdef _WIN32
    WindowsLocalClock clock{};
    if (!WindowsClockFromLocal(local_time, clock)) return static_cast<int>(0x80020016);
    if (utc_time != nullptr) *utc_time = clock.utc;
    if (timezone != nullptr) *timezone = WindowsGuestTimezone(clock);
    if (dst_seconds != nullptr) *dst_seconds = clock.daylightOffset;
#else
    if (utc_time != nullptr) *utc_time = local_time;
    if (timezone != nullptr) *timezone = {0, 0};
    if (dst_seconds != nullptr) *dst_seconds = 0;
#endif
    return 0;
}

int APS5_VABI sceKernelConvertUtcToLocaltime(int64_t utc_time, int64_t* local_time, KernelTimesec* st, uint64_t* dst_sec) {
#ifdef _WIN32
    WindowsLocalClock clock{};
    if (!WindowsClockFromUtc(utc_time, clock)) return static_cast<int>(0x80020016);
    if (local_time != nullptr) *local_time = clock.local;
    if (st != nullptr) *st = {utc_time, static_cast<std::uint32_t>(clock.standardOffset), static_cast<std::uint32_t>(clock.daylightOffset)};
    if (dst_sec != nullptr) *dst_sec = static_cast<std::uint64_t>(clock.daylightOffset);
#else
    if (local_time != nullptr) *local_time = utc_time;
    if (st != nullptr) *st = {utc_time, 0u, 0u};
    if (dst_sec != nullptr) *dst_sec = 0;
#endif
    return 0;
}

int APS5_VABI sceKernelGettimeofday(KernelTimeval* tp) {
    if (tp == nullptr) return SceKernelErrorEfault;
    return gettimeofday_nid_postfix(tp, nullptr);
}

int APS5_VABI sceKernelGettimezone(KernelTimezone* tz) {
    if (tz == nullptr) return SceKernelErrorEfault;
#ifdef _WIN32
    KernelTimeval now{};
    if (gettimeofday_nid_postfix(&now, tz) != 0) return static_cast<int>(0x80020016);
#else
    *tz = {0, 0};
#endif
    return 0;
}

uint64_t APS5_VABI sceKernelReadTsc(void) {
#ifdef _WIN32
    if (const GuestTscFn guestTsc = RendererGuestTsc()) return guestTsc();
#endif
    if (TimeScale() == 1.0) return __rdtsc();
    static const std::uint64_t origin = __rdtsc();
    return origin + static_cast<std::uint64_t>(static_cast<double>(__rdtsc() - origin) * TimeScale());
}

uint64_t APS5_VABI sceKernelGetTscFrequency(void) {
    static const std::uint64_t frequency = [] {
        const std::uint64_t startNanos = RawMonotonicNanos();
        const std::uint64_t startTicks = __rdtsc();
        TimedWait::SleepNanos(20000000ULL);
        const std::uint64_t elapsedNanos = RawMonotonicNanos() - startNanos;
        const std::uint64_t elapsedTicks = __rdtsc() - startTicks;
        return static_cast<std::uint64_t>(static_cast<long double>(elapsedTicks) * 1000000000.0L / static_cast<long double>(elapsedNanos));
    }();
    return frequency;
}

unsigned int APS5_VABI sceKernelSleep(unsigned int seconds) {
    TimedWait::SleepNanos(static_cast<std::uint64_t>(seconds) * 1000000000ULL);
    return 0;
}

}
