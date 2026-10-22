#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using u8=unsigned char;
using TelemetryUpdateFn=void(__attribute__((sysv_abi)) *)(void*,void*);
static TelemetryUpdateFn g_telemetry_update{};
static void __attribute__((sysv_abi)) TelemetryUpdateHook(void*,void*) {}
// Generated verbatim from the reviewed integration, compared with upstream.
#include <trigger_install_under_test.inc>
int main() {
    auto* base=static_cast<u8*>(VirtualAlloc(nullptr,0x2860000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!base)return 2;
    auto* slot=reinterpret_cast<TelemetryUpdateFn*>(base+0x2858390);
    const auto original=reinterpret_cast<TelemetryUpdateFn>(base+0x68cf20);
    const u8 magic[]{0xc7,0x87,0xdc,0x01,0,0,0x30,0x53,0x37,0x47};std::memcpy(base+0x68cf53,magic,sizeof magic);
    int errors=0;
    const char* values[]{"","0","0disabled","1","false"};
    for(auto value:values){_putenv_s("RYGT_ADAPTIVE_TRIGGERS",value);*slot=original;g_telemetry_update=nullptr;
        InstallTriggerTelemetry(base);const bool expected=value[0]!='0';
        const bool ok=expected?(*slot==&TelemetryUpdateHook && g_telemetry_update==original):(*slot==original && !g_telemetry_update);
        std::printf("%s RYGT_ADAPTIVE_TRIGGERS='%s' hook=%d\n",ok?"PASS":"FAIL",value,*slot!=original);errors+=!ok;}
    _putenv_s("RYGT_ADAPTIVE_TRIGGERS","");*slot=original;base[0x68cf53]=0;InstallTriggerTelemetry(base);
    const bool rejected=*slot==original;std::printf("%s incompatible executable guard\n",rejected?"PASS":"FAIL");errors+=!rejected;
    VirtualFree(base,0,MEM_RELEASE);return errors?1:0;
}
