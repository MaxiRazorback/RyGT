# Renderer changes (shadGT, branch `ryty-gpu`)

The renderer (`libSceVideoOut.prx`) is built from
[shadGT, branch `ryty-gpu`](https://github.com/mlgprorektm8/shadGT/tree/ryty-gpu). Changes made
after shadGT commit `29af7ca6` are kept here as patches, in order (the branch itself includes all of them from
commit `4371a500`):

| Patch | What it adds |
|---|---|
| `0001-ryGT-frame-generation-...` | AMD FSR 3.1.4 frame generation, shader precompile on the splash screen (with a progress bar), pausing the game while new shaders compile, compile batching, efficiency-core shader compiles, opt-in diagnostics |
| `0002-ryGT-splash-screen-shows-...` | Hotfix: the splash screen and progress bar show while the game waits for the precompile (it was a black screen) |
| `0003-ryGT-Lewis-Hamilton-Time-Trial-...` | GTS-002: unlocks the Lewis Hamilton Time Trial Challenge in the Campaign menu (the add-on's unlock flags came from the shut-down servers); `RYGT_LEWIS=0` turns it off |
| `0004-ryGT-Livery-Editor-offline-GTS-003` | GTS-003: enables the Livery button on the main menu while offline (the menu script greyed it out without the servers; the editor has its own offline mode that saves liveries locally); `RYGT_LIVERY=0` turns it off |
| `0005-ryGT-effects-fix-frame-generation-...` | Dust, smoke and rain no longer show pink and black rectangles (the game's clears of reused effect memory ran as skipped "mask clears"); frame generation starts after the splash screen (it could freeze the precompile); slow-motion shader compiles; freeze and frame-capture diagnostics |
| `0006-ryGT-on-screen-keyboard-offline-livery-...` | On-screen keyboard for text fields, whole-script replacements from `patches\scripts` (GTS-005), custom decals from the `decals` folder (GTS-006) |
| `0007-ryGT-renderer-speed-work-2026-10-15` | Recorder speed-ups (fence writes through cached aliases, texture lookup memo), the decoder's shader search tries the last match first, early-fence deadlock fixes, and the opt-in multithreaded groundwork (per-thread draw state and command rings, buffer stage workers, shared buffer-cache lock) with ceiling and timeline diagnostics |
| `0008-ryGT-direct-memory-renderer-2026-10-16` | The new renderer (launcher option "New renderer", `GPU.direct_renderer`, or `SHADGT_DIRECT=1`): shaders read game memory in place through a GPU page table over imported guest memory instead of copying buffers every draw (Tokyo rain benchmark about 32 -> 43 fps); freed render targets are reused instead of rebuilt (all modes); wet-road reflection (GTS-007) and half-rate mirror (GTS-008) toggles; texture-binding shortcuts; import prototype, ceiling and timing diagnostics |
| `0009-ryGT-buffer-stage-with-direct-renderer` | With the new renderer, a helper thread fetches the remaining (copied) buffers ahead of the draw recorder (about +1.5 fps in the Tokyo rain benchmark) |
| `0010-ryGT-framegen-compile-freeze-fix` | Frame generation pauses while new shaders compile (with it on, heavy compiling could freeze the game in the driver); timing switches for the command thread and pipeline selection |
| `0011-ryGT-quality-preset-memory` | The reused-image pool size follows the launcher's quality preset (`GPU.image_recycle_mb`) |
| `0012-ryGT-old-shader-fallback-direct-default` | The new renderer is the only renderer (`SHADGT_DIRECT=0` for comparisons); while its own shaders and pipelines are built in the background, draws use the old renderer's stored shaders, so switching causes no shader-compile freezes |
| `0019-ghost-quad-sample-mask` | Per-quad sample coverage masks restore recorded ghost transparency; ordinary shader caches remain compatible |
| `0020-fix-dlss-temporal-scene-reconstruction` | Temporal DLSS scene inputs, persistent history, RGB conversion and HUD composition in the serial renderer; full-rate mirror while temporal DLSS is selected |

To build: check out shadGT at `29af7ca6`, apply the patches with
`git am <RyGT>/renderer/shadGT-patches/*.patch`, then build the `ryty_gpu` target as in the main
README (`cmake --preset x64-Clang-Release -DSHADGT_RYTY_GPU=ON`).

For the portable stdout severity gate and timestamped log rotation, see
[LOGGING.md](LOGGING.md). Patch 0019 is applied after the existing 0018 patch;
the current upstream renderer does not include it.

Patch 0020 fixes the non-renderer host libraries' shared-stub link dependencies
when rebuilding the complete library set with logging enabled.

Patches 0026-0038 come from contributors: 0026-0027 and `0019-ryGT-limit-concurrent-vulkan-pipeline-compiles`
(Tanos; that one applies after 0025), 0029-0038 (xacidr4inx, plain diffs: apply them with
`git apply`) with `vma-patches/0001-trim-empty-default-blocks.patch` for shadGT's `externals/vma`.
Patch 0039 is the shader-compile work (all-core compiles when many wait, faster shader-pack
translation, store dump). Patch 0040 adds the native renderer (experimental): GT Sport's command
buffers are recorded straight into Vulkan by its own renderer instead of shadGT's (about 33-39 fps
against 29 in the Tokyo rain benchmark). The launcher's "Renderer" dropdown picks it
(`GPU.native_renderer`; `RYGT_NATIVE=1` or `=0` overrides); the old renderer stays the default.
Known open problems: in Tokyo at night the cockpit's ambient light flickers between warm and dark
frames, and rare frames are drawn from a wrong camera. Patch 0041 makes shadGT apply the VMA patch
by itself at configure time. Patch 0042 lets the non-renderer host libraries link again (a no-op
Controllers window toggle), and 0043 replaces CreateFileMapping2, which Wine and Proton before 11
lack, with CreateFileMappingW so that every host library loads under Proton 10 (see
[docs/user/LINUX.md](../docs/user/LINUX.md)). Patch 0044 (FIX-160) makes the native renderer and
the background shader workers read guest memory safely, since the game can release it while
the renderer still reads it (two crashes in a Linux tester's runs).
Patch 0045 (DIAG-161) prints GT Sport's own fatal-error message to stderr before the game ends
itself ("[game] fatal error at <file>(<line>): <message>"). Patch 0046 (DIAG-163) logs the first 60
scripts the game loads at start-up ("[boot] script <n> (+<ms> ms): <unit>"; `RYGT_LOG_BOOT=0` turns it
off), to show how far a boot that ends early got.

Patch 0042 (Gabarsolon) fixes Intel Arc rendering, compute barrier deadlocks, and DirectMemory performance
on the native renderer:
- Replaces `OpControlBarrier` with `OpMemoryBarrier` in compute shaders when `subgroup_size < 64`, avoiding
  hardware EU arrival counter hangs in wave64-uniform branches.
- Removes Intel from `needs_lds_barriers` to match AMD.
- Unconditionally binds compute pipelines in `DispatchDirect` and `DispatchIndirect` on the primary command stream.
- Selects `HOST_CACHED` (Type 2) memory for host-imported buffers in `DirectMemory` and `ImportHostMemory`.

Despite its number, 0042-intel-arc applies after 0053 and before 0054 (it was written later). The copy
here is UTF-8 (the first one was UTF-16, which `git apply` can't read) and has two conflicts resolved:
`log.cpp` keeps the leveled stdout without a renderer log file (0029/0048), and FIX-171's rectangle and
packed texture probes get the same transfer-to-host barrier as the patch's general probe.

`0019-ghost-quad-sample-mask` (Gabarsolon) is applied in shadGT after the native renderer, merged
with its larger push block (the quad sample mask follows the four buffer-offset vectors).

shadGT's branch `ryty-gpu` has every patch applied (commit `0522545b`; `84cf50ce` added skipping
screenshots of an empty picture); building from it is the simplest way to get the current renderer.
Patches 0042-0050 are included there too (`ryty-gpu` and `linux-proton` are both at `0522545b`). Patch 0047
puts the shader cache in `<RyGT folder>\shaders` instead of `user\cache`; 0048 makes the launcher's Info
log level include the renderer's Info lines (frame-time windows). 0049 (FIX-165) stops using the driver's
pipeline cache file (`RYGT_DRIVER_CACHE=1` turns it back on, saved only at exit): at 3.7 GB, its
saves every 30 s made native races drop to 10 fps and back. 0050 (DIAG-166) attaches RenderDoc when its
layer is on: F12 or a `capture.now` file in the RyGT folder captures a frame into `captures`.
Patch 0057 (GTS-009) fills screens wider (or narrower) than 16:9: `GPU.aspect_ratio` ("auto", the
launcher's Display > Screen shape > "Fill the screen", is the shape of the game's window or screen;
also "21:9", "32:9", width:height or a number; `RYGT_ASPECT` overrides it) widens the race camera's
16:9 aspect (eboot 0x5e717f) and stretches the picture over the whole window instead of black
bars. The HUD and menus are stretched with the picture; 16:9, the default, changes nothing.

Patches 0058-0061 are AMD fixes, tested on an RX 7600 (Windows 11, driver 26.8.1, 16 GB RAM) at
Dragon Trail. They apply after 0057 (shadGT `ryty-gpu` at `7080bee5`).
- 0058 (FIX-188) turns direct memory back on by default. 0042-intel-arc turned it off for every
  GPU; without it the native renderer copies every buffer through host upload memory, and a 16 GB
  PC ran out at the start of a race ("NAT-100: no upload memory"). `SHADGT_DIRECT=0` still turns
  it off (Intel Arc).
- 0059 (FIX-189): AMD's driver refuses storage use for BC6H images even with extended usage, so
  the native renderer made no image for GT Sport's HDR sky and reflection maps (black sky, flat
  car paint). Such images are now made without storage use; shaders only sample them.
- 0060 (FIX-191): without manual interpolation (AMD's path), a fragment input that a shader reads
  both blended (`V_INTERP_P2_F32`) and per vertex (`V_INTERP_MOV_F32`) was read at vertex 0 for the
  blended read, so terrain and kerbs were drawn as flat-coloured triangles. The blend is now
  computed from the barycentrics. Stored translations keep the old result: clear
  `<RyGT folder>\shaders\<serial>-direct-sets` once after updating.
- 0061 (FIX-187): with frame generation on, minimizing the window (a 0x0 surface) no longer
  crashes the game; generation stays off until the window is restored.

## Combined renderer enhancements

Patch 0062 applies after the temporal DLSS patch 0020. For the tested source combination,
check out shadGT `ryty-gpu` at `1d13c88d2e57bc63ce1586fe0b8f3342b6afdd19`, then apply the
current 0020 and 0062 patches with `git apply`. Other patches in this directory are already
included in that shadGT revision. Build the `ryty_gpu` target, the main-branch DLSS plugin,
and the Windows launcher. The renderer/plugin DLSS interface headers must match.

0062 adds smooth recorded-ghost blending and four GPU JSON settings: `ghost_body_opacity`,
`ghost_rubber_opacity`, `ghost_rim_opacity`, and `ghost_rim_brightness` (0-100, default 25).
They are read when the game starts. It also adds the PS4 Pro 1080p preset, output-resolution
UI, optional fast flips, post-DLSS sharpening, experimental HDR, frame-generation gating,
shader-allocation guards, and bounded texture-memory recovery and supported VRAM paging.

Both renderers support smooth ghost blending, output-resolution UI and post-DLSS sharpening.
The native renderer synchronizes UI proxy writes before guest texture reads, storage writes,
copies and resolves, and captures the UI frame under its recording lock. Pro tiling specialization
is shared between renderers; native pipeline keys distinguish base and alternate layouts.
Native image/buffer allocation failures reclaim completed allocations and empty allocator blocks,
then retry the identical allocation once. Live images are not discarded for recovery.
Recognized SDR gameplay reconstructed by temporal DLSS in the default renderer uses its scene/HUD
output; other supported frames use the separate UI proxy. Upstream temporal DLSS remains limited
to the default renderer. Native rendering retains the legacy DLSS interface. HDR is experimental
in both paths; the native HDR layer is connected, but live HDR gameplay is not validated.
The Pro option selects the game's 1080p Neo preset; a higher host output size does not make
every internal game pass render at that resolution.

The launcher retains main's tabbed layout, renderer choice, ultrawide options, wheel support,
and temporal DLSS behavior. The shader-pack preparation destination retains the upstream
Hotfix 2 correction (`CUSA03220-direct-sets`).
Patches 0058 and 0059 limit 0042-intel-arc to Intel GPUs. On NVIDIA its shader translation turned
races red with magenta cars (memory-only compute barriers and 64-bit shared memory as 32-bit halves,
with an empty shader store), and it also turned direct memory off and undid SanitizeCopyLayers for
every GPU. 0058 reverts it; 0059 (FIX-188) brings its Intel Arc changes back behind
`Instance::IsIntel()` (vendor 0x8086), so every other GPU runs what it ran before 0042-intel-arc.
`ryty-gpu` and `linux-proton` have every patch through 0059 (commit `1d13c88d`).

## Camera Manager

`shadGT-patches/0065-in-game-camera-manager.patch` adds the in-game Camera Manager
for Windows. It is a plain Git diff against shadGT
`1d13c88d2e57bc63ce1586fe0b8f3342b6afdd19` on `ryty-gpu`; apply it with `git apply`
on that checkout, then build the existing `ryty_gpu` target. Its native renderer
and shader sources are unchanged. This patch follows the existing RyGT convention
of carrying shadGT additions here; the shadGT source tree is not vendored into RyGT.

The patch includes the dedicated camera sources, build integration and the host
startup, overlay and input connections. It does not include compiled modules.
Existing build options are retained; the camera target additionally links Windows
`bcrypt` for executable validation.

For controls, ranges and presets, see [Camera Manager](../docs/user/CAMERA_MANAGER.md).
For the standalone Windows tests, see [camera-manager-tests](camera-manager-tests/README.md).
## Temporal FSR 3.1 and Intel XeSS (0063-0064)

Patches 0063 and 0064 apply after 0062 (shadGT `ryty-gpu` at `1d13c88d` plus 0020, 0062 and
0058-0061). Tested on an RX 7600 (Windows 11, driver 26.8.1) at Dragon Trail.
- 0063 (FIX-211): AMD's driver refuses storage use for some block-compressed images (BC5, BC6H)
  even with extended usage. The default renderer made them anyway and the textures came out
  garbled (broken colours on car dashboards). They are now made without storage use when
  `getImageFormatProperties2` refuses it; shaders only sample them.
- 0064 (FSR-201..209): temporal upscaling with the scene inputs from 0020 (colour, depth, motion
  vectors and jitter), default renderer only. It replaces the game's anti-aliasing.
  - `GPU.fsr_temporal` turns it on (with `fsr_enabled` and `fsr3_enabled`; DLSS temporal wins
    when it is on). `SHADGT_FSR_TEMPORAL` overrides it.
  - `GPU.fsr_upscaler` picks the backend: `fsr3` (FSR 3.1 through `amd_fidelityfx_vk.dll`),
    `fsr4` (newest 4.x the DLL offers; AMD ships FSR 4 for D3D12 only, so this is a placeholder)
    or `xess` (Intel XeSS-SR, needs `libxess.dll` from the XeSS SDK next to `GTSport.exe`).
    `SHADGT_FSR_UPSCALER` overrides it.
  - `GPU.upscale_ratio` (string) sets the render size below the output: `1` (native, AA only),
    `1.3`, `1.5`, `1.7`, `2`, `2.3`, `3`; `0` renders at the window size. XeSS picks the quality
    mode whose ratio is nearest. `SHADGT_UPSCALE_RATIO` overrides it.
  - The window title names the upscaler in use, e.g. `ryGT - Gran Turismo Sport | FSR 3.1.4
    temporal 1280x720 -> 1920x1080`.
  - Depth reaches FSR/XeSS through an R32 copy and the reconstruction is blitted to the native
    image: on AMD, a D32S8 depth view and the view-copy draw faulted the GPU (device lost).
  - DIAG-206 logs a `VK_EXT_device_fault` report when the device is lost.

The launcher's Graphics tab has the matching options: "Temporal upscaling", "Upscaler" (AMD FSR
3.1, AMD FSR 4 (D3D12, WIP), Intel XeSS) and "Output resolution" (render size, from native 1080p
down to 360p for 1080p output).

Known issue (AMD, default renderer): scattered green and red dots on car dashboards (seen on the
R92CP in the garage and in races). They are older than these patches and do not show in a
RenderDoc replay of the same frame. Ruled out so far: FIX-191, image recycling, MSAA, draw-to-draw
barriers, finishing the GPU after every draw, the staging ring, zero-filled new images, present
FSR and zeroed undefined shader registers.

## Radeon RX 400 to RX 9000 (0065)

Patch 0065 applies after 0064. The fixes above were checked against the Vulkan reports of each
Radeon generation's newest Windows driver on vulkan.gpuinfo.org: RX 580 and RX Vega (2.0.283,
Vulkan 1.3.264), RX 5700 XT and RX 6800 (2.0.353), RX 7600 (2.0.406) and RX 9070 XT (2.0.416).
Every generation has what the renderer requires (Vulkan 1.3, push descriptors, vertex attribute
divisor, robustness2). FIX-189 and FIX-211 ask the driver per format, so they follow each card.
The texture formats the RX 7600 cannot use (96-bit R32G32B32, signed 10-bit A2B10G10R10) are
missing on every Radeon, and GT Sport's logs show no textures made in them.
- AMD-301: the 64-wide subgroup requirement goes only to stages the driver lists
  (`requiredSubgroupSizeStages`: compute on every Radeon, fragment from RX 7000 on). It was
  given to every graphics stage, which the Vulkan spec does not allow.
- AMD-302: RX 400/500 have no `shaderFloat16`; translated shaders that use 16-bit floats declare
  Float16, so `VK_AMD_gpu_shader_half_float` (on every Radeon) is enabled in that case.
- XESS-210: if XeSS does not start on a GPU, temporal upscaling switches to FSR 3.1.
- AMD-300 (test switch): `SHADGT_SIMULATE_GPU=gcn|rdna1|rdna2` makes the device report only what
  an RX 400/500/Vega, RX 5000 or RX 6000 driver offers; `SHADGT_SIMULATE_SKIP=<item>,...` keeps
  items real (extension names, `f16`, `gpl`, `v3d`, `af2`, `sub`, `push`, `driver`).

Tested on an RX 7600: races with 0065, and the simulated RX 400/500, RX 5000 and RX 6000 paths to
the menus. A full RX 400/500 simulation (both `VK_EXT_graphics_pipeline_library` and
`VK_EXT_vertex_input_dynamic_state` off in the driver) loses the device in a compute dispatch,
while the same renderer paths with the extensions still enabled in the driver render. That points
at the RDNA 3 driver in that mode rather than the renderer; RX 400/500 run AMD's separate legacy
driver, so only a real card can confirm them. The buffer cache's 4 GB sparse arena pages exceed
the 2 GB `maxBufferSize` every Radeon reports; RDNA 3's driver accepts them.

## Native resource lifetime and reflection fixes (0066)

Apply `0066-native-resource-lifetime-and-reflection-fixes.patch` after the current
0020 and 0062-0065 patches on shadGT `ryty-gpu` at
`1d13c88d2e57bc63ce1586fe0b8f3342b6afdd19`. The lighting-storage, compute-fence
completion and render-target identity fixes (0054-0056) must also be present.
When applying the Camera Manager after the upscaler patches, retain both the
`ryty_camera.h` and `ryty_upscaler_status.h` includes in `ryty_entry.cpp`.
The older local experimental 0047-0059 patch variants are not prerequisites.

This patch retains GPU-only lighting and depth images within a 512 MiB budget,
releases page watches only after their final native resource owner retires,
and invalidates failed host imports after guest mapping changes. It bounds
image/sparse allocations, protects queued pipeline and translated-fetch lifetimes,
and batches page protection while refreshing only the buffer ranges being used.
Menu buffer snapshots and bounded scalar prefixes improve menu performance;
races retain the direct path to avoid geometry corruption. Image downloads use
GPU-local tiling before copying back, and overlapping texture uploads preserve
GPU-written region data.

Normalized native fragment cube sampling uses cube-array views and reconstructed
directions for filtering across face edges. Vertex/compute sampling and unsupported
cube operations retain their existing paths. Translation cache versions invalidate
only the affected shader categories. Vertex-fetch conversions now agree with their
specialization keys. The window title identifies the active renderer.

The Release renderer built successfully and all 2,609 packaged import bindings
resolved. Resource-watch cleanup passed 4,097 ownership/overlap cases. Runtime
checks confirmed restored distant shadows, normal sparks, correct track and
showroom reflections, and improved menu performance. Intermittent menu-icon
corruption and other captured defects remain under investigation. Temporary GPU
timers, road/floor/curve traces, diagnostic shaders, capture helpers, and compiled
binaries are not included in this patch.
