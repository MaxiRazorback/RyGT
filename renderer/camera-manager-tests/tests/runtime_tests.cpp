#include "ryty_gpu/ryty_camera_runtime.cpp"
#include <cstdio>
namespace Ryty::Camera { bool overlay_visible{}; bool OverlayOpen() { return overlay_visible; } }
using namespace Ryty::Camera;
static unsigned called = 0, ray_calls = 0; static bool args_ok = true; static float road = 10;
static std::uintptr_t __attribute__((sysv_abi)) Original(void* a, void* b, void* c, float* v, std::uintptr_t flags) {
    args_ok &= a == (void*)1 && b == (void*)2 && c == (void*)3 && flags == 7;
    ++called; v[0] = 100; v[1] = 11; v[2] = 300; v[3] = 1;
    v[4] = v[5] = v[6] = 0; v[7] = 1; v[22] = 58;
    return 0x12345678;
}
static int __attribute__((sysv_abi)) Ray(void*, float* plane, const float*, const float*) { ++ray_calls; plane[0]=plane[2]=0; plane[1]=1; plane[3]=-road; return 0; }
static std::uint64_t __attribute__((sysv_abi)) Name(const char* s) { return s[0]; }
static unsigned selected{};
static unsigned native_id = 2148; static bool prepare_args_ok = false;
static int __attribute__((sysv_abi)) Count(void*) { return 1; }
static std::array<unsigned char,16> car_status{};
static void* __attribute__((sysv_abi)) CarStatus(void*, int index) { return index == 0 ? car_status.data() : nullptr; }
static unsigned __attribute__((sysv_abi)) CarCode(void*) { return native_id; }
static std::uintptr_t __attribute__((sysv_abi)) Prepare(void* camera, float*, void*, void* input, float dt) {
    prepare_args_ok = camera == (void*)1 && input == (void*)4 && dt == .125f; return 1;
}
static void __attribute__((sysv_abi)) Select(void*, int window, std::uint64_t code, bool force) { if (!window && force) selected = static_cast<unsigned>(code); }
static void Jump(std::uintptr_t at, const void* destination) {
    unsigned char bytes[]{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0}; std::memcpy(bytes+2,&destination,8); std::memcpy((void*)at,bytes,sizeof(bytes));
    DWORD ignored; VirtualProtect((void*)(at & ~4095ull),4096,PAGE_EXECUTE_READ,&ignored); FlushInstructionCache(GetCurrentProcess(),(void*)at,sizeof(bytes));
}
int main() {
    int errors = 0; auto check = [&](bool ok, const char* name) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); errors += !ok; };
    image = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, 0x3200000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)); if (!image) return 2;
    std::array<unsigned char, 0x25c0> root{}; std::array<unsigned char, 0x680> course{};
    void* rp = root.data(); void* organizer = &rp; *reinterpret_cast<void**>(image + 0x2bf5e48) = organizer;
    *reinterpret_cast<std::uintptr_t*>(root.data()) = image + 0x2854348;
    *reinterpret_cast<void**>(root.data()+0x25b0) = course.data();
    *reinterpret_cast<std::uintptr_t*>(course.data()+0x660) = image + 0x2896278; *reinterpret_cast<void**>(course.data()+0x668) = root.data();
    Jump(image+0xa91990,(void*)&Ray); Jump(image+0x866320,(void*)&Name); Jump(image+0x8657b0,(void*)&Select);
    for(auto& hook:hooks) { hook.original = reinterpret_cast<std::uintptr_t>(&Original) - image; ResetData(hook); }
    PublishIdentity(2148); PublishIdentity(2148); check(CurrentCar()==0,"car identity waits for consistent samples");
    PublishIdentity(2148); check(CurrentCar()==2148,"native car ID resolves against 340-car catalog");
    std::array<float,64> v{}; v[0]=100; v[1]=11; v[2]=300;
    auto run=[&](View view){
        using Fn = ViewFunction; const Fn methods[]{&CameraView<View::Cockpit>,&CameraView<View::Chase>,&CameraView<View::Bumper>,&CameraView<View::Roof>};
        auto before=called; auto result=methods[static_cast<unsigned>(view)]((void*)1,(void*)2,(void*)3,v.data(),7);
        check(result==0x12345678 && called==before+1 && args_ok,"original SysV arguments and return preserved exactly once");
    };
    run(View::Cockpit); check(v[22]==58 && v[1]==11,"fresh settings preserve native cockpit defaults");
    run(View::Chase); check(v[0]==100 && v[1]==11 && v[2]==300 && v[22]==58,"unmodified chase passes native position and FOV through unchanged");
    auto rays_before=ray_calls;run(View::Bumper);run(View::Roof);
    check(ray_calls==rays_before && v[1]==11,"default bumper/roof with closed UI perform no raycast and preserve native view");
    overlay_visible=true;run(View::Bumper);run(View::Roof);overlay_visible=false;
    check(ray_calls==rays_before+2 && MinimumHeight(View::Roof)>-5,"opening UI retains live terrain-derived minimum for default views");
    Settings s; s.fov=90; s.height=0.3f; s.distance=-0.5f; Presets().SetGlobal(View::Cockpit,s);
    run(View::Cockpit); check(v[22]==90 && std::abs(v[1]-11.3f)<1e-5 && v[2]==300.5f && NativeFov(View::Cockpit)==58,"cockpit custom FOV and signed position preserve observed native FOV");
    s.height=-5; s.distance=0; Presets().SetGlobal(View::Bumper,s); run(View::Bumper);
    check(std::abs(v[1]-10.45f)<1e-5 && v[2]==300 && v[22]==90,"bumper height constrained to road plus clearance");
    s.height=0.2f;s.distance=1;Presets().SetGlobal(View::Roof,s); run(View::Roof);
    check(std::abs(v[1]-11.2f)<1e-5 && v[2]==299,"positive roof distance moves forward and positive height moves up");
    s.height=0;s.distance=0;s.orbit=true;Presets().SetGlobal(View::Chase,s);PublishStick(1,0,true);
    run(View::Chase); Sleep(20); run(View::Chase);
    check(hooks[1].data[40] && reinterpret_cast<float*>(hooks[1].data.data())[38]>0,"right-stick orbit uses the tested smooth motion helper");
    PublishIdentity(1);check(CurrentCar()==0,"unknown ID clears old vehicle immediately");
    s.fov=77; s.height=0.4f; s.distance=0; Presets().SetGlobal(View::Cockpit,s);
    run(View::Cockpit);check(v[22]==77 && std::abs(v[1]-11.4f)<1e-5,"global camera edits work without recognized car identity");
    s.fov=81; s.height=-5; Presets().SetGlobal(View::Bumper,s);run(View::Bumper);
    check(v[22]==81 && std::abs(v[1]-10.45f)<1e-5,"unknown identity preserves global bumper controls and road clearance");
    PublishIdentity(0);check(CurrentCar()==0,"leaving race does not reuse stale car preset");
    for(int i=0;i<3;++i)PublishIdentity(2148);
    std::array<unsigned char,0x10> ctrl{};std::array<unsigned char,0x310> impl{};std::array<unsigned char,0x10> data{};std::array<unsigned char,0x48> di{};std::array<unsigned char,0x88> sw{};std::array<unsigned char,0x18> list{};std::array<std::uint64_t,16> entries{};
    *reinterpret_cast<std::uintptr_t*>(ctrl.data())=image+0x2877ae8;*reinterpret_cast<void**>(ctrl.data()+8)=impl.data();
    *reinterpret_cast<void**>(impl.data()+0x98)=ctrl.data();
    *reinterpret_cast<void**>(impl.data()+0xe0)=data.data();*reinterpret_cast<void**>(data.data()+8)=di.data();*reinterpret_cast<void**>(di.data()+0x40)=sw.data();
    *reinterpret_cast<std::uintptr_t*>(data.data())=image+0x28785f8;*reinterpret_cast<void**>(sw.data()+0x30)=list.data();
    entries[0]='i';entries[4]='c';entries[8]='d';entries[12]='b';*reinterpret_cast<void**>(list.data()+8)=entries.data();*reinterpret_cast<void**>(list.data()+16)=entries.data()+entries.size();
    std::array<unsigned char,16> race{};std::array<unsigned char,0x70> context{};
    *reinterpret_cast<void**>(race.data()) = reinterpret_cast<void*>(image+0x2000000);
    *reinterpret_cast<void**>(car_status.data()) = reinterpret_cast<void*>(image+0x2001000);
    *reinterpret_cast<void**>(context.data()+8)=race.data();
    *reinterpret_cast<void**>(context.data()+0x28)=impl.data()+0x48;
    *reinterpret_cast<void**>(image+0x2000028)=reinterpret_cast<void*>(image+0x501000);
    *reinterpret_cast<void**>(image+0x2000058)=reinterpret_cast<void*>(image+0x502000);
    *reinterpret_cast<void**>(image+0x20010d8)=reinterpret_cast<void*>(image+0x503000);
    Jump(image+0x501000,(void*)&Count);Jump(image+0x502000,(void*)&CarStatus);Jump(image+0x503000,(void*)&CarCode);
    Jump(image+0x8a5560,(void*)&Prepare);
    v[0xbc/4] = 0;v[0xc4/4] = 0;
    current_car.store(0);candidate_car=0;candidate_count=0;
    for(int i=0;i<3;++i)check(PrepareView((void*)1,v.data(),context.data(),(void*)4,.125f)==1,"prepare return preserved");
    check(prepare_args_ok && CurrentCar()==2148 && native_samples==3,"active native camera identifies car without any telemetry or script reference");
    overlay_visible=true;
    run(View::Roof);check(CurrentView()==View::Roof,"native Roof camera callback publishes active view while overlay is open");
    run(View::Chase);check(CurrentView()==View::Chase,"controller camera changes can synchronize UI without a UI request");
    const auto old_timestamp=view_updated.load();view_updated.store(Milliseconds()-1501);
    check(CurrentView()==View::Count,"inactive camera observation expires outside a race");view_updated.store(old_timestamp);
    PublishStick(1,0,true);run(View::Chase);Sleep(20);run(View::Chase);
    check(hooks[1].data[127] && reinterpret_cast<float*>(hooks[1].data.data())[38]>0,"live right-stick orbit remains active with overlay open");
    overlay_visible=false;
    runtime_ready=true;RequestView(View::Roof);ProcessViewRequest();check(selected=='b',"UI selection routes the named roof scenario through native selector");
    selected=0;RequestView(View::Bumper);ProcessViewRequest();check(selected=='d',"bumper routes to native driver scenario");
    selected=0;RequestView(View::Cockpit);native_id=1;ObserveContext((void*)1,v.data(),context.data());ProcessViewRequest();check(selected==0,"queued car-bound selection discarded after identity is lost");
    selected=0;RequestView(View::Chase);run(View::Cockpit);check(selected=='c',"unknown vehicle does not block selection dispatched by onboard camera callback");
    selected=0;RequestView(View::Roof);request.timestamp=Milliseconds()-2001;run(View::Cockpit);check(selected==0,"stale selection does not execute when camera callbacks resume");
    selected=0;ProcessViewRequest();check(selected==0 && !request_pending.load(),"empty request fast path does not repeat the previous selection");
    native_id=2148;for(int i=0;i<3;++i)ObserveContext((void*)1,v.data(),context.data());
    auto before_car=CurrentCar();*reinterpret_cast<void**>(ctrl.data()+8)=nullptr;
    ObserveContext((void*)1,v.data(),context.data());check(CurrentCar()==before_car,"mismatched owner is rejected before native methods");
    *reinterpret_cast<void**>(ctrl.data()+8)=impl.data();
    check(ExecutableHash(GamePath()).size()==64,"executable guard computes complete SHA256");
    VirtualFree((void*)image,0,MEM_RELEASE); return errors?1:0;
}
