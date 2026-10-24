# Camera Manager tests

These tests compile the actual Camera Manager sources from a separate patched
shadGT checkout. They do not require game files or send force-feedback output.
They currently target Windows using Clang with the MSVC ABI and Windows SDK.

On shadGT `1d13c88d2e57bc63ce1586fe0b8f3342b6afdd19`, apply
`renderer/shadGT-patches/0065-in-game-camera-manager.patch` with `git apply`.
Initialize that checkout's pinned submodules. With the normal Windows Clang/SDK
build environment active, configure this directory with:

```text
cmake -S renderer/camera-manager-tests -B build/camera-manager-tests -G Ninja -DSHADGT=<absolute-patched-shadGT-path> -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build/camera-manager-tests
ctest --test-dir build/camera-manager-tests --output-on-failure
```

For LLVM-MinGW's clang-cl distribution, also pass
`-DCMAKE_CXX_COMPILER_ARG1="--no-default-config --target=x86_64-pc-windows-msvc"`.
Use the same MSVC/SDK environment as the port's renderer build.

The fourteen suites cover input event routing, preset persistence and isolation,
camera-specific ranges, the Roof preset key, native vehicle and camera observation,
camera selection, live orbit with the menu open, real ImGui offscreen rendering,
window recovery, 1080p/4K bounds and resolution changes. Orbit regressions cover
centered sensitivity, continuous stick deadzone response, both axis directions,
independent inversion, global/per-car parity and legacy preset migration.

Executable validation tests cover the nine camera/helper function signatures,
all eight hook slots, PE section bounds and permissions, unrelated PE metadata,
unreadable/guarded memory, installation failures and hook rollback. They use a
synthetic image and do not execute game code. Optionally run
`camera_validation_tests <path-to-converted-GTSport.exe>` to validate a local
image after mapping its sections and applying its PE relocations in private memory;
the executable on disk is not modified and is not part of the test payload.
Startup validation writes `logs/camera-validation.log` beside the game executable.
Each session appends signature results, all eight slot validations, successful
hook installations and the final ready/failure state. No per-frame logging is used.

Configure `renderer/camera-manager-tests/audit` separately with the same toolchain
and `-DSHADGT=<patched-shadGT-path>` to run the original adaptive-trigger installer
policy regression. It requires Python 3 and uses synthetic memory, not hardware.

These standalone tests do not replace RyGT's root `linux-release` test suite and
are not added to the existing CI workflow in this change.
