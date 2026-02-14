# GPU Findings: Mali-G57 on tg5050

## Hardware

- **GPU**: Mali-G57 MP1 (Valhall architecture)
- **Driver**: ARM proprietary blob `r32p0-01eac1`
- **Vulkan**: 1.2.225 (63 device extensions)
- **OpenGL ES**: 3.2
- **EGL**: 1.4
- **DRM**: `/dev/dri/card0` only (no render node `renderD128`)
- **Display**: DSI-1 (type=16), 1280x720 @ 60Hz
- **DRM resources**: 2 connectors, 2 CRTCs, 3 encoders
- **DRM planes**: Primary (id=93, 36 formats), Overlay (id=101, 36 formats)

## What Works

| Capability | Status | Notes |
|---|---|---|
| Vulkan instance (no WSI) | PASS | |
| Vulkan device (compute-only) | PASS | |
| Graphics + compute queue (family 0) | PASS | flags=0x7, count=2 |
| Host-visible coherent memory | PASS | type 0 |
| Buffer create + mmap + write | PASS | |
| Command pool | PASS | |
| `VK_KHR_swapchain` | PASS | Available as device ext |
| `VK_KHR_external_memory` | PASS | |
| `VK_KHR_external_memory_fd` | PASS | |
| `VK_EXT_external_memory_dma_buf` | PASS | |
| `VK_EXT_image_drm_format_modifier` | PASS | |
| `VK_KHR_external_fence` + `_fd` | PASS | |
| `VK_KHR_external_semaphore` + `_fd` | PASS | |
| `VK_EXT_queue_family_foreign` | PASS | |
| `VK_KHR_descriptor_update_template` | PASS | |
| `VK_EXT_headless_surface` (create) | PASS | Surface created, queue reports presentation support |
| GBM device + BO + surface | PASS | 640x480 XRGB8888, scanout+rendering |
| EGL on GBM | PASS | eglGetDisplay(gbm_dev) works |
| GLES 3.2 context | PASS | glClear works, GPU rendering confirmed |
| EGL window surface on GBM | PASS | eglCreateWindowSurface works |
| eglSwapBuffers | PASS | Display pipeline operational |
| gbm_surface_lock_front_buffer | PASS | Scanout-ready BO available after swap |
| DRM dumb buffer create + mmap | PASS | CPU-writable scanout buffer works |
| DRM scanout (drmModeSetCrtc) | PASS | Visually confirmed: solid colors + test pattern display correctly |
| DRM primary plane scaling | PASS | 320x240 -> 1280x720 and 640x480 -> 1280x720 both work |
| DRM overlay plane scaling | PASS | Same scaling works, plus centered 320x240 -> 960x720 |
| DRM plane hw-scaled page flips | PASS | 63 fps (320x240 -> 1280x720), vsync-limited |
| `EGL_KHR_image_base` | PASS | |
| `EGL_EXT_image_dma_buf_import` | PASS | |
| `EGL_KHR_surfaceless_context` | PASS | |
| `EGL_KHR_fence_sync` | PASS | |
| `EGL_ANDROID_native_fence_sync` | PASS | |

## What's Broken

| Capability | Status | Notes |
|---|---|---|
| `vkGetPhysicalDeviceDisplayPropertiesKHR` | **SEGFAULT** | Crashes inside libmali.so |
| `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` (headless) | **SEGFAULT** | Crashes inside libmali.so |
| `VK_KHR_display` (all surface creation) | **BROKEN** | Cannot enumerate displays at all |
| SDL3 KMSDRM Vulkan surface | **BROKEN** | Depends on VK_KHR_display |
| `VK_KHR_push_descriptor` | N/A | Not available |
| `VK_EXT_external_memory_host` | N/A | Not available |

The Mali driver advertises `VK_KHR_display` but the implementation segfaults on the very first call (`vkGetPhysicalDeviceDisplayPropertiesKHR`). This is not a permissions or DRM-master issue -- our standalone test (no SDL, no DRM fd held) also crashes. The headless surface can be *created* but querying its capabilities also segfaults, making `VK_EXT_headless_surface` unusable for swapchain creation.

## Why gopher64 Crashes

