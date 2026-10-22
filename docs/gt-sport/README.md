# GT Sport on RyGT

RyGT is Ryty with the changes GT Sport (CUSA03220, update 1.69) needs to run as a native Windows
program, together with shadGT's Vulkan renderer.

- **This repository:** Ryty plus the GT Sport fixes (libkernel memory and event queues for the
  renderer, save data, AppContent, audio, NetCtl, libc crash reports and guest allocations, and
  converter fixes in the relinker).
- **The renderer:** github.com/mlgprorektm8/shadGT, branch `ryty-gpu`. It builds shadGT's video
  output, Gnm driver and renderer as `libSceVideoOut.prx` (target `ryty_gpu`) and shadGT's NP,
  audio and other HLE libraries for Ryty.

## Building

- Runtime libraries: the `windows-mingw` build (MSYS2, LTO off).
- Converter: `core/relinker` gives `ryty.exe`.
- Renderer: in shadGT, `cmake --preset x64-Clang-Release -DSHADGT_RYTY_GPU=ON`, target `ryty_gpu`.

## Setup with the loader

Unzip a release on the same drive as your game dump, run `RyGT.exe`, pick the game folder, the
1.69 update folder, a folder with your firmware's system modules and one with its fonts, then
press **Set up** and **Play**. Set up also gives `GTSport.exe` the ryGT icon and makes it a Windows
(GUI) program, so after that you can also open `GTSport.exe` directly: the runtime and renderer
default to the settings GT Sport needs, and the game writes `stdout.log` and `stderr.log` itself.
The loader's source is in [loader](../../loader) (build it with `loader/build.cmd`).

Options: **Start in fullscreen** (borderless, on the window's display), the splash screen, FPS
counter, Discord and upscaling (below).

## Upscaling

GT Sport draws at 1920x1080. With a larger window or screen, the launcher's **Upscale with AMD FSR**
box scales it up (with optional RCAS sharpening); otherwise the picture is stretched. It uses AMD FSR
3.1.4, the newest FSR with a Vulkan backend (FidelityFX SDK 1.1.4; FSR 4 is DirectX 12 only), through
AMD's signed `amd_fidelityfx_vk.dll`, which the release puts next to `GTSport.exe`. Unticking **Use FSR
3.1.4** switches to shadGT's FSR 1. The renderer only has the finished picture, without motion
vectors or depth, so FSR 3.1 starts a new history every frame and works as a spatial upscaler. NVIDIA
DLSS is available as a separate plugin you build and install yourself, because NVIDIA's SDK can't be
shipped with the GPL renderer; see [plugins/dlss](../../plugins/dlss). The launcher's DLSS box stays
grey until the plugin is installed.

## Setup by hand


No game files, converted executables, PS4 system modules or fonts are distributed; you need your
own decrypted dump of CUSA03220 with update 1.69 and your own firmware files.

1. Unwrap the 1.69 `eboot.bin` (a SELF; its segments are not encrypted) into `eboot.elf`, and
   rewrite its PT_SCE_RELRO segment (type 0x61000010) as a read-write PT_LOAD. Do the same for
   each module in step 2.
2. Next to `eboot.elf`, make a folder `sce_module` with the game's `libc.prx`, `libSceFios2.prx`,
   `libSceSecure.prx` and `logiWheel.prx`, and from your firmware `libSceFont.prx`, `libSceFontFt.prx`,
   `libSceFreeTypeOt.prx`, `libSceJson2.prx`, `libSceNgs2.prx` and `libSceBluetoothHid.prx`, plus
   `libSceJpegDec.prx` (Scapes images) and `libScePngEnc.prx` (saving photos) when your firmware has them.
3. Convert:
   `ryty.exe --windows --ps4 --skip-syscall-check --registry --report port-report.json eboot.elf GTSport.exe`
4. Next to `GTSport.exe`, put the release's `libs` folder, MinGW DLLs and
   `user\custom_configs\CUSA03220.json`, and an `app0` folder with the game's files (the base
   game's `gt*.vol`, the update's `gt.idx` and `gtuc/gtui/gtun.vol`, `sce_sys`, `app_param.sfo`
   and the rest of the dump's root) and the converted `sce_module\*.guest.prx` in
   `app0\sce_module`.
5. System fonts go in `sys\common\font` and `sys\common\font2`.
6. Run `GTSport.exe` (`RyGT.exe --prepare GTSport.exe` gives it the icon and the Windows subsystem,
   so no console window opens).

The RyGT launcher has an **In-game user name** field. It saves your choice and passes it to the
game when you press **Play**. Leaving it empty keeps the default name, `Player`. The runtime also
accepts the `RYGT_USER_NAME` environment variable when the game is started directly.

## Status (2026-10-11)

Boots, menus, races and the race summary work, at about 42-60 fps in races depending on how many
cars are on screen. The first 30 seconds of a race can stutter while pipelines compile.

Fixed in the second build of 2026-10-11: the crash on a fresh save at boot, striped photos in
Brand Central and black Scapes/dynamic scenes (the renderer now protects the game code's red
zones from Windows exception frames), the crash when buying a car (the game's clocks stand still
while shaders compile) and a crash at one showroom dynamic scene.

Known issues: the screen stays white for about 6 seconds before the home menu (black on a PS4),
and one showroom dynamic scene renders black.
