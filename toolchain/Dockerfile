FROM debian:bookworm-slim

ARG TOOLCHAIN_RELEASE=20231018
ARG LINARO_VERSION=7.5.0
ARG SDL_VERSION=2.26.1
ENV DEBIAN_FRONTEND=noninteractive

# Pinned toolchain: TrimUI SDK release 20231018, Linaro GCC 7.5.0, and SDL2 2.26.1.
RUN dpkg --add-architecture arm64 \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates wget make gcc g++ file pkg-config \
       binutils-aarch64-linux-gnu linux-libc-dev-arm64-cross \
       libc6-arm64-cross libc6-dev-arm64-cross \
       libsdl2-gfx-dev:arm64 libsdl2-image-dev:arm64 \
       libsdl2-mixer-dev:arm64 libsdl2-net-dev:arm64 \
       libsdl2-ttf-dev:arm64 libegl1-mesa-dev libgles2-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q "https://github.com/trimui/toolchain_sdk_smartpro/releases/download/${TOOLCHAIN_RELEASE}/aarch64-linux-gnu-${LINARO_VERSION}-linaro.tgz" \
    && tar -C /usr/local -xzf "aarch64-linux-gnu-${LINARO_VERSION}-linaro.tgz" \
    && rm "aarch64-linux-gnu-${LINARO_VERSION}-linaro.tgz"

ENV TOOLCHAIN=/usr/local/aarch64-linux-gnu-7.5.0-linaro
ENV SYSROOT=/usr/local/aarch64-linux-gnu-7.5.0-linaro/sysroot
ENV PATH="/usr/local/aarch64-linux-gnu-7.5.0-linaro/bin:${PATH}"

RUN wget -q "https://github.com/trimui/toolchain_sdk_smartpro/releases/download/${TOOLCHAIN_RELEASE}/SDK_usr_tg5040_a133p.tgz" \
    && mkdir -p "${SYSROOT}/usr" \
    && cp -a "${TOOLCHAIN}/aarch64-linux-gnu/libc/." "${SYSROOT}/" \
    && tar -xzf SDK_usr_tg5040_a133p.tgz -C /tmp \
    && cp -a /tmp/usr/. "${SYSROOT}/usr/" \
    && rm -rf SDK_usr_tg5040_a133p.tgz /tmp/usr

# Build target SDL2 against the TrimUI sysroot. Never use host SDL2 headers
# or link the development binaries against host libraries.
RUN wget -q "https://github.com/trimui/toolchain_sdk_smartpro/releases/download/${TOOLCHAIN_RELEASE}/SDL2-${SDL_VERSION}.GE8300.tgz" \
    && tar -xzf "SDL2-${SDL_VERSION}.GE8300.tgz" -C /tmp \
    && cd "/tmp/SDL2-${SDL_VERSION}" \
    && ./configure --host=aarch64-linux-gnu --prefix=/usr --libdir=/usr/lib \
       --disable-video-wayland --disable-video-kmsdrm \
       --disable-pulseaudio --disable-jack --disable-sndio \
       --with-sysroot="${SYSROOT}" \
    && make -j2 \
    && make install DESTDIR="${SYSROOT}" \
    && rm -rf "/tmp/SDL2-${SDL_VERSION}" "SDL2-${SDL_VERSION}.GE8300.tgz"

RUN aarch64-linux-gnu-gcc --version | head -n 1 \
    && test -f "${SYSROOT}/usr/include/SDL2/SDL.h" \
    && test -e "${SYSROOT}/usr/lib/libSDL2.so"

WORKDIR /workspace