gopher64 -> parallel-rdp WSI -> `SDL_Vulkan_CreateSurface()` -> SDL3 KMSDRM Vulkan -> `vkGetPhysicalDeviceDisplayPropertiesKHR()` -> **SEGFAULT in libmali.so**

SDL3's KMSDRM Vulkan backend has exactly one code path: enumerate displays via `VK_KHR_display`, match against the DRM connector, then call `vkCreateDisplayPlaneSurfaceKHR`. Since display enumeration crashes, there is no workaround within SDL3's existing KMSDRM Vulkan code.

---

## Display Throughput Benchmarks

Measured on-device with `drm_scanout_test` and `drm_plane_scale_test`:

| Method | Resolution | Fill size | FPS | Notes |
|---|---|---|---|---|
| drmModeSetCrtc (dumb buf) | 1280x720 | 3.7 MB | 26 fps | CPU-fill bottlenecked |
| drmModeSetPlane hw-scaled | 320x240 -> 1280x720 | 0.3 MB | **63 fps** | **Vsync-limited (60Hz)** |
| drmModeSetPlane hw-scaled | 640x480 -> 1280x720 | 1.2 MB | ~60 fps | Also vsync-limited |

**Key finding**: The Allwinner display engine has a hardware scaler on both primary and overlay planes. A 320x240 dumb buffer can be scaled to 1280x720 at full 60fps with zero CPU scaling overhead. The bottleneck in the unscaled path is purely the CPU `memset` of 3.7MB per frame -- the display hardware itself is not the limiter.

---

## Viable Approaches (ranked by performance)

### 1. Vulkan render -> CPU readback -> DRM plane hw-scaled scanout (RECOMMENDED)

**Performance**: Full 60fps. Vulkan renders at N64 native res, CPU copies 307KB/frame, display HW scales to 1280x720.

