// Offscreen rendering of the actual production UI. Runtime data is simulated here only.
#include <d3d11.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include "ryty_gpu/ryty_camera.h"
#include <cstdio>
#include <fstream>
#include <vector>
namespace ImGui::Core { int captures{}; bool IsGamepadInputCaptured(){return captures!=0;} }
void assert_fail_debug_msg(const char* s) { std::fprintf(stderr,"ImGui assertion: %s\n",s); std::abort(); }
namespace Ryty { bool ControllerOverlayOpen(){return false;} }
namespace Ryty::Camera {
Store& Presets(){static Store s("preview-presets.json");return s;}
void InitializeRuntime(){}
std::uint32_t CurrentCar(){return 2148;}
View observed_view=View::Cockpit;
View CurrentView(){return observed_view;}
std::string CarName(std::uint32_t){return "Mazda Roadster S '15 (ID 2148)";}
std::string RuntimeStatus(){return "Camera controls ready.";}
std::string RuntimeDiagnostics(){return "Camera callbacks: 240 | Vehicle samples: 120 | Raw ID: 2148 | Flags: 1";}
float NativeFov(View){return 50;}
float MinimumHeight(View){return -0.35f;}
int selection_requests{};
void RequestView(View){++selection_requests;}
}
int main(int argc,char** argv){
    ID3D11Device* device{}; ID3D11DeviceContext* context{}; D3D_FEATURE_LEVEL level{};
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)))return 2;
    unsigned width=1280,height=800;
    for(int a=1;a<argc;++a) std::sscanf(argv[a],"--size=%ux%u",&width,&height);
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target{}; ID3D11RenderTargetView* rtv{};
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&target))||FAILED(device->CreateRenderTargetView(target,nullptr,&rtv)))return 3;
    ImGui::CreateContext(); auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={float(width),float(height)};io.DeltaTime=1.0f/60;
    // Match the renderer's default 32px font and 0.5 global factor.
    io.FontDefault=io.Fonts->AddFontFromFileTTF(CAMERA_TEST_FONT,32);
    io.FontGlobalScale=0.5f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    const bool dock_regression = argc > 1 && std::strcmp(argv[1], "--dock-regression") == 0;
    bool chase_preview=false;
    bool resize_regression=false;
    for(int a=1;a<argc;++a) if(std::strcmp(argv[a],"--chase")==0)chase_preview=true;
    for(int a=1;a<argc;++a) if(std::strcmp(argv[a],"--resize-regression")==0)resize_regression=true;
    if (dock_regression) {
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::LoadIniSettingsFromMemory("[Window][RytyCameraSettings]\nPos=0,0\nSize=1280,800\nCollapsed=0\nDockId=0x08BD597D\n\n[Docking][Data]\nDockSpace ID=0x08BD597D Window=0x1BBC0F80 Pos=0,0 Size=1280,800 CentralNode=1\n");
    }
    ImGui::StyleColorsDark();ImGui_ImplDX11_Init(device,context);
    auto s=Ryty::Camera::Settings{};s.fov=75;s.height=0.1f;s.distance=0.2f;s.orbit=true;Ryty::Camera::Presets().SetGlobal(Ryty::Camera::View::Chase,s);
    Ryty::Camera::ToggleOverlay();
    int errors=0;
    const auto save=[&](const char* name){
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D* staging{};
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging))){++errors;return;}
        context->CopyResource(staging,target);D3D11_MAPPED_SUBRESOURCE map{};
        if(FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&map))){++errors;staging->Release();return;}
        std::ofstream out(name,std::ios::binary);unsigned row=(width*3+3)&~3u,size=54+row*height;
        unsigned char header[54]{};header[0]='B';header[1]='M';std::memcpy(header+2,&size,4);header[10]=54;header[14]=40;std::memcpy(header+18,&width,4);std::memcpy(header+22,&height,4);header[26]=1;header[28]=24;out.write((char*)header,54);
        std::vector<unsigned char> pixels(row);
        for(int y=height-1;y>=0;--y){const auto* src=(const unsigned char*)map.pData+y*map.RowPitch;for(unsigned x=0;x<width;++x){pixels[x*3]=src[x*4+2];pixels[x*3+1]=src[x*4+1];pixels[x*3+2]=src[x*4];}out.write((char*)pixels.data(),pixels.size());}
        context->Unmap(staging,0);staging->Release();
    };
    // Optional clicks are ImGui input events in this offscreen context, not desktop input.
    struct Click { int frame; float x,y; bool down; };std::vector<Click> clicks;
    for(int a=1;a<argc;++a){Click c;int down;if(std::sscanf(argv[a],"%d,%f,%f,%d",&c.frame,&c.x,&c.y,&down)==4){c.down=down!=0;clicks.push_back(c);}}
    for(int frame=0;frame<24;++frame){
        const bool native_capture=frame==10 || frame==11;
        ImGui::Core::captures=native_capture?1:0;
        if(!chase_preview && frame==8)Ryty::Camera::observed_view=Ryty::Camera::View::Roof;
        if(!chase_preview && frame==16)Ryty::Camera::observed_view=Ryty::Camera::View::Chase;
        if(!chase_preview && frame==4)io.AddKeyEvent(ImGuiKey_GamepadDpadDown,true);
        if(!chase_preview && frame==7)io.AddKeyEvent(ImGuiKey_GamepadDpadDown,false);
        if(resize_regression&&frame==10)io.DisplaySize={1920,1080};
        if(chase_preview){
            if(frame==2){const auto* w=ImGui::FindWindowByName("Camera Settings###RytyCameraSettings");io.AddMousePosEvent(w->Pos.x+100*width/1280.f,w->Pos.y+94*height/800.f);io.AddMouseButtonEvent(0,true);}
            if(frame==3)io.AddMouseButtonEvent(0,false);
            if(frame==4)io.AddKeyEvent(ImGuiKey_DownArrow,true);
            if(frame==5)io.AddKeyEvent(ImGuiKey_DownArrow,false);
            if(frame==6)io.AddKeyEvent(ImGuiKey_Enter,true);
            if(frame==7)io.AddKeyEvent(ImGuiKey_Enter,false);
            if(frame==8){const auto* w=ImGui::FindWindowByName("Camera Settings###RytyCameraSettings");io.AddMousePosEvent(w->Pos.x+100*width/1280.f,w->Pos.y+138*height/800.f);io.AddMouseButtonEvent(0,true);}
            if(frame==9)io.AddMouseButtonEvent(0,false);
        }
        for(auto c:clicks)if(c.frame==frame){io.AddMousePosEvent(c.x,c.y);io.AddMouseButtonEvent(0,c.down);}
        ImGui_ImplDX11_NewFrame();ImGui::NewFrame();
        if (dock_regression) ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (dock_regression && frame == 10) {
            if (auto* w = ImGui::FindWindowByName("Camera Settings###RytyCameraSettings")) ImGui::SetWindowPos(w, {-4000, -4000});
            Ryty::Camera::CloseOverlay(); Ryty::Camera::ResetOverlayLayout();
        }
        Ryty::Camera::DrawOverlay();ImGui::Render();
        if(ImGui::Core::captures!=(native_capture?1:0) ||
           bool(io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad)!=native_capture)++errors;
        if(frame==20){
            const auto* w=ImGui::FindWindowByName("Camera Settings###RytyCameraSettings");
            if(!w||w->Pos.x<0||w->Pos.y<0||w->Pos.x+w->Size.x>io.DisplaySize.x+1||w->Pos.y+w->Size.y>io.DisplaySize.y+1||w->Pos.y>io.DisplaySize.y*.15f||w->Size.y>io.DisplaySize.y*.50f)++errors;
            if(resize_regression&&(!w||std::abs(w->Pos.x/io.DisplaySize.x-.10f)>.002f||std::abs(w->Pos.y/io.DisplaySize.y-.04f)>.002f||std::abs(w->Size.x/io.DisplaySize.x-.80f)>.002f))++errors;
        }
        if (dock_regression && (frame == 6 || frame == 11)) {
            const auto* w = ImGui::FindWindowByName("Camera Settings###RytyCameraSettings");
            if (!w || w->DockId || !(w->Flags & ImGuiWindowFlags_NoDocking) || w->Pos.x < 0 || w->Pos.y < 0 || w->Size.x >= width || !Ryty::Camera::OverlayOpen()) ++errors;
        }
        if (frame == 5 || frame == 6 || frame == 7 || frame == 20) {
            std::printf("Frame %d down=%d clicked=%d hovered=%u active=%u\n",frame,io.MouseDown[0],io.MouseClicked[0],GImGui->HoveredId,GImGui->ActiveId);
            for (int i = 0; i < GImGui->TabBars.GetMapSize(); ++i) if (auto* bar = GImGui->TabBars.TryGetMapData(i)) {
                std::printf("Tabs selected=%u next=%u at %.1f %.1f\n",bar->SelectedTabId,bar->NextSelectedTabId,bar->BarRect.Min.x,bar->BarRect.Min.y);
                for (auto& tab : bar->Tabs) std::printf("Tab %s id=%u x=%.1f width=%.1f\n",bar->TabsNames.c_str()+tab.NameOffset,tab.ID,tab.Offset,tab.Width);
            }
        }
        const float clear[]{0.08f,0.10f,0.13f,1};context->OMSetRenderTargets(1,&rtv,nullptr);context->ClearRenderTargetView(rtv,clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if(frame==22)save("overlay-global.bmp");
        if(ImGui::GetDrawData()->TotalVtxCount==0)++errors;
    }
    Ryty::Camera::CloseOverlay();ImGui_ImplDX11_NewFrame();ImGui::NewFrame();Ryty::Camera::DrawOverlay();ImGui::Render();
    if(ImGui::Core::captures || !(io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad))++errors;
    if(!chase_preview && clicks.empty() && Ryty::Camera::selection_requests)++errors;
    std::printf("%s real ImGui overlay renders offscreen, no gamepad capture, navigation restored on close\n",errors?"FAIL":"PASS");
    ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();rtv->Release();target->Release();context->Release();device->Release();return errors?1:0;
}
