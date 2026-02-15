# nextui-gopher64-pak: Final Findings

Last updated: February 15, 2026

## Project Status

This project is being abandoned due to sustained performance limits on target hardware.

Current result on tg5050:
- Correct image output is achievable.
- Stable 60 FPS is not achievable in real gameplay.
- Sustained performance remains around 15-20 FPS in heavy scenes, with short bursts to ~35 FPS.

## Target Platform

- Device: TrimUI Brick / tg5050 class device
- SoC: Allwinner
- GPU: Mali-G57 MP1
- Display: 1280x720 @ 60Hz
- Driver: libmali (Vulkan 1.2.225, driver 32.0.0)

## Core Technical Findings

## 1) Vulkan WSI path in libmali is broken

The normal SDL KMSDRM Vulkan path cannot be used because:

- `vkGetPhysicalDeviceDisplayPropertiesKHR()` segfaults in `libmali.so`.
- `vkGetPhysicalDeviceSurfaceCapabilitiesKHR()` also segfaults in related paths.
- `VK_KHR_display` is advertised but unusable in practice.

Result:
- Upstream SDL Vulkan surface creation path is not viable on this device.

## 2) Custom no-surface Vulkan path works

A patched path was implemented:

- Vulkan compute/render without SDL Vulkan surface creation.
- Direct DRM scanout path in patched `interface.cpp` / `drm_display.cpp`.

This avoids the libmali WSI crash and renders correctly.

## 3) DRM plane scaling behavior differs between isolated tests and emulator path

Isolated DRM tests (`tests/drm_plane_scale_test`) show:

- `drmModeSetPlane` scaling works at full rate.
- ~63 FPS for 320x240 -> 1280x720 (vsync-limited).

In gopher64 integration, plane path showed corruption in multiple scenarios.

Workaround used:

- Force `drmModeSetCrtc` path (`DRM_DISABLE_PLANE=1`) for correctness.

Tradeoff:

- Correct output, but lower performance and audio underruns/choppiness.

## 4) Zero-copy GPU -> DRM import path was implemented and works

A DMA-BUF import path was added:

- DRM dumb buffer created first.
- Imported into Vulkan as external memory image.
- GPU blit from scanout image into display image.
- Page flip via DRM.

Logs confirm:

- `[gpu_display] Zero-copy GPU->DRM ready (import): 1280x720`

This removed CPU copy/upscale bottlenecks from the display path.

## 5) Display path is not the final bottleneck

Perf telemetry added to frame pipeline:

- `scanout`, `render`, `flip`, `total`, and `gap`.

Observed in real gameplay:

- Stage time often ~4-8 ms total.
- `gap` often ~50-64 ms during low-FPS periods.
- GPU utilization low-to-moderate (~14-32%).

Interpretation:

- The dominant limiter is upstream frame production/pacing, not the final display blit/flip path.

## 6) CPU tuning and big-core pinning did not solve sustained FPS

Added:

- Performance governor handling for little/big clusters.
- Optional process affinity pinning to performance cluster via `.g64-pin-big-core`.

Verified in logs:

- `[interface] Pinned process threads to performance cores (CPU4-CPU7)`

Result:

- No stable improvement to sustained FPS ceiling.
- FPS still falls back to ~15-20 in sustained load.

## What Was Tried

- NEON optimization for CPU pixel conversion/scaling paths.
- Multiple DRM scanout strategies:
  - `drmModeSetCrtc`
  - `drmModeSetPlane`
  - overlay/primary variants
  - vblank / no-vblank / msync variants
- GPU upscaling (`video.upscale` 2x/4x) in parallel-rdp.
- Zero-copy Vulkan -> DRM display import.
- Speed limiter toggles:
  - disable limiter
  - force limiter frequency mode
- CPU affinity and governor tuning.

## Final Conclusion

The project solved correctness and platform compatibility issues, but not sustained gameplay performance.

Most important final insight:

- On this hardware/driver stack, the major remaining FPS loss appears to come from emulation/frame pacing workload, not from scanout or scaling transport.

Given current constraints, there is no clear low-risk patch path to consistently exceed 20 FPS in the target scenarios, so development is stopped.

## Relevant Files

- `patches/interface.cpp`
- `patches/drm_display.cpp`
- `patches/drm_display.hpp`
- `launch.sh`
- `tests/GPU_FINDINGS.md`
- `tests/gpu_probe.c`
- `tests/drm_plane_scale_test.c`
- `tests/drm_setplane_noscale_test.c`

