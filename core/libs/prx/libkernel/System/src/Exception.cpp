#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#endif

#ifdef _WIN32
namespace {
// RyGT: the game calls sceKernelDebugRaiseExceptionOnReleaseMode when it gives up (after shutting
// its own libraries down), and nothing else in the logs says why it closed. Say so, with the
// GTSport.exe return addresses found on the stack, so a player's log shows where it decided to.
void ReportGameFatal(int c1, int c2) {
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe + dos->e_lfanew);
    const std::uintptr_t end = exe + nt->OptionalHeader.SizeOfImage;
    char line[1024];
    int length = std::snprintf(line, sizeof(line), "[fatal] the game ended itself (sceKernelDebugRaiseExceptionOnReleaseMode %d, %d); GTSport.exe on the stack:", c1, c2);
    const auto* slot = static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(slot, &region, sizeof(region)) == sizeof(region)) {
        const auto* limit = reinterpret_cast<const std::uintptr_t*>(static_cast<const char*>(region.BaseAddress) + region.RegionSize);
        for (unsigned found = 0; slot < limit && found < 16 && length < static_cast<int>(sizeof(line)) - 24; ++slot) {
            if (*slot > exe && *slot < end) {
                length += std::snprintf(line + length, sizeof(line) - length, " +0x%llx", static_cast<unsigned long long>(*slot - exe));
                ++found;
            }
        }
    }
    std::fprintf(stderr, "%s\n", line);
}
}
#endif

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 (void)signum;
 (void)handler;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 (void)signum;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 (void)thread;
 (void)signum;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
 (void)c1;
 (void)c2;
 NotImplemented_nid_no_patch(__func__);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
#ifdef _WIN32
 ReportGameFatal(c1, c2);
#else
 (void)c1;
 (void)c2;
#endif
}

}
