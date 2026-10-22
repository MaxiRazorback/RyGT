# ryGT DLSS plugin

An optional plugin for NVIDIA DLSS reconstruction in ryGT.
It is not part of the ryGT release: NVIDIA's DLSS SDK can't be linked into the GPL renderer, so
DLSS lives in this separate MIT-licensed DLL, and you build and install it yourself.

## What it does

Renderer patch `0020` supplies scene color, depth, motion vectors and camera jitter from the same
guest frame, before composing the HUD. The temporal interface requests preset M and requires a
DLSS runtime supporting that preset, such as DLSS 4.5. The DLSS checkbox enables this path in the
serial renderer for recognized SDR scene passes. Unsupported passes retain game rendering.

The game renders at up to 1920x1080. Reconstructing that scene to 3840x2160 has the Performance
ratio. There is no quality selector: changing the NGX profile does not change guest render
resolution. Frame generation remains a separate option.

The version 1 interface remains available to older renderers. It uses placeholder depth and
motion, no jitter and a history reset every frame, so it is a single-frame upscaler. Successful
quality selections are cached until the input/output dimensions or device session change.
Installing a newer runtime alone does not enable temporal reconstruction in an older renderer.

## Temporal interface

Version 2 uses guest frame IDs, input-pixel jitter and current-to-previous motion converted through
the supplied motion scales. Color and motion inputs use `SHADER_READ_ONLY_OPTIMAL`; depth also
accepts `DEPTH_STENCIL_READ_ONLY_OPTIMAL`. Output is a distinct RGBA16F storage image in `GENERAL`.
The caller owns resource lifetimes and barriers, serializes evaluations and finishes recorded GPU
work before changing feature dimensions or creation flags.

Successful consecutive frames retain history. First use, explicit resets, frame gaps, rewinds,
feature recreation and failed evaluations invalidate it. Duplicate presentations are not new
guest frames. `RygtDlss_GetOptimalSettings` returns the supported input range for an explicit
quality mode; inputs outside that range are rejected. Current renderer limitations are recorded
in [technical debt](../../docs/dev/TechnicalDebt.md#functional).

## Build

You need clang-cl, Visual Studio's C++ libraries, NVIDIA's DLSS SDK
(<https://github.com/NVIDIA/DLSS>, the `include` and `lib\Windows_x86_64\x64` folders) and the
Vulkan headers (shadGT's `externals\vulkan-headers` by default).

```
set DLSS_SDK=C:\path\to\DLSS
build.cmd C:\path\to\output
```

## Install

Next to `GTSport.exe`, make a `dlss` folder holding:

- `ryGT_dlss.dll` from the build;
- `nvngx_dlss.dll` from NVIDIA's DLSS SDK (`lib\Windows_x86_64\rel`) or another DLSS release.

Use "Add DLSS file..." in `RyGT.exe` to import `nvngx_dlss.dll` while the game is closed. The
previous runtime is kept in a timestamped `.bak` file. This imports only the NVIDIA runtime;
`ryGT_dlss.dll` must already be installed separately.

Tick "NVIDIA DLSS" (available when both files are present), or set `"dlss_enabled": true` in the
`GPU` section of `user\custom_configs\CUSA03220.json`. Changes apply at the next game launch.
The half-rate mirror patch is disabled while temporal DLSS is selected to preserve motion data.

`rygt_dlss_api.h` is the interface between the renderer and the plugin; shadGT's
`ryty_gpu/rygt_dlss_api.h` must stay identical.

## Tests

The mode-cache and temporal-history tests need a C++17 compiler and CMake, without the DLSS SDK
or an RTX GPU:

```
cmake -S plugins/dlss/tests -B build/dlss-tests
cmake --build build/dlss-tests --config Release
ctest --test-dir build/dlss-tests -C Release --output-on-failure
```

They are also registered by the root project when `BUILD_TESTING` is enabled.
