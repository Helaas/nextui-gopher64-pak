#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE_NAME="gopher64-tg5050-builder"

echo "=== Building gopher64 for tg5050 ==="

# Build the Docker image
echo "--- Building Docker image ---"
docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"

# Clone gopher64 source and build inside Docker
echo "--- Building gopher64 binary ---"
docker run --rm \
    -v "$SCRIPT_DIR/bin/tg5050:/output" \
    "$IMAGE_NAME" \
    bash -c '
set -e

# Clone gopher64 with submodules (parallel-rdp)
git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/gopher64/gopher64.git /build/gopher64
cd /build/gopher64

# Add sdl-unix-console-build feature for DRM/KMS-only systems (no X11/Wayland)
sed -i "s/features = \[\"build-from-source-static\"\]/features = [\"build-from-source-static\", \"sdl-unix-console-build\"]/" Cargo.toml

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
