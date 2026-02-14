#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE_NAME="gopher64-tg5050-builder"

# Pinned gopher64 version — tested with our DRM display patches
GOPHER64_COMMIT="efbeaeab888c25c752d1531149d20cdcbe50c7be"

echo "=== Building gopher64 for tg5050 ==="
echo "--- Commit: $GOPHER64_COMMIT ---"

# Build the Docker image
echo "--- Building Docker image ---"
docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"

# Clone gopher64 source and build inside Docker
echo "--- Building gopher64 binary ---"
docker run --rm \
    -v "$SCRIPT_DIR/bin/tg5050:/output" \
    -v "$SCRIPT_DIR/patches:/patches:ro" \
    "$IMAGE_NAME" \
    bash -c '
set -e

# Clone gopher64 at pinned commit with submodules
git clone https://github.com/gopher64/gopher64.git /build/gopher64
cd /build/gopher64
git checkout '"$GOPHER64_COMMIT"'
git submodule update --init --recursive --depth 1

# Add sdl-unix-console-build feature for DRM/KMS-only systems (no X11/Wayland)
sed -i "s/features = \[\"build-from-source-static\"\]/features = [\"build-from-source-static\", \"sdl-unix-console-build\"]/" Cargo.toml

# Apply all DRM display patches via mounted script
bash /patches/apply_patches.sh

echo "--- Patches applied ---"

# Override .cargo/config.toml for cross-compilation
mkdir -p .cargo
cat > .cargo/config.toml << CARGO_EOF
[target.aarch64-unknown-linux-gnu]
linker = "clang"
rustflags = [
    "-C", "link-arg=--target=aarch64-unknown-linux-gnu",
    "-C", "link-arg=--sysroot=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc",
    "-C", "link-arg=-fuse-ld=lld",
    "-C", "link-arg=-Wl,--allow-shlib-undefined",
    "-C", "link-arg=-lcxx_compat",
]
CARGO_EOF

echo "--- Starting cargo build ---"
cargo build --release --target aarch64-unknown-linux-gnu 2>&1

echo "--- Copying binary to output ---"
cp target/aarch64-unknown-linux-gnu/release/gopher64 /output/gopher64
echo "SUCCESS: Binary copied to /output/gopher64"
'

echo "=== Binary available at $SCRIPT_DIR/bin/tg5050/gopher64 ==="
echo "=== Deploy with: adb push $SCRIPT_DIR /mnt/SDCARD/Emus/tg5050/N64-gopher64.pak/ ==="
