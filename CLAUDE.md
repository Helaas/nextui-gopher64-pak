# NextUI Gopher64 Pak

## Project Goal

A MinUI/NextUI Emu Pak wrapping **Gopher64** (Rust N64 emulator with Vulkan parallel-rdp) for the **tg5050** platform, modeled after [minui-n64-pak](https://github.com/josegonzalez/minui-n64-pak) (mupen64plus for tg5040).

## Target Device: TrimUI TG5050

- **SoC**: Allwinner, big.LITTLE — 4x Cortex-A55 @1416MHz + 4x A76 @2160MHz
- **GPU**: Mali-G57 MP1, Vulkan 1.2 (via libmali.so)
- **Display**: 1280x720 (DRM/KMS, no X11/Wayland)
- **Kernel**: Linux 5.15.147 aarch64
- **Toolchain GCC**: 10.3.0 (libstdc++ 6.0.28)
- **Sysroot**: `/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc` (env: `$SYSROOT`)

## Pak Structure

```
N64-gopher64.pak/
├── launch.sh              # Entry point called by MinUI
├── pak.json               # Pak metadata
├── settings.json          # Per-game settings UI (CPU mode)
├── res/menu.json          # In-game menu (Continue/Save/Load/Exit)
├── data/
│   └── default_input_profile.json  # Gamepad mapping
└── bin/tg5050/
    ├── gopher64            # Cross-compiled binary (~40MB)
    ├── handle-power-button # Sleep/wake handler
    └── handle-menu-button  # In-game menu handler
```

## Building

### Prerequisites
- Docker
- Device libraries in `sysroot-libs/` (pulled from tg5050 via ADB):
  - `libfontconfig.so.1.12.0`
  - `libfreetype.so.6.18.3`
  - `libstdc++.so.6.0.28`

### Build Command
```bash
./build.sh
```

This builds a Docker image extending `ghcr.io/loveretro/tg5050-toolchain:latest` with Rust 1.93.1 + clang/lld, then cross-compiles gopher64 for aarch64 inside the container.

### Deploy
```bash
# Push only the runtime files (not build artifacts)
adb push bin/ /mnt/SDCARD/Emus/tg5050/N64-gopher64.pak/bin/
adb push launch.sh pak.json settings.json /mnt/SDCARD/Emus/tg5050/N64-gopher64.pak/
adb push res/ /mnt/SDCARD/Emus/tg5050/N64-gopher64.pak/res/
adb push data/ /mnt/SDCARD/Emus/tg5050/N64-gopher64.pak/data/
```

## Cross-Compilation Details

Key challenges resolved during build:

1. **SDL3 on DRM/KMS**: Added `sdl-unix-console-build` feature to sdl3-sys crate (bypasses X11/Wayland requirement)
2. **Fontconfig/Freetype**: Real device `.so` files used for linking (stubs were insufficient — Skia references 30+ Fc* symbols)
3. **libstdc++ ABI mismatch**: Device has GCC 10 libstdc++ (6.0.28), but parallel-rdp compiles with clang++ using host GCC 13 C++ headers. Two missing symbols resolved via `cxx_compat.cpp` → `libcxx_compat.a`:
   - `std::__throw_bad_array_new_length()`
   - `std::__cxx11::basic_string<char>::_M_replace_cold()`
4. **Shared lib undefined symbols**: `--allow-shlib-undefined` linker flag needed because device glibc is older than what host libstdc++ references

## MinUI Integration

- **Environment vars**: `$SDCARD_PATH`, `$PLATFORM`, `$LOGS_PATH`, `$USERDATA_PATH`, `$SHARED_USERDATA_PATH`, `$TEMP_ROM`
- **Portable mode**: `portable.txt` placed next to binary; saves go to `portable_data/`
- **Save sharing**: `portable_data/data/saves` symlinked to `$SHARED_USERDATA_PATH` for MinUI save management
- **Settings menu**: R2 held during game selection shows CPU mode choice (ondemand/performance)
- **Controller**: tg5050 exposes as Xbox 360 compatible (`TRIMUI Player1`, Vendor=045e Product=028e)
