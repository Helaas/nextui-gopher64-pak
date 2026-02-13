FROM ghcr.io/loveretro/tg5050-toolchain:latest

# Install host build tools needed for cross-compilation
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    ca-certificates \
    clang \
    lld \
    llvm \
    libclang-dev \
    pkg-config \
    git \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Install Rust toolchain
ENV RUSTUP_HOME=/opt/rust/rustup
ENV CARGO_HOME=/opt/rust/cargo
ENV PATH="/opt/rust/cargo/bin:${PATH}"
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y \
    --default-toolchain 1.93.1 \
    --target aarch64-unknown-linux-gnu \
    --no-modify-path

# Pull EGL/GBM/Vulkan headers + stubs from the device into the sysroot
# so SDL3 and parallel-rdp can find them during cross-compilation.
# We use Mesa's headers since they're standard.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libvulkan-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy Vulkan, EGL, GBM, and KHR headers into the cross-compilation sysroot
RUN cp -r /usr/include/vulkan ${SYSROOT}/usr/include/ 2>/dev/null || true && \
    cp -r /usr/include/vk_video ${SYSROOT}/usr/include/ 2>/dev/null || true

# Create EGL/KHR/GBM headers in sysroot (minimal stubs for SDL3 KMSDRM build)
RUN mkdir -p ${SYSROOT}/usr/include/EGL ${SYSROOT}/usr/include/KHR ${SYSROOT}/usr/include/gbm

