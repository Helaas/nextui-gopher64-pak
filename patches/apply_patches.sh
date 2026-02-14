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

# Ensure sdl-unix-console-build feature is enabled (idempotent)
python3 << 'PYEOF'
with open('Cargo.toml', 'r') as f:
    content = f.read()

needle = 'features = ["build-from-source-static"]'
replace = 'features = ["build-from-source-static", "sdl-unix-console-build"]'

if needle in content:
    content = content.replace(needle, replace, 1)

with open('Cargo.toml', 'w') as f:
    f.write(content)
print('Patched Cargo.toml: ensured sdl-unix-console-build feature')
PYEOF

# Patch build.rs: add drm_display.cpp, DRM include path, link libdrm (idempotent)
python3 << 'PYEOF'
with open('build.rs', 'r') as f:
    content = f.read()

def insert_after_once(text: str, anchor: str, line: str) -> str:
    if line in text:
        return text
    return text.replace(anchor, anchor + "\n" + line, 1)

content = insert_after_once(
    content,
    '.file("parallel-rdp/wsi_platform.cpp")',
    '        .file("parallel-rdp/drm_display.cpp")'
)
content = insert_after_once(
    content,
    '.include("parallel-rdp/parallel-rdp-standalone/util")',
    '        .include("/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr/include/libdrm")'
)
content = insert_after_once(
    content,
    'rdp_build.compile("parallel-rdp");',
    '    println!("cargo:rustc-link-lib=drm");'
)

with open('build.rs', 'w') as f:
    f.write(content)
print('Patched build.rs: added drm_display.cpp, DRM includes, libdrm link')
PYEOF

# Patch video.rs: remove SDL_WINDOW_VULKAN flag (idempotent)
python3 << 'PYEOF'
import re

with open('src/ui/video.rs', 'r') as f:
    content = f.read()

# Remove SDL_WINDOW_VULKAN and trailing "|" in all occurrences
content = re.sub(r'sdl3_sys::video::SDL_WINDOW_VULKAN\s*\|\s*', '', content)

with open('src/ui/video.rs', 'w') as f:
    f.write(content)
print('Patched video.rs: removed SDL_WINDOW_VULKAN')
PYEOF

echo "--- Patches applied ---"
