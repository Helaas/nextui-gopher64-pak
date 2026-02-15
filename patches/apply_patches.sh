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

# Patch vi.rs: optional runtime toggle to force limiter limit_freq=1 (idempotent)
python3 << 'PYEOF'
from pathlib import Path

path = Path('src/device/vi.rs')
content = path.read_text()

helper_anchor = "const MAX_LIMIT_FREQ: u64 = 3;\n"
helper_block = """
fn force_limit_freq1_enabled() -> bool {
    static FORCE_LIMIT_FREQ1: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *FORCE_LIMIT_FREQ1
        .get_or_init(|| std::env::var("G64_FORCE_LIMIT_FREQ1").map_or(false, |v| v == "1"))
}
"""
if "fn force_limit_freq1_enabled() -> bool" not in content:
    content = content.replace(helper_anchor, helper_anchor + helper_block + "\n", 1)

speed_fn_anchor = "fn speed_limiter(device: &mut device::Device, speed_limiter_toggled: bool) {\n"
speed_init_block = """    if force_limit_freq1_enabled() && device.vi.limit_freq != 1 {
        device.vi.limit_freq = 1;
        create_limiter(device);
    }

"""
if speed_init_block.strip() not in content:
    content = content.replace(speed_fn_anchor, speed_fn_anchor + speed_init_block, 1)

adaptive_anchor = """    if std::time::Instant::now()
        .duration_since(device.vi.limit_freq_check)
        .as_secs_f64()
        > 1.0
    {
"""
adaptive_guard = """    if force_limit_freq1_enabled() {
        return;
    }

"""
if adaptive_guard.strip() not in content:
    content = content.replace(adaptive_anchor, adaptive_guard + adaptive_anchor, 1)

path.write_text(content)
print('Patched vi.rs: added G64_FORCE_LIMIT_FREQ1 runtime toggle')
PYEOF

echo "--- Patches applied ---"