# Download Mesa EGL/GBM/KHR headers from the Ubuntu host packages instead
# (GitLab raw URLs may redirect to HTML pages)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libegl-dev libgbm-dev \
    && cp /usr/include/EGL/egl.h ${SYSROOT}/usr/include/EGL/ \
    && cp /usr/include/EGL/eglext.h ${SYSROOT}/usr/include/EGL/ \
    && cp /usr/include/EGL/eglplatform.h ${SYSROOT}/usr/include/EGL/ \
    && cp /usr/include/EGL/eglextchromium.h ${SYSROOT}/usr/include/EGL/ 2>/dev/null || true \
    && cp /usr/include/KHR/khrplatform.h ${SYSROOT}/usr/include/KHR/ \
    && cp /usr/include/gbm.h ${SYSROOT}/usr/include/gbm.h \
    && rm -rf /var/lib/apt/lists/*

# Fix DRM include path: xf86drm.h includes <drm.h> but it lives in drm/ subdir
RUN ln -sf drm/drm.h ${SYSROOT}/usr/include/drm.h && \
    ln -sf drm/drm_mode.h ${SYSROOT}/usr/include/drm_mode.h && \
    ln -sf drm/drm_fourcc.h ${SYSROOT}/usr/include/drm_fourcc.h && \
    ln -sf drm/drm_sarea.h ${SYSROOT}/usr/include/drm_sarea.h

# Create libgbm and libEGL stub .so symlinks pointing to libmali in sysroot
# (At runtime, the real libmali provides these)
RUN cd ${SYSROOT}/usr/lib && \
    ln -sf libmali.so libgbm.so && \
    ln -sf libmali.so libgbm.so.1 && \
    ln -sf libmali.so libEGL.so && \
    ln -sf libmali.so libEGL.so.1 && \
    ln -sf libmali.so libvulkan.so && \
    ln -sf libmali.so libvulkan.so.1

# Create pkg-config files for gbm and egl
RUN echo 'prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: gbm\nDescription: Mesa gbm library\nVersion: 21.0.0\nLibs: -L${libdir} -lgbm\nCflags: -I${includedir}' \
    > ${SYSROOT}/usr/lib/pkgconfig/gbm.pc && \
    echo 'prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: egl\nDescription: Mesa EGL library\nVersion: 21.0.0\nLibs: -L${libdir} -lEGL\nCflags: -I${includedir}' \
    > ${SYSROOT}/usr/lib/pkgconfig/egl.pc

# Install fontconfig headers and create pkg-config file in sysroot
# (the device has libfontconfig.so, but the sysroot is missing it)
RUN apt-get update && apt-get install -y --no-install-recommends libfontconfig1-dev && \
    cp -r /usr/include/fontconfig ${SYSROOT}/usr/include/ && \
    rm -rf /var/lib/apt/lists/*

# Copy real device libraries (pulled via ADB) into the sysroot for linking
COPY sysroot-libs/libfontconfig.so.1.12.0 ${SYSROOT}/usr/lib/
COPY sysroot-libs/libfreetype.so.6.18.3 ${SYSROOT}/usr/lib/
COPY sysroot-libs/libstdc++.so.6.0.28 ${SYSROOT}/usr/lib/
RUN cd ${SYSROOT}/usr/lib && \
    ln -sf libfontconfig.so.1.12.0 libfontconfig.so.1 && \
    ln -sf libfontconfig.so.1 libfontconfig.so && \
    ln -sf libfreetype.so.6.18.3 libfreetype.so.6 && \
    ln -sf libfreetype.so.6 libfreetype.so && \
    ln -sf libstdc++.so.6.0.28 libstdc++.so.6 && \
    ln -sf libstdc++.so.6 libstdc++.so && \
    printf 'prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: fontconfig\nDescription: Font configuration library\nVersion: 2.13.1\nRequires: freetype2\nLibs: -L${libdir} -lfontconfig\nCflags: -I${includedir}\n' \
    > ${SYSROOT}/usr/lib/pkgconfig/fontconfig.pc

# Create a custom CMake toolchain file for SDL3/Slint cross-compilation
# that tells SDL3 we don't need X11/Wayland (tg5050 uses DRM/KMS directly)
RUN printf '\
set(CMAKE_SYSTEM_NAME Linux)\n\
set(CMAKE_SYSTEM_PROCESSOR aarch64)\n\
set(CMAKE_C_COMPILER clang)\n\
set(CMAKE_CXX_COMPILER clang++)\n\
set(CMAKE_AR llvm-ar)\n\
set(CMAKE_SYSROOT /opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc)\n\
set(CMAKE_FIND_ROOT_PATH /opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc)\n\
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n\
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n\
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n\
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n\
set(SDL_UNIX_DESKTOP_WINDOWING OFF CACHE BOOL "" FORCE)\n\
' > /opt/cross-toolchain.cmake

# Also create a cmake initial cache script as fallback
RUN printf 'set(SDL_UNIX_DESKTOP_WINDOWING OFF CACHE BOOL "" FORCE)\n' > /opt/sdl3-init-cache.cmake

# Set up cross-compilation environment for gopher64
# Override CC/CXX to use clang with target triple for cross-compilation
ENV CMAKE_TOOLCHAIN_FILE_aarch64_unknown_linux_gnu="/opt/cross-toolchain.cmake"
ENV CC_aarch64_unknown_linux_gnu="clang"
ENV CXX_aarch64_unknown_linux_gnu="clang++"
ENV AR_aarch64_unknown_linux_gnu="llvm-ar"
ENV CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER="clang"
ENV CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_RUSTFLAGS="-C link-arg=--target=aarch64-unknown-linux-gnu -C link-arg=--sysroot=${SYSROOT} -C link-arg=-fuse-ld=lld -C link-arg=-Wl,--allow-shlib-undefined"
ENV CFLAGS_aarch64_unknown_linux_gnu="--target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT} -march=armv8.2-a"
ENV CXXFLAGS_aarch64_unknown_linux_gnu="--target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT} -march=armv8.2-a"
ENV PKG_CONFIG_SYSROOT_DIR="${SYSROOT}"
ENV PKG_CONFIG_SYSROOT_DIR_aarch64_unknown_linux_gnu="${SYSROOT}"
ENV PKG_CONFIG_PATH="${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig"
ENV PKG_CONFIG_PATH_aarch64_unknown_linux_gnu="${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig"
ENV PKG_CONFIG_ALLOW_CROSS="1"
ENV PKG_CONFIG_aarch64_unknown_linux_gnu="pkg-config"
ENV BINDGEN_EXTRA_CLANG_ARGS_aarch64_unknown_linux_gnu="--target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT}"

# Create a static library with C++ ABI stubs for symbols present in host's
# libstdc++ (GCC 13) but missing from device's libstdc++ (GCC 10).
# These are needed because parallel-rdp is compiled with clang++ using host C++23
# headers, which reference newer libstdc++ symbols. At runtime, the symbols resolve
# from the device's libstdc++ or are unused in practice.
COPY cxx_compat.cpp /tmp/cxx_compat.cpp
RUN clang++ --target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT} -c /tmp/cxx_compat.cpp -o /tmp/cxx_compat.o && \
    llvm-ar rcs ${SYSROOT}/usr/lib/libcxx_compat.a /tmp/cxx_compat.o

WORKDIR /build
