# GPU Findings: Mali-G57 on tg5050

## Hardware

- **GPU**: Mali-G57 MP1 (Valhall architecture)
- **Driver**: ARM proprietary blob `r32p0-01eac1`
- **Vulkan**: 1.2.225 (63 device extensions)
- **OpenGL ES**: 3.2
- **EGL**: 1.4
- **DRM**: `/dev/dri/card0` only (no render node `renderD128`)
- **Display**: DSI-1 (built-in), DP-1 (disconnected)
- **DRM resources**: 2 connectors, 2 CRTCs, 3 encoders

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

## Viable Approaches (ranked by performance)

### 1. Zero-copy: Vulkan DMA-BUF export -> DRM/KMS scanout (RECOMMENDED)

**Performance**: Best. No CPU copies. GPU renders directly to scanout-capable buffers.

**How it works**:
- Create Vulkan device with `VK_KHR_external_memory`, `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`
- Allocate Vulkan images with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`
- Export as DMA-BUF fd via `vkGetMemoryFdKHR`
- Import into DRM as framebuffer via `drmModeAddFB2` (using the DMA-BUF fd + `drmPrimeFDToHandle`)
- Page-flip via `drmModePageFlip` or `drmModeSetCrtc`
- Double/triple buffer by rotating Vulkan images

**What to patch in gopher64**:
- `parallel-rdp/wsi_platform.cpp` -- Replace SDL Vulkan surface with custom DRM/KMS WSI that exports scanout images as DMA-BUFs
- `parallel-rdp/wsi_platform.hpp` -- Add DRM fd, connector/CRTC state, DMA-BUF image pool
- `parallel-rdp/interface.cpp` -- After `processor->scanout()`, blit to the exportable image, trigger DRM page flip
- `src/ui/video.rs` -- Remove `SDL_WINDOW_VULKAN` flag, create plain KMSDRM window (or no window at all)
- Build: link against `libdrm` (already in sysroot)

**Complexity**: High. Requires implementing a mini Vulkan WSI layer with DRM/KMS modesetting + DMA-BUF export + page flip synchronization.

**Risk**: `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier` on this Mali driver might have similar quality issues as `VK_KHR_display`. Needs a focused test to confirm DMA-BUF export actually works before committing to this path.

---

### 2. Vulkan render -> EGL interop -> GBM/DRM scanout

**Performance**: Near-zero-copy. GPU-to-GPU via EGL image import.

**How it works**:
- Create Vulkan device for parallel-rdp rendering (no WSI needed)
- After `processor->scanout()`, export the Vulkan image as DMA-BUF fd
- Import into EGL via `EGL_EXT_image_dma_buf_import` -> `eglCreateImageKHR`
- Bind to a GLES texture, draw a fullscreen quad
- `eglSwapBuffers` on GBM surface -> `gbm_surface_lock_front_buffer` -> `drmModePageFlip`

**What to patch in gopher64**:
- Same Vulkan changes as approach 1 for scanout image export
- Add EGL/GBM display initialization (open DRM fd, create GBM device, EGL display+context+surface)
- Add GLES blit shader (trivial fullscreen textured quad)
- Manage GBM front/back buffer locking and DRM page flips

**Complexity**: High. Two GPU API surfaces (Vulkan + EGL/GLES), DMA-BUF interop between them.

**Risk**: Same DMA-BUF export risk as approach 1. Additionally, the EGL interop path is more complex and harder to debug.

---

### 3. Vulkan render -> CPU readback -> DRM dumb buffer scanout

**Performance**: Good enough. One `vkMapMemory` + `memcpy` per frame.

At N64 native resolution (320x240x4 bytes x 60fps) = **18 MB/s**. Even at 2x upscale (640x480) = **73 MB/s**. Both are negligible on this SoC.

**How it works**:
- Create Vulkan device for parallel-rdp rendering (no WSI, no swapchain)
- After `processor->scanout()`, copy rendered image to a host-visible staging buffer
- `vkMapMemory` to get CPU pointer
- `memcpy` into a DRM dumb buffer (mmap'd)
- `drmModeSetCrtc` or `drmModePageFlip` to display

**What to patch in gopher64**:
- `parallel-rdp/wsi_platform.cpp` -- Stub out surface creation (return null or headless)
- `parallel-rdp/interface.cpp` -- Replace swapchain blit with: scanout -> vkCmdCopyImageToBuffer -> map -> memcpy to DRM dumb buffer -> page flip
- `src/ui/video.rs` -- Remove `SDL_WINDOW_VULKAN`, add DRM display setup
- Build: link against `libdrm`

**Complexity**: Medium. No GPU interop needed. DRM modesetting is the main new code (connector enumeration, CRTC setup, dumb buffer allocation, page flip).

**Risk**: Low. All components individually confirmed working by the probe. The CPU copy is the only overhead, and it's tiny at N64 resolutions.

---

### 4. Vulkan render -> CPU readback -> SDL2/3 software renderer

**Performance**: Acceptable. Same readback as approach 3, but SDL handles display.

**How it works**:
- Create Vulkan device for parallel-rdp (no WSI)
- Readback frames to CPU
- Create SDL window (non-Vulkan), create SDL_Renderer + SDL_Texture
- `SDL_UpdateTexture` + `SDL_RenderCopy` + `SDL_RenderPresent`

**What to patch in gopher64**:
- Same Vulkan changes as approach 3
- Replace SDL Vulkan window with SDL software/EGL window
- Use SDL's 2D renderer API for display

**Complexity**: Low-Medium. SDL handles modesetting and display. Less platform-specific code.

**Risk**: Low. But SDL3's KMSDRM non-Vulkan path might use EGL internally (which works) or software rendering (slower). Need to verify SDL3 KMSDRM actually works without Vulkan.

---

## Recommendation

**Start with Approach 3 (CPU readback -> DRM dumb buffer)**, then optimize to Approach 1 (zero-copy DMA-BUF) if performance is insufficient.

**Rationale**:
- Approach 3 is the safest -- every component is individually confirmed working by the probe
- The CPU copy overhead (18-73 MB/s) is negligible vs the SoC's memory bandwidth
- It requires no GPU interop, no DMA-BUF export (which might hit more Mali bugs), and no EGL
- It can be implemented as a relatively contained patch to gopher64's parallel-rdp integration
- If we later need zero-copy, Approach 1 shares the same DRM modesetting code -- only the buffer source changes

**Before starting, we should write one more focused test**: confirm that DRM modesetting + dumb buffer scanout actually displays pixels on the screen (set CRTC with a solid-color dumb buffer). This validates the display output path end-to-end before we invest in patching gopher64.
