# RyGT

<h1 align="center">
  <img src="assets/GranTurismoSportBanner.png" width="800" alt="Gran Turismo Sport banner">
  <br>
  <b>RyGT</b>
  <br>
  <sub>Gran Turismo Sport on PC</sub>
</h1>


**Gran Turismo Sport running as a native Windows program.**

RyGT ports Gran Turismo Sport (CUSA03220, update 1.69) from PlayStation 4 to Windows. The game is
not emulated: RyGT's converter turns the game's executable into `GTSport.exe`, the game code runs
directly on your CPU, and RyGT's libraries stand in for the PS4 system. Graphics go through
[shadGT](https://github.com/mlgprorektm8/shadGT/tree/ryty-gpu)'s Vulkan renderer, built as the
game's video output library.

RyGT is a fork of [Ryty](https://github.com/aarvsn/Ryty), the general PS4/PS5 porting tool, with
everything GT Sport needs added on top.

## Status

| Area | State |
| --- | --- |
| Boot, menus, car and track selection | Working |
| Races | Working, about 42-60 fps depending on how many cars are on screen |
| Race summary and results | Working |
| Audio | Working |
| Controllers | Working through SDL |
| Online features | Not supported |

Known issues:
- The first 30 seconds of a race can stutter while graphics pipelines compile.
- A few draws in the race summary can occasionally read garbage vertex descriptors; those draws
  are skipped.

Tested on Windows 11 with an Intel i7-13700KF and an RTX 4070Ti.
Tested on Windows 11 with an AMD Ryzen 5 9600X and an RTX 3060 12GB

## Download

Get the latest build from [Releases](https://github.com/mlgprorektm8/RyGT/releases). A release
contains the runtime libraries, the renderer, the converter, a launcher and the GT Sport settings.
It contains **no game files**: you need your own decrypted dump of GT Sport CUSA03220 with update
1.69, and the system modules and fonts from your own PS4 firmware.

Unzip it on the same drive as your dump and run `RyGT.exe`: pick your game, update, firmware
module and font folders, press **Set up**, then **Play**. The loader's source is in
[loader](loader). Manual setup steps are in [docs/gt-sport/README.md](docs/gt-sport/README.md).

## How it fits together

```
your dump: eboot.bin (1.69) + sce_module
        |
        |  unwrap the SELF, then convert with ryty.exe --windows --ps4
        v
GTSport.exe + app0\sce_module\*.guest.prx
        |
        |  loads at run time
        v
libs\  RyGT runtime libraries (kernel, libc, save data, audio, input, ...)
       + shadGT renderer (libSceVideoOut.prx: Gnm driver, PM4 processing, Vulkan)
```

## What this repository contains

- **Converter** ([core/relinker](core/relinker)): turns the PS4 executable and its modules into
  Windows binaries. RyGT adds fixes for GT Sport's executable and modules.
- **Runtime libraries** ([core/libs/prx](core/libs/prx)): the PS4 system libraries the game links
  against. RyGT's changes for GT Sport include:
  - kernel: an event queue bridge for the renderer, mapping and protection change notices so the
    renderer's memory tracking stays correct, `MAP_FIXED` replacing mappings, direct memory and
    semaphore fixes;
  - libc: C++ exceptions through the SEH runtime, guest allocation fixes, crash reports with
    debugging aids;
  - save data, AppContent, audio output and network control fixes, including holding the first
    save data mount until the boot screen is drawn (GT Sport's boot script otherwise crashes).
- **Loader** ([loader](loader)): `RyGT.exe`, which converts the game from your own dump and
  starts it, with options for the splash screen, FPS counter, Discord and AMD FSR 3.1 or FSR 1 upscaling.
- **DLSS plugin** ([plugins/dlss](plugins/dlss)): optional NVIDIA DLSS upscaling, built and
  installed separately (not in the releases).
- **GT Sport guide** ([docs/gt-sport](docs/gt-sport)): setup from your own dump.

The renderer lives in [shadGT, branch `ryty-gpu`](https://github.com/mlgprorektm8/shadGT/tree/ryty-gpu)
(`ryty_gpu/`). It builds shadGT's video output, Gnm driver and Vulkan renderer as
`libSceVideoOut.prx`, plus shadGT's audio, NP and other libraries for RyGT. It carries the GT
Sport rendering fixes and the performance work (Vulkan calls on their own thread, cheaper buffer
and image tracking).

## Building

Most people should use a [release](https://github.com/mlgprorektm8/RyGT/releases). To build it
yourself on Windows you need:

| What | Why | Where |
| --- | --- | --- |
| Git | cloning, with submodules | [git-scm.com](https://git-scm.com/download/win) |
| MSYS2 (UCRT64 shell) with GCC, CMake and Ninja | the runtime libraries (`libs\*.prx`) and the converter | [msys2.org](https://www.msys2.org) |
| Internet access while configuring | CMake downloads FFmpeg's prebuilt libraries once | |
| .NET Framework 4.x (part of Windows 10/11) | the launcher, `RyGT.exe` | built in |
| Visual Studio 2022 with "C++ Clang tools", the Vulkan SDK | only to rebuild the renderer (`libSceVideoOut.prx`) | [visualstudio.microsoft.com](https://visualstudio.microsoft.com), [vulkan.lunarg.com](https://vulkan.lunarg.com) |

You also need everything a release needs to run: your own decrypted GT Sport dump (CUSA03220,
update 1.69) and your PS4 firmware's system modules and fonts.

### 1. Get the code with its submodules

```sh
git clone --recursive -b development https://github.com/mlgprorektm8/RyGT.git
cd RyGT
```

Already cloned without `--recursive`? Run `git submodule update --init --recursive`. Without the
submodules (SDL2, FFmpeg, FreeType, stb, glslang and the others in `3rdparty`), CMake builds only
the converter and **skips every runtime library**, so `GTSport.exe` then fails to load
`libScePad.prx` and the rest. The configure step warns about this.

### 2. Install the compiler (MSYS2)

Open the **MSYS2 UCRT64** shell (not "MSYS2 MSYS") and run:

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

### 3. Build the runtime libraries and the converter

In the UCRT64 shell, from the RyGT folder:

```sh
cmake -S . -B build/windows-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release -DRTY_ENABLE_LTO=OFF -DRTY_BUILD_GUI=OFF
cmake --build build/windows-mingw --target libs ryty
```

Check that the configure output has no "3rdparty submodules are missing" warning. The results:

- `build/windows-mingw/core/libs/libs/*.prx`: the runtime libraries. They go in the game folder's
  `libs` folder.
- `build/windows-mingw/core/relinker/ryty.exe`: the converter. It goes in `converter`.

The libraries also need `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` from
`C:\msys64\ucrt64\bin` next to `GTSport.exe` (a release already has them).

Building from PowerShell or cmd instead of the UCRT64 shell? Put `C:\msys64\ucrt64\bin` first on
`PATH`, or the build fails without a clear error.

### 4. Build the launcher

From cmd or PowerShell:

```bat
loader\build.cmd C:\path\to\your\game\folder
```

This writes `RyGT.exe` with the C# compiler that ships with Windows.

### 5. Put it together

The easiest way is to start from a release: unzip it, then copy your own `libs\*.prx`,
`converter\ryty.exe` and `RyGT.exe` over the release's files. The release also has the renderer,
the FSR library, the launcher pictures and the tools, which this repository doesn't build. Then
run `RyGT.exe` and press Set up, as in the release's README.

### Renderer (optional)

The renderer is in [shadGT, branch `ryty-gpu`](https://github.com/mlgprorektm8/shadGT/tree/ryty-gpu).
In a "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake --preset x64-Clang-Release -DSHADGT_RYTY_GPU=ON
cmake --build Build/x64-Clang-Release --target ryty_gpu
```

This makes `libSceVideoOut.prx` and the other `libSce*.prx` files in `Build/x64-Clang-Release`.
They replace the ones in `libs`.

More details: [docs/user/BUILDING.md](docs/user/BUILDING.md).

## Branches

- `main`: stable builds; releases come from here.
- `development`: ongoing work. Changes reach `main` once they are tested.

# Discord Server
Join our server for more updates: https://discord.gg/8dz4U9Nd8d

## Documentation

- [GT Sport setup](docs/gt-sport/README.md)
- [Linux (Steam Proton)](docs/user/LINUX.md)
- Ryty's general documentation: [index](docs/README.md), [building](docs/user/BUILDING.md),
  [usage](docs/user/USAGE.md), [input mapping](docs/user/INPUT_MAPPING.md),
  [porting pipeline](docs/dev/PORTING_PIPELINE.md)

## Credits

- [Ryty](https://github.com/aarvsn/Ryty) by aarvsn, itself a fork of
  [AnyPS5](https://github.com/boykopovar/AnyPS5) by Andrii Semkiv (boykopovar) and its
  contributors: the converter, the system library implementations and the test infrastructure.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) and its contributors, through shadGT: the
  renderer, shader recompiler and HLE libraries.
- [Nenkai](https://github.com/Nenkai): [GTToolsSharp](https://github.com/Nenkai/GTToolsSharp)
  (reading the game's volumes) and [GTAdhocToolchain](https://github.com/Nenkai/GTAdhocToolchain)
  (the AdHoc script compiler and disassembler), both MIT licensed. The launcher uses them to build
  the offline livery and decal scripts from your own game files, and Nenkai's research into
  Gran Turismo's formats and scripts made that work possible.
- See [AUTHORS.md](AUTHORS.md).

## Disclaimer

This project is for interoperability, research and preservation. It does not include or
distribute copyrighted games, firmware, keys or proprietary libraries. Use your own legally
obtained copy of the game and your own console's firmware files, in line with the laws where
you live.

Please do NOT ask shadps4 or Ryty servers for help from this project.

## License

GNU General Public License version 2 only, as Ryty. The renderer (shadGT) is also GPL-2.0.

<p align="center">
  <img src="assets/GranTurismoSportBanner2.webp" width="800" alt="Gran Turismo Sport banner">
</p>