**How it works**:
- Create Vulkan device for parallel-rdp rendering (no WSI, no swapchain needed)
- After `processor->scanout()`, copy rendered image to a host-visible staging buffer via `vkCmdCopyImageToBuffer`
- `vkMapMemory` to get CPU pointer, `memcpy` 307KB into a DRM dumb buffer (mmap'd)
- `drmModeSetPlane` with src=320x240 dst=1280x720, display controller scales in hardware
- Double-buffer: alternate between two dumb buffers

**What to patch in gopher64**:
- `parallel-rdp/wsi_platform.cpp` -- Replace SDL surface with a no-op (return VK_NULL_HANDLE, skip surface entirely)
- `parallel-rdp/wsi_platform.hpp` -- Add DRM fd, connector/CRTC/plane IDs, dumb buffer pool
- `parallel-rdp/interface.cpp` -- Replace swapchain blit with: scanout -> vkCmdCopyImageToBuffer -> map -> memcpy to dumb buf -> drmModeSetPlane
- `src/ui/video.rs` -- Remove `SDL_WINDOW_VULKAN` flag; add DRM modesetting init
- Build: link against `libdrm` (already in sysroot)

**Complexity**: Medium. DRM modesetting is the main new code (~200 lines: connector enumeration, CRTC setup, dumb buffer allocation, plane config). No GPU interop needed.

**Risk**: Low. Every component individually confirmed working. CPU readback of 307KB at 60fps = 18 MB/s, trivial on this SoC. HW scaling confirmed at 63fps.

---

### 2. Zero-copy: Vulkan DMA-BUF export -> DRM plane scanout

**Performance**: Best theoretical. No CPU copies. GPU renders directly to scanout-capable buffers.

**How it works**:
- Create Vulkan device with `VK_KHR_external_memory`, `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`
- Allocate Vulkan images with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`
- Export as DMA-BUF fd via `vkGetMemoryFdKHR`
- Import into DRM as framebuffer via `drmPrimeFDToHandle` + `drmModeAddFB2`
- `drmModeSetPlane` with hw scaling
- Double/triple buffer by rotating Vulkan images

**What to patch in gopher64**:
- Same DRM modesetting as approach 1
- `wsi_platform.cpp` -- Allocate exportable Vulkan images, export DMA-BUF fds, import as DRM FBs
- More complex buffer lifecycle (Vulkan owns the memory, DRM references it)

**Complexity**: High. Requires implementing DMA-BUF export + DRM import + synchronization between Vulkan rendering and DRM scanout.

**Risk**: Medium. `VK_EXT_external_memory_dma_buf` on this Mali driver might have quality issues similar to `VK_KHR_display`. Would need a focused test before committing. The marginal performance gain over approach 1 (eliminating a 307KB memcpy) is unlikely to matter.

---

### 3. Vulkan render -> EGL interop -> GBM/DRM scanout

**Performance**: Near-zero-copy. GPU-to-GPU via EGL image import.

**How it works**:
- Create Vulkan device for parallel-rdp rendering (no WSI needed)
- After `processor->scanout()`, export the Vulkan image as DMA-BUF fd
- Import into EGL via `EGL_EXT_image_dma_buf_import` -> `eglCreateImageKHR`
- Bind to a GLES texture, draw a fullscreen quad (GLES does the scaling)
- `eglSwapBuffers` on GBM surface -> `gbm_surface_lock_front_buffer` -> `drmModePageFlip`

**What to patch in gopher64**:
- Same Vulkan DMA-BUF export as approach 2
- Add EGL/GBM display initialization
- Add GLES blit shader
- Manage GBM front/back buffer locking and DRM page flips

**Complexity**: High. Two GPU APIs (Vulkan + EGL/GLES), DMA-BUF interop between them.

**Risk**: Same DMA-BUF export risk as approach 2. More moving parts.

---

### 4. Vulkan render -> CPU readback -> SDL2/3 software renderer

**Performance**: Acceptable but suboptimal. SDL adds overhead vs direct DRM.

**How it works**:
- Create Vulkan device for parallel-rdp (no WSI)
- Readback frames to CPU
- Create SDL window (non-Vulkan), create SDL_Renderer + SDL_Texture
- `SDL_UpdateTexture` + `SDL_RenderCopy` + `SDL_RenderPresent`

**Complexity**: Low-Medium. SDL handles modesetting and display.

**Risk**: Low. But SDL3's KMSDRM non-Vulkan path adds unknown overhead (EGL context, texture upload, blit). No hw-plane-scaling control.

---

## Recommendation

**Approach 1: Vulkan render -> CPU readback -> DRM plane hw-scaled scanout.**

**Rationale**:
- **Every component is confirmed working** on this device by three separate test programs
- **63 fps proven** with hw-scaled dumb buffer flips — saturates the 60Hz vsync
- **307KB/frame readback** at 60fps = 18 MB/s, negligible vs SoC memory bandwidth
- **No DMA-BUF export needed** — avoids risking more Mali driver bugs
- **No EGL/GLES needed** — single GPU API (Vulkan), simple display path (DRM)
- **Shares DRM code with approach 2** — if we ever need zero-copy, the upgrade path is clean
- **Medium complexity** — ~200 lines of DRM modesetting + ~50 lines of readback replaces the broken SDL3 Vulkan WSI

The zero-copy DMA-BUF approach (2) is only worth pursuing if profiling shows the 18 MB/s memcpy is actually a bottleneck, which is very unlikely at N64 resolutions.

---

## Test Programs

All in `tests/`:

| File | Purpose |
|---|---|
| `gpu_probe.c` | Comprehensive GPU capability scan (Vulkan, EGL, GBM, DRM) |
| `drm_scanout_test.c` | End-to-end DRM dumb buffer display (solid colors + test pattern + flip benchmark) |
| `drm_plane_scale_test.c` | DRM plane hw scaling (320x240/640x480 -> 1280x720, throughput comparison) |

Cross-compile all with Docker toolchain:
```bash
docker run --rm -v $(pwd)/tests:/src -v /tmp:/output gopher64-tg5050-builder \
  bash -c 'for f in gpu_probe drm_scanout_test drm_plane_scale_test; do
    clang --target=aarch64-unknown-linux-gnu --sysroot=$SYSROOT -fuse-ld=lld \
      -I$SYSROOT/usr/include -I$SYSROOT/usr/include/libdrm -L$SYSROOT/usr/lib \
      -o /output/$f /src/$f.c -ldrm -ldl 2>&1
  done'
```

Run on device:
```bash
adb push /tmp/gpu_probe /tmp/drm_scanout_test /tmp/drm_plane_scale_test /tmp/
adb shell "/tmp/gpu_probe --safe"
adb shell "/tmp/drm_scanout_test"
adb shell "/tmp/drm_plane_scale_test"
```
