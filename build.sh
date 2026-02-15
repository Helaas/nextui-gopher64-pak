#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE_NAME="gopher64-tg5050-builder"
GOPHER64_REPO_URL="https://github.com/gopher64/gopher64.git"
FORCE_BUILD="${FORCE_BUILD:-0}"
UPDATE_MIRROR="${UPDATE_MIRROR:-0}"

# Pinned gopher64 version — tested with our DRM display patches
GOPHER64_COMMIT="efbeaeab888c25c752d1531149d20cdcbe50c7be"

CACHE_ROOT="$SCRIPT_DIR/.cache"
STATE_DIR="$CACHE_ROOT/state"
GIT_CACHE_DIR="$CACHE_ROOT/git"
SRC_CACHE_DIR="$CACHE_ROOT/src"
CARGO_HOME_CACHE="$CACHE_ROOT/cargo/home"
CARGO_TARGET_CACHE="$CACHE_ROOT/cargo/target"
OUTPUT_DIR="$SCRIPT_DIR/bin/tg5050"
OUTPUT_BIN="$OUTPUT_DIR/gopher64"

GIT_MIRROR="$GIT_CACHE_DIR/gopher64.git"
DOCKER_HASH_FILE="$STATE_DIR/docker-input.hash"
BUILD_HASH_FILE="$STATE_DIR/build-input.hash"

echo "=== Building gopher64 for tg5050 ==="
echo "--- Commit: $GOPHER64_COMMIT ---"

for arg in "$@"; do
	case "$arg" in
	--force)
		FORCE_BUILD=1
		;;
	--update-mirror)
		UPDATE_MIRROR=1
		;;
	--help|-h)
		echo "Usage: ./build.sh [--force] [--update-mirror]"
		echo "  --force    Ignore input-hash fast-path and run build pipeline."
		echo "  --update-mirror  Refresh cached gopher64 git mirror from GitHub."
		exit 0
		;;
	*)
		echo "Unknown argument: $arg" >&2
		echo "Usage: ./build.sh [--force] [--update-mirror]" >&2
		exit 1
		;;
	esac
done

mkdir -p "$STATE_DIR" "$GIT_CACHE_DIR" "$SRC_CACHE_DIR" "$CARGO_HOME_CACHE" "$CARGO_TARGET_CACHE" "$OUTPUT_DIR"

if command -v shasum >/dev/null 2>&1; then
	HASH_CMD=(shasum -a 256)
elif command -v sha256sum >/dev/null 2>&1; then
	HASH_CMD=(sha256sum)
else
	echo "ERROR: neither shasum nor sha256sum is available." >&2
	exit 1
fi

hash_file() {
	"${HASH_CMD[@]}" "$1" | awk "{print \$1}"
}

hash_stdin() {
	"${HASH_CMD[@]}" | awk "{print \$1}"
}

compute_docker_hash() {
	{
		echo "dockerfile $(hash_file "$SCRIPT_DIR/Dockerfile")"
		echo "cxx_compat $(hash_file "$SCRIPT_DIR/cxx_compat.cpp")"
		while IFS= read -r file; do
			echo "${file#$SCRIPT_DIR/} $(hash_file "$file")"
		done < <(find "$SCRIPT_DIR/sysroot-libs" -type f | sort)
	} | hash_stdin
}

compute_source_hash() {
	{
		echo "commit $GOPHER64_COMMIT"
		echo "build.sh $(hash_file "$SCRIPT_DIR/build.sh")"
		while IFS= read -r file; do
			echo "${file#$SCRIPT_DIR/} $(hash_file "$file")"
		done < <(find "$SCRIPT_DIR/patches" -type f | sort)
	} | hash_stdin
}

DOCKER_HASH="$(compute_docker_hash)"
SOURCE_HASH="$(compute_source_hash)"
BUILD_HASH="$(printf '%s\n%s\n' "$DOCKER_HASH" "$SOURCE_HASH" | hash_stdin)"

if [ "$FORCE_BUILD" != "1" ] && [ -f "$OUTPUT_BIN" ] && [ -f "$BUILD_HASH_FILE" ] && [ "$(cat "$BUILD_HASH_FILE")" = "$BUILD_HASH" ]; then
	echo "--- No relevant changes detected. Reusing existing binary: $OUTPUT_BIN ---"
	echo "--- Run './build.sh --force' to force a rebuild ---"
	exit 0
fi

if [ "$FORCE_BUILD" = "1" ]; then
	echo "--- Force build enabled; skipping fast-path hash check ---"
fi

if [ ! -d "$GIT_MIRROR" ]; then
	echo "--- Creating git mirror cache at $GIT_MIRROR ---"
	git clone --mirror "$GOPHER64_REPO_URL" "$GIT_MIRROR"
else
	if [ "$UPDATE_MIRROR" = "1" ]; then
		echo "--- Updating git mirror cache ---"
		git -C "$GIT_MIRROR" remote update --prune
	else
		echo "--- Reusing cached git mirror (skip network update). Use --update-mirror to refresh. ---"
	fi
