#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE_NAME="gopher64-tg5050-builder"
GOPHER64_SRC="/build/gopher64"

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
set -euo pipefail

SYSROOT=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc

# Clone gopher64
cd /build
git clone --recursive https://github.com/gopher64/gopher64.git
cd gopher64

# Override .cargo/config.toml for cross-compilation
# The upstream config assumes native compilation with system clang
mkdir -p .cargo
cat > .cargo/config.toml << "CARGOEOF"
[target.aarch64-unknown-linux-gnu]
linker = "clang"
rustflags = [
    "-C", "link-arg=--target=aarch64-unknown-linux-gnu",
    "-C", "link-arg=--sysroot=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc",
    "-C", "link-arg=-fuse-ld=lld",
    "-C", "target-cpu=cortex-a76",
]

[env]
CC = "clang"
CXX = "clang++"
AR = "llvm-ar"
CARGOEOF

# Set cross-compilation environment
export CC="clang"
export CXX="clang++"
export AR="llvm-ar"
export CC_aarch64_unknown_linux_gnu="clang"
export CXX_aarch64_unknown_linux_gnu="clang++"
export AR_aarch64_unknown_linux_gnu="llvm-ar"
export CFLAGS="--target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT} -march=armv8.2-a"
export CXXFLAGS="--target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT} -march=armv8.2-a"
export BINDGEN_EXTRA_CLANG_ARGS="--target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT}"
export PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"
export PKG_CONFIG_PATH="${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig"
export PKG_CONFIG_ALLOW_CROSS=1

echo "--- Starting cargo build ---"
cargo build --release --target=aarch64-unknown-linux-gnu 2>&1

echo "--- Copying binary to output ---"
cp target/aarch64-unknown-linux-gnu/release/gopher64 /output/gopher64
echo "--- Build complete! ---"
'

echo "=== Binary available at $SCRIPT_DIR/bin/tg5050/gopher64 ==="
