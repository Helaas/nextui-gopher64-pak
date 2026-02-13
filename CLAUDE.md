# NextUI Gopher64 Pak

## Project Goal

Create a MinUI/NextUI Emu Pak that wraps the **Gopher64** standalone N64 emulator (Rust-based) for the **tg5050** platform, modeled after the existing [minui-n64-pak](https://github.com/josegonzalez/minui-n64-pak) which wraps mupen64plus for tg5040.

## Reference: minui-n64-pak Structure

The existing N64 pak (for mupen64plus on tg5040) has this structure:

```
N64.pak/
├── launch.sh              # Entry point; sets up env, runs emulator
├── pak.json               # Pak metadata (name, platforms, version)
├── settings.json          # Per-game settings UI definition
├── res/
│   ├── background.png
│   └── menu.json          # In-game menu (Continue/Save/Load/Exit)
├── bin/
│   ├── arm64/             # Architecture-specific binaries
│   │   ├── auto-resume
│   │   ├── gptokeyb2      # Key remapping tool
│   │   ├── launch-mupen64plus
│   │   └── pngwrite
│   └── tg5040/            # Platform-specific binaries
│       ├── gm
│       ├── handle-menu-button
│       └── handle-power-button
├── lib/
│   └── tg5040/            # Platform-specific shared libs
├── mupen64plus/
│   ├── mupen64plus-2.5.9  # Emulator binary
│   ├── mupen64plus-2.6.0
│   ├── config/tg5040/     # Platform configs (input, video, etc.)
│   ├── lib/               # Emulator shared libraries
│   └── plugin/tg5040/     # Video/audio/input/RSP plugins
└── screenshots/
```

Key design patterns:
- `launch.sh` is the main entry point called by MinUI with ROM path as argument
- Uses MinUI environment variables: `$SDCARD_PATH`, `$PLATFORM`, `$LOGS_PATH`, `$USERDATA_PATH`, `$SHARED_USERDATA_PATH`, `$TEMP_ROM`
- `gptokeyb2` maps hardware buttons to keyboard keys for the emulator
- Settings menu shown when R2 is held during game selection
- Supports save states, in-game saves, screenshots, sleep mode
- Platform check: currently only allows `tg5040` (and `tg3040` aliased to it)

## Target Device: TrimUI TG5050

### Hardware Specs (from ADB inspection)
- **SoC**: Allwinner (Longan platform), big.LITTLE with 8 cores
  - 4x Cortex-A55 (CPU part 0xd05) @ up to 1416 MHz (cores 0-3)
  - 4x (likely Cortex-A76) @ up to 2160 MHz (cores 4-7)
- **GPU**: Mali-G57 MP1 (1 core), r0p1, max 888 MHz
- **Display**: 1280x720 (720p), DP and DSI outputs
- **RAM**: ~426MB available to Linux (Buildroot 2022.05)
- **Kernel**: Linux 5.15.147 aarch64, SMP PREEMPT
- **Architecture**: aarch64 (ARMv8.2-A features: fp, asimd, aes, pmull, sha1, sha2, crc32, atomics, fphp, asimdhp, asimdrdm, lrcpc, dcpop, asimddp)

### Software Environment
- **OS**: Buildroot 2022.05 (Allwinner Longan)
- **Vulkan**: Available! libvulkan.so.1.3.296, ICD via libmali.so (Vulkan 1.2.0)
- **SDL**: SDL2 2.32.6 (no SDL3 on device)
- **MinUI tools**: `minui-list`, `minui-presenter` available
- **SD card mount**: `/mnt/sdcard/mmcblk1p1` (FUSE/NTFS)
- **Emus path**: `/mnt/sdcard/mmcblk1p1/Emus/tg5050/`
- **No existing N64 pak** on tg5050

### Key Differences from tg5040
- tg5050 has both `tg5040` and `tg5050` directories under Emus
- Likely higher performance CPU (big.LITTLE vs. tg5040's quad-core)
- Same GPU family (Mali), Vulkan 1.2 support confirmed
- 720p screen (same as tg5040 Brick)

## Gopher64 Emulator Analysis

### Overview
- **Language**: Rust (edition 2024, requires Rust 1.93.1)
- **License**: GPLv3
- **Version**: 1.1.15
- **Video backend**: Vulkan via parallel-rdp (SDL3 window + Vulkan surface)
- **Audio**: SDL3 audio
- **Input**: SDL3 gamepad/joystick
- **GUI**: Slint UI framework (for ROM browser; not needed for pak mode)

### Critical Dependencies
1. **SDL3** (built from source as static lib via `sdl3-sys` crate with `build-from-source-static`)
2. **SDL3-TTF** (built from source as static lib via `sdl3-ttf-sys` crate)
3. **Vulkan** (runtime via volk dynamic loader - loads libvulkan.so at runtime)
4. **parallel-rdp** (C++ RDP renderer, compiled as part of build, uses Vulkan)
5. **Slint** (UI toolkit with Skia renderer - heavy dependency, used for GUI mode)
6. **clang/clang++** (required by `.cargo/config.toml`: `CC=clang`, `CXX=clang++`)
7. **lld** (linker, required by rustflags for aarch64 linux)
8. **bindgen** (generates Rust FFI bindings at build time, needs libclang)

### Build Configuration (`.cargo/config.toml`)
- aarch64 Linux: uses `lld` linker, targets `cortex-a76` CPU
- Requires `clang`/`clang++` as C/C++ compilers, `llvm-ar` as archiver
- C++ code compiled with `-std=c++23` and `-march=armv8.2-a`

### Architecture
- When invoked with a ROM path argument (`gopher64 /path/to/rom.z64`), it directly runs the game (no GUI)
- Supports `--fullscreen` flag
- Supports portable mode (place `portable.txt` next to binary)
- Uses `parallel-rdp` for N64 RDP rendering via Vulkan
- Volk is used to dynamically load Vulkan functions (no compile-time Vulkan SDK linkage needed)

## Toolchain: `ghcr.io/loveretro/tg5050-toolchain:latest`

### What's Available
- **C/C++ cross-compiler**: `aarch64-nextui-linux-gnu-gcc` (GCC 10.3.0, crosstool-NG 1.25.0)
- **Sysroot**: `/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc`
- **SDL2**: Available in sysroot (headers + libs)
- **Mali libs**: `libmali.so` in sysroot
- **pkg-config**: Available with sysroot paths configured
- **Environment**: `UNION_PLATFORM=tg5050`, `CROSS_COMPILE=aarch64-nextui-linux-gnu-`

### What's Missing (for Gopher64 build)
- **No Rust/Cargo** - not installed in the toolchain image
- **No clang/clang++** - only GCC available (gopher64 requires clang)
- **No lld** - no LLVM linker
- **No libclang** - needed by bindgen build dependency
- **No Vulkan headers** - not in sysroot (but volk loads vulkan dynamically, and parallel-rdp includes its own vulkan headers)
- **No SDL3** - only SDL2 in sysroot (but gopher64 builds SDL3 from source as static)
- **No Slint dependencies** - Skia renderer needs various system libs

## Build Strategy / Challenges

### Major Challenges

1. **Rust toolchain not in Docker image**: Need to install Rust 1.93.1 + aarch64-unknown-linux-gnu target inside the container, or use a multi-stage build approach.

2. **Clang/LLD requirement**: Gopher64's build requires clang/clang++ and lld. The toolchain only has GCC. Options:
   - Install LLVM/Clang inside the Docker container
   - Modify `.cargo/config.toml` to use GCC instead (may break C++23 compilation of parallel-rdp)
   - Use a separate build environment and only use the toolchain's sysroot for linking

3. **SDL3 vs SDL2**: Gopher64 uses SDL3 (built from source as static). The device has SDL2. Since SDL3 is statically linked, this should work - the static SDL3 build just needs the right system headers (X11, DRM, etc.) which may or may not be in the sysroot.

4. **Slint/Skia**: The GUI framework is heavy and may be hard to cross-compile. However, for pak mode (CLI with ROM argument), the Slint GUI is only used in the `else` branch when no game argument is provided. We might be able to:
   - Build with Slint anyway (it's required by the code)
   - Consider using a lighter Slint backend (e.g., `renderer-software` instead of `renderer-skia`)

5. **Vulkan on Mali-G57**: The device has Vulkan 1.2 via Mali driver. Gopher64's parallel-rdp needs Vulkan. Since volk dynamically loads vulkan, this should work at runtime. The Vulkan headers are bundled in the parallel-rdp submodule.

6. **CPU target**: `.cargo/config.toml` targets `cortex-a76`. The tg5050 has Cortex-A55 and likely A76 cores. The A55 cores support ARMv8.2-A, which matches the `-march=armv8.2-a` flag used for C/C++ code. The `cortex-a76` Rust target flag should be compatible.

### Recommended Build Approach

1. **Use a custom Dockerfile** that extends the tg5050 toolchain:
   ```dockerfile
   FROM ghcr.io/loveretro/tg5050-toolchain:latest
   # Install Rust
   RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y \
       --default-toolchain 1.93.1 \
       --target aarch64-unknown-linux-gnu
   # Install LLVM/Clang (version matching Rust's LLVM)
   RUN apt-get update && apt-get install -y clang lld llvm libclang-dev
   ```

2. **Configure cross-compilation**:
   - Set `SYSROOT` to the toolchain's sysroot for finding system libs
   - SDL3 builds from source (static), so no system SDL3 needed
   - Point pkg-config to the toolchain sysroot
   - Vulkan headers come from parallel-rdp's bundled vulkan-headers

3. **Build gopher64**:
   ```bash
   cargo build --release --target=aarch64-unknown-linux-gnu
   ```

4. **Create the pak structure** modeled after minui-n64-pak:
   ```
   N64-gopher64.pak/
   ├── launch.sh
   ├── pak.json
   ├── settings.json
   ├── res/
   │   └── menu.json
   ├── bin/
   │   └── tg5050/
   │       ├── gopher64          # The compiled binary
   │       ├── gptokeyb2         # Key remapping (reuse from minui-n64-pak)
   │       ├── handle-menu-button
   │       └── handle-power-button
   └── data/                     # Gopher64 data files (fonts, shaders)
   ```

5. **launch.sh** would be simplified vs mupen64plus version:
   - No plugin system (gopher64 is monolithic)
   - Direct invocation: `gopher64 --fullscreen /path/to/rom.z64`
   - Use portable mode (`portable.txt`) to keep saves in pak directory
   - Still need gptokeyb2 for button remapping
   - Still need power/menu button handling

### Settings (Simpler than mupen64plus)
Gopher64 config options relevant for pak:
- **Video**: upscale, integer_scaling, fullscreen, widescreen, crt shader
- **Emulation**: overclock, disable_expansion_pak
- **Input**: controller profiles, deadzone

### Risks & Open Questions
1. **Performance**: Mali-G57 MP1 (single core) with Vulkan - parallel-rdp is GPU-intensive. May struggle with demanding N64 games. The tg5040's mupen64plus uses software rendering (Rice/Glide64) which is CPU-bound instead.
2. **Vulkan 1.2 vs requirements**: Need to verify parallel-rdp works with Vulkan 1.2 (Mali ICD claims 1.2, loader is 1.3).
3. **Display server**: The device likely uses DRM/KMS directly (no X11/Wayland). SDL3's DRM/KMS backend should handle this when built from source.
4. **glibc compatibility**: The toolchain uses GCC 10.3 (glibc from Buildroot 2022.05). Need to ensure the Rust binary links against a compatible glibc.
5. **Binary size**: Gopher64 with static SDL3 + Slint/Skia could produce a large binary. Consider stripping (already in release profile).
6. **Slint backend**: May need to use `backend-linuxkms` instead of `backend-winit` for headless/DRM rendering, or keep winit if it supports DRM via SDL3.
