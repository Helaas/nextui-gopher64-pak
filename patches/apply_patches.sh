#!/bin/bash
# Apply DRM display patches to gopher64 source tree
# Run from the gopher64 source root
set -e

echo "--- Applying DRM display patches ---"

# Replace WSI platform (bypass broken VK_KHR_display on Mali-G57)
cp /patches/wsi_platform.hpp parallel-rdp/wsi_platform.hpp
cp /patches/wsi_platform.cpp parallel-rdp/wsi_platform.cpp

# Replace interface.cpp (Vulkan compute + DRM readback scanout)
cp /patches/interface.cpp parallel-rdp/interface.cpp

# Add DRM display module
cp /patches/drm_display.hpp parallel-rdp/drm_display.hpp
cp /patches/drm_display.cpp parallel-rdp/drm_display.cpp

# Patch build.rs: add drm_display.cpp, DRM include path, link libdrm
python3 << 'PYEOF'
import re

with open('build.rs', 'r') as f:
    content = f.read()

# Add drm_display.cpp after wsi_platform.cpp
content = content.replace(
    '.file("parallel-rdp/wsi_platform.cpp")',
    '.file("parallel-rdp/wsi_platform.cpp")\n        .file("parallel-rdp/drm_display.cpp")'
)

# Add DRM include path after parallel-rdp-standalone/util include
content = content.replace(
    '.include("parallel-rdp/parallel-rdp-standalone/util")',
    '.include("parallel-rdp/parallel-rdp-standalone/util")\n        .include("/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/include/libdrm")'
)

# Add libdrm link directive after rdp_build.compile
content = content.replace(
    'rdp_build.compile("parallel-rdp");',
    'rdp_build.compile("parallel-rdp");\n    println!("cargo:rustc-link-lib=drm");'
)

with open('build.rs', 'w') as f:
    f.write(content)
print('Patched build.rs: added drm_display.cpp, DRM includes, libdrm link')
PYEOF

# Patch video.rs: remove SDL_WINDOW_VULKAN flag
python3 << 'PYEOF'
import re

with open('src/ui/video.rs', 'r') as f:
    content = f.read()

# Remove SDL_WINDOW_VULKAN and the following | on the next line
content = re.sub(r'sdl3_sys::video::SDL_WINDOW_VULKAN\s*\|\s*', '', content, count=1)

with open('src/ui/video.rs', 'w') as f:
    f.write(content)
print('Patched video.rs: removed SDL_WINDOW_VULKAN')
PYEOF

echo "--- Patches applied ---"
