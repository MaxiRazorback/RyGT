#include <windows.h>
#include <cstdio>
static long long tick{};
static BOOL WINAPI TestClock(LARGE_INTEGER* value) { value->QuadPart = (tick += 1000); return TRUE; }
static BOOL WINAPI TestFrequency(LARGE_INTEGER* value) { value->QuadPart = 120000; return TRUE; }
#define QueryPerformanceCounter TestClock
#define QueryPerformanceFrequency TestFrequency
#include "ryty_gpu/ryty_camera_runtime.cpp"
#undef QueryPerformanceCounter
#undef QueryPerformanceFrequency
#include "ryty_gpu/ryty_camera_input.h"
namespace Ryty::Camera { bool OverlayOpen() { return true; } }
using namespace Ryty::Camera;
static std::uintptr_t __attribute__((sysv_abi)) Original(void*,void*,void*,float* view,std::uintptr_t) {
    view[0]=100;view[1]=11;view[2]=300;view[3]=1;view[4]=view[5]=view[6]=0;view[7]=1;view[22]=58;return 1;
}
static int __attribute__((sysv_abi)) Ray(void*,float* plane,const float*,const float*) { plane[1]=1;plane[3]=-10;return 0; }
static void Jump(std::uintptr_t at,const void* target) {
    unsigned char code[]{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};std::memcpy(code+2,&target,8);std::memcpy((void*)at,code,sizeof(code));
    DWORD old{};VirtualProtect((void*)(at&~4095ull),4096,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),(void*)at,sizeof(code));
}
int main() {
    int failures{};auto check=[&](bool ok,const char* message) {std::printf("%s %s\n",ok?"PASS":"FAIL",message);failures+=!ok;};
    image=(std::uintptr_t)VirtualAlloc(nullptr,0x3200000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!image)return 2;
    std::array<unsigned char,0x25c0> root{};std::array<unsigned char,0x680> course{};
    void* object=root.data();void* organizer=&object;*(void**)(image+0x2bf5e48)=organizer;
    *(std::uintptr_t*)root.data()=image+0x2854348;*(void**)(root.data()+0x25b0)=course.data();
    *(std::uintptr_t*)(course.data()+0x660)=image+0x2896278;*(void**)(course.data()+0x668)=root.data();Jump(image+0xa91990,(void*)&Ray);
    auto& hook=hooks[1];hook.original=(std::uintptr_t)&Original-image;
    const auto measure=[&](float sensitivity,int raw_x,int raw_y,bool invert_x=false,bool invert_y=false,bool custom=false) {
        ResetData(hook);hook.car=0;hook.last_frame=0;tick=0;
        Settings settings;settings.orbit=true;settings.sensitivity=sensitivity;settings.smoothing_ms=0;settings.auto_return=false;
        settings.invert_x=invert_x;settings.invert_y=invert_y;
        Presets().SetGlobal(View::Chase,settings);
        if(custom) {PublishIdentity(2148);PublishIdentity(2148);PublishIdentity(2148);Presets().SetCar(2148,View::Chase,settings);}
        else Presets().UseGlobal(2148,View::Chase);
        for(int frame=0;frame<13;++frame) {
            std::array<float,64> view{};view[0]=100;view[1]=11;view[2]=300;view[7]=1;
            PublishStick(NormalizeOrbitAxis(raw_x),NormalizeOrbitAxis(raw_y),true);CameraView<View::Chase>((void*)1,nullptr,nullptr,view.data(),0);
        }
        const auto* p=(const float*)hook.data.data();
        std::printf("MEASURE sensitivity=%.3f x=%d y=%d invert=%d/%d custom=%d yaw=%.6f pitch=%.6f velocity=%.6f/%.6f\n",sensitivity,raw_x,raw_y,invert_x,invert_y,custom,p[38],p[39],p[40],p[41]);
        return std::array<float,4>{p[38]>180?p[38]-360:p[38],p[39],p[40],p[41]};
    };
    check(Settings{}.sensitivity==0,"zero is the actual new/reset default");
    const float sensitivities[]{-15,-10,-5,0,5,10,15},speeds[]{30,60,90,120,160,200,240};
    for(unsigned i=0;i<7;++i) {
        const float value=sensitivities[i],speed=speeds[i];
        auto x=measure(value,255,128),y=measure(value,128,255);
        check(std::abs(x[2]+speed)<.002f && x[0]<0 && x[3]==0,"right stick uses corrected default X direction at every sensitivity without moving Y");
        auto left=measure(value,0,128);
        check(std::abs(left[2]-speed)<.002f && left[0]>0 && left[3]==0,"left stick uses the opposite X direction at every sensitivity without moving Y");
        check(std::abs(y[3]+speed)<.002f && y[1]<0 && y[2]==0,"sensitivity changes Y speed without reversing direction or moving X");
        check(std::abs(x[0]+speed*.2f)<.002f && std::abs(y[1]+speed*.2f)<.002f,"both camera angles integrate at the expected bounded speed");
    }
    for(float value:{-15.f,0.f,15.f}) {
        auto normal=measure(value,255,255),ix=measure(value,255,255,true,false),iy=measure(value,255,255,false,true),both=measure(value,255,255,true,true);
        check(ix[2]==-normal[2] && ix[3]==normal[3],"Invert X reverses only X at negative/default/positive sensitivity");
        check(iy[2]==normal[2] && iy[3]==-normal[3],"Invert Y reverses only Y at negative/default/positive sensitivity");
        check(both[2]==-normal[2] && both[3]==-normal[3],"both invert switches work independently from sensitivity");
        auto custom=measure(value,255,255,false,false,true);
        check(custom[2]==normal[2] && custom[3]==normal[3],"per-car sensitivity uses the same direction-preserving scale as global settings");
    }
    auto small=measure(0,144,128);
    check(std::abs(small[2]+.8160336f)<.002f,"default small-stick curve retains gentle response with corrected X direction");
    auto stationary=measure(0,128,128);
    check(stationary[0]==0 && stationary[1]==0 && stationary[2]==0 && stationary[3]==0,"neutral stick remains stationary at default sensitivity");
    VirtualFree((void*)image,0,MEM_RELEASE);return failures?1:0;
}
