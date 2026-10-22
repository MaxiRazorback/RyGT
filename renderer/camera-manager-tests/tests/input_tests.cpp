#include "ryty_gpu/ryty_camera_input.h"
#include <cstdio>
#include <initializer_list>
using namespace Ryty::Camera;
int main() {
    int errors=0;
    auto check=[&](bool ok,const char* name) { std::printf("%s %s\n",ok?"PASS":"FAIL",name);errors+=!ok; };
    for(auto type:{SDL_EVENT_KEY_DOWN,SDL_EVENT_TEXT_INPUT,SDL_EVENT_TEXT_EDITING,SDL_EVENT_MOUSE_MOTION,SDL_EVENT_MOUSE_BUTTON_DOWN,SDL_EVENT_MOUSE_WHEEL}) {
        check(RouteInput(true,type)==InputRoute::InterfaceOnly,"keyboard/mouse input goes only to overlay");
        check(RouteInput(false,type)==InputRoute::Normal,"closing overlay restores native keyboard/mouse path");
    }
    for(auto type:{SDL_EVENT_KEY_UP,SDL_EVENT_MOUSE_BUTTON_UP})
        check(RouteInput(true,type)==InputRoute::ReleaseBinding,"release events cannot leave driving inputs stuck");
    for(auto type:{SDL_EVENT_GAMEPAD_BUTTON_DOWN,SDL_EVENT_GAMEPAD_BUTTON_UP,SDL_EVENT_GAMEPAD_AXIS_MOTION,SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN,SDL_EVENT_GAMEPAD_TOUCHPAD_UP,SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION,SDL_EVENT_GAMEPAD_SENSOR_UPDATE})
        check(RouteInput(true,type)==InputRoute::Gamepad && RouteInput(false,type)==InputRoute::Normal,"gamepad input bypasses camera UI consumption");
    for(auto type:{SDL_EVENT_GAMEPAD_ADDED,SDL_EVENT_GAMEPAD_REMOVED,SDL_EVENT_JOYSTICK_AXIS_MOTION,SDL_EVENT_WINDOW_RESIZED,SDL_EVENT_QUIT})
        check(RouteInput(true,type)==InputRoute::Normal,"device events and native window handling remain unchanged");
    return errors?1:0;
}
