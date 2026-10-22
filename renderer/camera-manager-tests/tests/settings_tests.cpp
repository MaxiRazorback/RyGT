#include "ryty_gpu/ryty_camera_settings.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>
#include <limits>
using namespace Ryty::Camera;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::filesystem::path path = argv[1];
    std::filesystem::remove(path); std::filesystem::remove(path.string() + ".tmp");
    int errors = 0; auto check = [&](bool ok, const char* name) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); errors += !ok; };
    Store store(path); std::string error;
    check(store.Load(error) && !store.Dirty() && store.Effective(2148, View::Cockpit).fov == 0, "missing file retains car-specific native FOV");
    Settings global; global.fov = 80; global.height = 0.2f;
    store.SetGlobal(View::Roof, global);
    check(store.Effective(2148, View::Roof) == global && store.Effective(999, View::Roof) == global, "global settings apply across different cars");
    Settings custom = global; custom.distance = 0.8f;
    store.SetCar(2148, View::Roof, custom);
    check(store.Effective(2148, View::Roof) == custom && store.Effective(999, View::Roof) == global, "override belongs only to its own stable car ID");
    check(store.Effective(2148, View::Cockpit) == Settings{}, "override belongs only to selected camera");
    store.RestoreDefaults(2148, View::Roof);
    check(store.Effective(2148, View::Roof) == Settings{} && store.Effective(999, View::Roof) == global, "car Restore Defaults uses original defaults even when global is customized");
    store.UseGlobal(2148, View::Roof);
    check(store.Effective(2148, View::Roof) == global && store.Read().cars.empty(), "Use Global removes override and follows future changes");
    store.SetCar(2148, View::Roof, custom);
    auto appearance = store.Read().appearance; appearance.scale = 1.5f; appearance.font_size = 24; appearance.opacity = 0.6f; appearance.accent = {0.2f, 0.4f, 0.8f}; store.SetAppearance(appearance);
    check(store.Save(error) && !store.Dirty(), "explicit Save writes clean settings atomically");
    Store reloaded(path);
    check(reloaded.Load(error) && reloaded.Read() == store.Read(), "all camera presets and appearance survive restart");
    global.fov = 65; store.SetGlobal(View::Chase, global);
    check(store.Save(error) && reloaded.Load(error) && reloaded.Effective(0, View::Chase).fov == 65, "Save replaces an existing file successfully");
    Document unchanged = reloaded.Read(), output = unchanged;
    auto legacy = Encode(unchanged); legacy["appearance"].erase("theme_version");
    legacy["appearance"]["background"] = {.055f,.06f,.075f};
    legacy["appearance"]["accent"] = {.9f,.12f,.19f}; legacy["appearance"]["opacity"] = .94f;
    check(Decode(legacy, output, error) && output.appearance.background == Document::Appearance{}.background &&
          output.appearance.accent == Document::Appearance{}.accent && output.appearance.opacity == .97f &&
          output.cars == unchanged.cars && output.global == unchanged.global,
          "legacy default colors migrate to exact Controllers palette without changing presets");
    output = unchanged;
    auto invalid = Encode(unchanged); invalid["cars"]["2148"]["Roof"]["height"] = "bad";
    check(!Decode(invalid, output, error) && output == unchanged, "malformed preset rejects transaction without partial changes");
    invalid = Encode(unchanged); invalid["version"] = 99;
    check(!Decode(invalid, output, error) && output == unchanged, "future format version rejected without data loss");
    invalid = Encode(unchanged); invalid["cars"]["4294967296"] = nlohmann::json::object();
    check(!Decode(invalid, output, error), "overflowing car ID rejected");
    invalid = Encode(unchanged); invalid["global"]["Chase"]["sensitivity"] = -99; invalid["global"]["Chase"]["return_seconds"] = 2000;
    check(Decode(invalid, output, error) && output.global[1].sensitivity == -15 && output.global[1].return_seconds == 10, "hand-edited values bounded to supported ranges");
    Settings extreme; extreme.height = 5; extreme.distance = -5; extreme.return_seconds = 999;
    const float highs[]{.6f,1.f,.55f,.35f}, lows[]{-.6f,-1.f,-5.f,-.35f}, distances[]{.6f,3.f,0.f,1.f};
    for (unsigned i=0;i<4;++i) {
        const auto view=static_cast<View>(i);
        store.SetGlobal(view, extreme); store.SetCar(123, view, extreme);
        auto v=store.ReadGlobal(view);
        check(v.height==highs[i] && v.distance==-distances[i] && v.return_seconds==10 && store.Effective(123,view)==v,
              "global/custom high limits enforced on publication");
        auto negative=extreme;negative.height=-5;negative.distance=5;store.SetGlobal(view,negative);
        v=store.ReadGlobal(view);
        check(v.height==lows[i] && v.distance==distances[i],"camera-specific low height and distance limits enforced");
    }
    auto old_roof=Encode(unchanged);old_roof["global"]["Roof"]["height"]=.2f;
    check(Decode(old_roof,output,error) && output.global[3].height==.2f && std::string(Names[3])=="Roof" && Encode(output)["global"].contains("Roof"),
          "Roof display name retains existing preset key and values");
    Settings nan; nan.height = std::numeric_limits<float>::quiet_NaN(); nan.fov = nan.height;
    check(Sanitize(nan).height == 0 && Sanitize(nan).fov == 0, "non-finite settings cannot enter camera callbacks");
    { std::ofstream corrupt(path); corrupt << "{broken"; }
    check(!reloaded.Load(error) && reloaded.Read() == unchanged, "corrupt file retains current live document");
    std::atomic<bool> good{true};
    std::thread writer([&] { for (int i = 0; i < 5000; ++i) { auto v = global; v.fov = i % 2 ? 50 : 90; v.height = v.fov / 1000; store.SetCar(2148, View::Roof, v); } });
    std::thread reader([&] { for (int i = 0; i < 10000; ++i) { auto v = store.Effective(2148, View::Roof); if (v.fov != custom.fov && v.height != v.fov / 1000) good = false; } });
    writer.join(); reader.join(); check(good, "UI publication and camera callback reads are consistent under concurrency");
    store.RestoreDefaults(0, View::Roof);
    check(store.Effective(999, View::Roof) == Settings{}, "global Restore Defaults preserves native FOV and zero offsets");
    return errors ? 1 : 0;
}
