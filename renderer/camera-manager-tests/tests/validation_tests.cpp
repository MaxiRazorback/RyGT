#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
static unsigned protect_calls{}, fail_protect{}, exchange_calls{}, race_exchange{};
static BOOL WINAPI TestProtect(void* p, SIZE_T n, DWORD flags, DWORD* old) {
    if (++protect_calls == fail_protect) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return VirtualProtect(p, n, flags, old);
}
static void* TestExchange(void* volatile* slot, void* value, void* expected) {
    if (++exchange_calls == race_exchange) *slot = reinterpret_cast<unsigned char*>(expected) + 16;
    return InterlockedCompareExchangePointer(slot, value, expected);
}
#define VirtualProtect TestProtect
#undef InterlockedCompareExchangePointer
#define InterlockedCompareExchangePointer TestExchange
#include "ryty_gpu/ryty_camera_runtime.cpp"
#undef VirtualProtect
#undef InterlockedCompareExchangePointer
namespace Ryty::Camera { bool OverlayOpen() { return false; } }
using namespace Ryty::Camera;

static bool InspectFixture(std::uintptr_t base, std::uintptr_t at, std::size_t size, bool code) {
    if (!size || at < base || at - base >= 0x4000000 || size > 0x4000000 - (at - base)) return false;
    for (auto cursor = at; cursor < at + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.AllocationBase != reinterpret_cast<void*>(base) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto flags = info.Protect & 0xff;
        if (flags != PAGE_READWRITE && flags != PAGE_READONLY && flags != PAGE_EXECUTE_READ) return false;
        if (code && flags != PAGE_EXECUTE_READ) return false;
        cursor = (std::min)(at + size, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
    }
    return true;
}
struct Fixture {
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, 0x4000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Fixture() {
        if (!base) std::abort();
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base); dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
        auto& nt = Nt(); nt.Signature = IMAGE_NT_SIGNATURE; nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64); nt.FileHeader.NumberOfSections = 2;
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt.OptionalHeader.SizeOfImage = 0x3200000;
        nt.OptionalHeader.SizeOfHeaders = 0x1000;
        auto* sections = Sections(); sections[0].VirtualAddress = 0x10000; sections[0].Misc.VirtualSize = 0x2600000;
        sections[0].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
        sections[1].VirtualAddress = 0x2800000; sections[1].Misc.VirtualSize = 0xa00000;
        sections[1].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
        for (const auto& s : Validation::Signatures) std::memcpy(reinterpret_cast<void*>(base + s.rva), s.bytes.data(), s.bytes.size());
        for (const auto& s : Validation::HookSites) *reinterpret_cast<std::uintptr_t*>(base + s.slot) = base + s.original;
        ProtectSections();
    }
    ~Fixture() { VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE); }
    IMAGE_NT_HEADERS64& Nt() { return *reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew); }
    IMAGE_SECTION_HEADER* Sections() { return IMAGE_FIRST_SECTION(&Nt()); }
    void ProtectSections() {
        for (unsigned i = 0; i < Nt().FileHeader.NumberOfSections; ++i) {
            const auto& s = Sections()[i]; DWORD old{};
            const auto protection = s.Characteristics & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READ :
                s.Characteristics & IMAGE_SCN_MEM_WRITE ? PAGE_READWRITE :
                s.Characteristics & IMAGE_SCN_MEM_READ ? PAGE_READONLY : PAGE_NOACCESS;
            if (!VirtualProtect(reinterpret_cast<void*>(base + s.VirtualAddress), s.Misc.VirtualSize, protection, &old)) std::abort();
        }
    }
    template<class T> void Write(std::uintptr_t rva, const T& value) {
        auto* p = reinterpret_cast<void*>(base + rva); DWORD old{}, ignored{};
        if (!VirtualProtect(p, sizeof(T), PAGE_READWRITE, &old)) std::abort();
        std::memcpy(p, &value, sizeof(T)); VirtualProtect(p, sizeof(T), old, &ignored);
    }
    std::string Check() { return Validation::Validate(base, Validation::HookSites, InspectFixture); }
    bool OriginalSlots() {
        for (const auto& s : Validation::HookSites) if (*reinterpret_cast<std::uintptr_t*>(base+s.slot) != base+s.original) return false;
        return true;
    }
    void Load(const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary); if (!input) std::abort();
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data()+dos->e_lfanew);
        if (nt->OptionalHeader.SizeOfImage > 0x4000000 || nt->OptionalHeader.SizeOfHeaders > bytes.size()) std::abort();
        DWORD old{}; VirtualProtect(reinterpret_cast<void*>(base), 0x4000000, PAGE_READWRITE, &old);
        std::memset(reinterpret_cast<void*>(base), 0, 0x4000000);
        std::memcpy(reinterpret_cast<void*>(base), bytes.data(), nt->OptionalHeader.SizeOfHeaders);
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i=0; i<nt->FileHeader.NumberOfSections; ++i) {
            const auto& s=sections[i]; const auto count=(std::min)(s.SizeOfRawData,s.Misc.VirtualSize);
            if (s.PointerToRawData+std::uint64_t(count)>bytes.size() || s.VirtualAddress+std::uint64_t(count)>0x4000000) std::abort();
            std::memcpy(reinterpret_cast<void*>(base+s.VirtualAddress),bytes.data()+s.PointerToRawData,count);
        }
        const auto reloc=Nt().OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        const auto delta=base-nt->OptionalHeader.ImageBase;
        for (std::uint32_t pos=0;pos<reloc.Size;) {
            const auto* block=reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base+reloc.VirtualAddress+pos);
            if (block->SizeOfBlock<8 || block->SizeOfBlock>reloc.Size-pos) std::abort();
            const auto* entries=reinterpret_cast<const WORD*>(block+1);
            for (unsigned i=0;i<(block->SizeOfBlock-8)/2;++i)
                if (entries[i]>>12==IMAGE_REL_BASED_DIR64) *reinterpret_cast<std::uintptr_t*>(base+block->VirtualAddress+(entries[i]&0xfff))+=delta;
            pos+=block->SizeOfBlock;
        }
        ProtectSections();
    }
};
int main(int argc,char** argv) {
    unsigned errors{};
    const auto check=[&](bool ok,const char* message) {std::printf("%s %s\n",ok?"PASS":"FAIL",message);errors+=!ok;};
    Fixture f;
    check(f.Check().empty(),"known camera layout validates at a relocated image base");
    f.Nt().OptionalHeader.Subsystem=IMAGE_SUBSYSTEM_WINDOWS_CUI;
    f.Nt().FileHeader.TimeDateStamp=0x12345678;
    f.Nt().OptionalHeader.CheckSum=0x76543210;
    f.Nt().OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]={0x3100000,28};
    check(f.Check().empty(),"different subsystem, timestamp, checksum and debug metadata remain compatible");
    f.Nt().FileHeader.NumberOfSections=3;f.Nt().OptionalHeader.SizeOfImage=0x3400000;
    f.Sections()[2].VirtualAddress=0x3300000;f.Sections()[2].Misc.VirtualSize=0x1000;
    f.Sections()[2].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    check(f.Check().empty(),"additional converter startup section does not invalidate unchanged camera layout");
    f.Nt().FileHeader.NumberOfSections=2;f.Nt().OptionalHeader.SizeOfImage=0x3200000;
    for(const auto& s:Validation::Signatures) {
        const unsigned char changed=s.bytes[20]^1; f.Write(s.rva+20,changed);
        const auto error=f.Check();
        check(error.find("signature mismatch")!=std::string::npos && error.find(s.name)!=std::string::npos,"non-prologue change in every required function is rejected with its name");
        f.Write(s.rva+20,s.bytes[20]);
    }
    for(const auto& s:Validation::HookSites) {
        f.Write(s.slot,f.base+s.original+1); const auto error=f.Check();
        check(error.find("hook conflict")!=std::string::npos && error.find("expected")!=std::string::npos,"every one of eight conflicting slots is rejected with target detail");
        f.Write(s.slot,f.base+s.original);
    }
    check(!Validation::Validate(f.base,std::span(Validation::HookSites).first(7),InspectFixture).empty(),"incomplete hook plan rejected");
    auto plan=Validation::HookSites;plan[7]=plan[6];
    check(!Validation::Validate(f.base,plan,InspectFixture).empty(),"unexpected or duplicate hook layout rejected");
    auto& nt=f.Nt();const auto machine=nt.FileHeader.Machine;nt.FileHeader.Machine=IMAGE_FILE_MACHINE_I386;
    check(!f.Check().empty(),"wrong architecture rejected");nt.FileHeader.Machine=machine;
    const auto magic=nt.OptionalHeader.Magic;nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    check(!f.Check().empty(),"32-bit optional header rejected");nt.OptionalHeader.Magic=magic;
    const auto header_size=nt.OptionalHeader.SizeOfHeaders;nt.OptionalHeader.SizeOfHeaders=0x80;
    check(!f.Check().empty(),"section table outside headers rejected");nt.OptionalHeader.SizeOfHeaders=header_size;
    const auto size=nt.OptionalHeader.SizeOfImage;nt.OptionalHeader.SizeOfImage=0x1000;
    check(!f.Check().empty(),"truncated image rejected");nt.OptionalHeader.SizeOfImage=size;
    const auto sections=nt.FileHeader.NumberOfSections;nt.FileHeader.NumberOfSections=97;
    check(!f.Check().empty(),"excessive section count rejected");nt.FileHeader.NumberOfSections=sections;
    const auto flags=f.Sections()[0].Characteristics;f.Sections()[0].Characteristics=IMAGE_SCN_MEM_READ;
    check(!f.Check().empty(),"non-executable function section rejected");f.Sections()[0].Characteristics=flags|IMAGE_SCN_MEM_WRITE;
    check(!f.Check().empty(),"writable code section rejected");f.Sections()[0].Characteristics=flags;
    const auto data_flags=f.Sections()[1].Characteristics;f.Sections()[1].Characteristics|=IMAGE_SCN_MEM_EXECUTE;
    check(!f.Check().empty(),"executable data/hook section rejected");f.Sections()[1].Characteristics=data_flags;
    const auto data=f.Sections()[1].VirtualAddress;f.Sections()[1].VirtualAddress=0x10000;
    check(!f.Check().empty(),"overlapping sections rejected");f.Sections()[1].VirtualAddress=data;
    DWORD old{},ignored{};auto* code=reinterpret_cast<void*>(f.base+Validation::Signatures[0].rva);
    VirtualProtect(code,64,PAGE_NOACCESS,&old);check(!f.Check().empty(),"unreadable code page rejected without access violation");VirtualProtect(code,64,old,&ignored);
    VirtualProtect(code,64,PAGE_READONLY,&old);check(!f.Check().empty(),"non-executable function memory rejected");VirtualProtect(code,64,old,&ignored);
    VirtualProtect(code,64,PAGE_READONLY|PAGE_GUARD,&old);check(!f.Check().empty(),"guarded code page rejected without consuming guard");VirtualProtect(code,64,old,&ignored);
    auto* last=reinterpret_cast<void*>(f.base+Validation::HookSites[7].slot);
    VirtualProtect(last,8,PAGE_NOACCESS,&old);check(!f.Check().empty(),"unreadable final hook page rejected");VirtualProtect(last,8,old,&ignored);
    const auto self=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    check(Validation::InspectImage(self,self,sizeof(IMAGE_DOS_HEADER),false),"production inspector accepts mapped module headers");
    check(!Validation::InspectImage(f.base,f.base,64,false),"production inspector rejects private allocations pretending to be a module");
    check(!Validation::InspectImage(self,f.base,64,false),"production inspector rejects a different allocation");
    check(!Validation::Validate(0,Validation::HookSites).empty(),"null image rejected");
    image=f.base;
    std::vector<std::string> messages;
    const Validation::Report report=[&](const std::string& message) { messages.push_back(message); };
    f.Write(Validation::HookSites[7].slot,std::uintptr_t{});protect_calls=exchange_calls=0;
    check(!InstallCameraHooks(InspectFixture,report) && protect_calls==0 && exchange_calls==0,"invalid last slot prevents every hook write");
    check(messages.back().starts_with("VALIDATION FAILED:") && std::none_of(messages.begin(),messages.end(),[](const auto& s){return s.starts_with("HOOK INSTALLED");}),"failure log confirms no hooks installed");
    f.Write(Validation::HookSites[7].slot,f.base+Validation::HookSites[7].original);
    protect_calls=exchange_calls=0;fail_protect=5;
    check(!InstallCameraHooks(InspectFixture) && f.OriginalSlots() && status.find("rolled back")!=std::string::npos,"partial installation protection failure rolls back previous slots");
    fail_protect=0;protect_calls=exchange_calls=0;race_exchange=5;
    check(!InstallCameraHooks(InspectFixture) && status.find("changed during installation")!=std::string::npos,"concurrent hook ownership change is detected");
    bool preserved=true;
    for(unsigned i=0;i<Validation::HookSites.size();++i) {
        const auto& s=Validation::HookSites[i];const auto value=*reinterpret_cast<std::uintptr_t*>(f.base+s.slot);
        preserved &= value==f.base+s.original+(i==4?16:0);
        f.Write(s.slot,f.base+s.original);
    }
    check(preserved,"rollback preserves third-party slot while restoring our earlier writes");
    race_exchange=0;protect_calls=exchange_calls=0;
    fail_protect=4;
    check(!InstallCameraHooks(InspectFixture) && f.OriginalSlots() && status.find("restore camera hook protection")!=std::string::npos,"protection-restoration failure also rolls back the just-installed slot");
    fail_protect=0;protect_calls=exchange_calls=0;
    messages.clear();
    check(InstallCameraHooks(InspectFixture,report) && exchange_calls==8,"all eight hooks install only after full validation");
    check(std::count_if(messages.begin(),messages.end(),[](const auto& s){return s.starts_with("HOOK VALIDATED");})==8,"log contains all eight successful slot validations");
    check(std::count_if(messages.begin(),messages.end(),[](const auto& s){return s.starts_with("HOOK INSTALLED");})==8 && messages.back().starts_with("INSTALLATION SUCCESS:"),"log contains eight installed hooks and final success confirmation");
    if(argc==2) {
        Fixture actual;actual.Load(std::filesystem::path(argv[1]));
        const auto error=actual.Check();if(!error.empty())std::printf("REAL IMAGE: %s\n",error.c_str());
        check(error.empty(),"real converted GT Sport image validates after PE relocation");
        actual.Nt().OptionalHeader.Subsystem=IMAGE_SUBSYSTEM_WINDOWS_CUI;
        actual.Nt().FileHeader.TimeDateStamp^=1;
        check(actual.Check().empty(),"real image remains compatible after unrelated PE metadata changes");
    }
    std::printf("Validation failures: %u\n",errors);return errors?1:0;
}