fi

if docker image inspect "$IMAGE_NAME" >/dev/null 2>&1 && [ -f "$DOCKER_HASH_FILE" ] && [ "$(cat "$DOCKER_HASH_FILE")" = "$DOCKER_HASH" ]; then
	echo "--- Reusing existing Docker image: $IMAGE_NAME ---"
else
	echo "--- Building Docker image ---"
	docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"
	printf '%s\n' "$DOCKER_HASH" >"$DOCKER_HASH_FILE"
fi

echo "--- Building gopher64 binary ---"
docker run --rm -i \
	-e GOPHER64_COMMIT="$GOPHER64_COMMIT" \
	-e SOURCE_HASH="$SOURCE_HASH" \
	-e UPDATE_MIRROR="$UPDATE_MIRROR" \
	-v "$OUTPUT_DIR:/output" \
	-v "$SCRIPT_DIR/patches:/patches:ro" \
	-v "$GIT_MIRROR:/git-cache/gopher64.git:ro" \
	-v "$SRC_CACHE_DIR:/cache/src" \
	-v "$CARGO_HOME_CACHE:/cache/cargo-home" \
	-v "$CARGO_TARGET_CACHE:/cache/cargo-target" \
	"$IMAGE_NAME" \
	bash -seu <<'CONTAINER_EOF'
set -euo pipefail

export CARGO_HOME=/cache/cargo-home
export CARGO_TARGET_DIR=/cache/cargo-target

SRC_DIR="/cache/src/gopher64-${GOPHER64_COMMIT}"
PATCH_MARKER="${SRC_DIR}/.patch-source-hash"

if [ ! -d "${SRC_DIR}/.git" ]; then
	echo "--- Cloning gopher64 source into cache ---"
	git clone /git-cache/gopher64.git "${SRC_DIR}"
fi

cd "${SRC_DIR}"

if ! git cat-file -e "${GOPHER64_COMMIT}^{commit}" 2>/dev/null; then
	if [ "${UPDATE_MIRROR:-0}" = "1" ]; then
		git fetch /git-cache/gopher64.git "${GOPHER64_COMMIT}" --depth 1
	else
		echo "ERROR: commit ${GOPHER64_COMMIT} missing from local mirror cache." >&2
		echo "Run ./build.sh --update-mirror to refresh cached git data." >&2
		exit 1
	fi
fi

current_commit="$(git rev-parse --verify HEAD 2>/dev/null || true)"
need_checkout_refresh=0
need_patch_refresh=0

if [ "$current_commit" != "$GOPHER64_COMMIT" ]; then
	need_checkout_refresh=1
fi

if [ ! -f "$PATCH_MARKER" ] || [ "$(cat "$PATCH_MARKER")" != "$SOURCE_HASH" ]; then
	need_patch_refresh=1
fi

if [ "$need_checkout_refresh" -eq 1 ]; then
	echo "--- Commit changed or source missing; refreshing checkout/submodules ---"
	git reset --hard
	git clean -fdx
	git checkout --detach "$GOPHER64_COMMIT"
	git submodule sync --recursive
	git submodule update --init --recursive --depth 1
	# Fresh checkout always needs patch reapply.
	need_patch_refresh=1
fi

if [ "$need_patch_refresh" -eq 1 ]; then
	echo "--- Patch set changed; reapplying local patches only ---"
	# apply_patches.sh is idempotent and can run on an already patched tree.
	bash /patches/apply_patches.sh
	printf '%s\n' "$SOURCE_HASH" >"$PATCH_MARKER"
else
	echo "--- Reusing cached patched source tree ---"
fi

# Ensure cross-compilation config exists (cheap, deterministic).
mkdir -p .cargo
cat > .cargo/config.toml << CARGO_EOF
[target.aarch64-unknown-linux-gnu]
linker = "clang"
rustflags = [
    "-C", "target-cpu=cortex-a55",
    "-C", "link-arg=--target=aarch64-unknown-linux-gnu",
    "-C", "link-arg=--sysroot=/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc",
    "-C", "link-arg=-fuse-ld=lld",
    "-C", "link-arg=-Wl,--allow-shlib-undefined",
    "-C", "link-arg=-lcxx_compat",
]
CARGO_EOF

echo "--- Starting cargo build (incremental cache enabled) ---"
export CARGO_PROFILE_RELEASE_LTO=thin
export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
cargo build --release --target aarch64-unknown-linux-gnu

echo "--- Copying binary to output ---"
cp "$CARGO_TARGET_DIR/aarch64-unknown-linux-gnu/release/gopher64" /output/gopher64
echo "SUCCESS: Binary copied to /output/gopher64"
CONTAINER_EOF

printf '%s\n' "$BUILD_HASH" >"$BUILD_HASH_FILE"

echo "=== Binary available at $SCRIPT_DIR/bin/tg5050/gopher64 ==="
echo "=== Deploy with: adb push $SCRIPT_DIR /mnt/SDCARD/Emus/tg5050/N64-gopher64.pak/ ==="
