#!/usr/bin/env bash
# dav1d's arm asm against its C functions, without arm hardware: check.c built with the engine's
# dav1d CMake (engine/libs/dav1d) by clang 18 for aarch64 and armv7 (Linux) and run under QEMU
# user mode with every CPU feature QEMU emulates (-cpu max: NEON, and on aarch64 dotprod, i8mm,
# SVE, SVE2), plus a native x86-64 build. Every frame of every clip must be identical with the
# asm on and off. Timings under QEMU say nothing about real hardware.
#
#   tools/htmlui/dav1d-asm-check/run.sh clip.webm [clip.webm ...]     (from the repository root;
#   needs Docker and FFmpeg; output in build/dav1d-asm-check)
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
OUT=build/dav1d-asm-check
mkdir -p "$ROOT/$OUT/clips"
CLIPS=()
for clip in "$@"; do
    ivf="$OUT/clips/$(basename "${clip%.*}").ivf"
    ffmpeg -v error -nostdin -y -i "$clip" -map 0:v:0 -c copy -f ivf "$ROOT/$ivf"
    CLIPS+=("/src/$ivf")
done
docker run --rm ${CPUS:+--cpus "$CPUS"} -v "$ROOT:/src" -w /src ubuntu:24.04 bash -euo pipefail -c "
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends clang lld cmake ninja-build nasm gcc libc6-dev qemu-user \
        gcc-aarch64-linux-gnu libc6-dev-arm64-cross gcc-arm-linux-gnueabihf libc6-dev-armhf-cross > /dev/null
    clang --version | head -1
    build() { # name, extra cmake arguments...
        name=\$1; shift
        cmake -S tools/htmlui/dav1d-asm-check -B $OUT/\$name -G Ninja -DCMAKE_BUILD_TYPE=Release \"\$@\" > $OUT/\$name.configure.log
        grep 'dav1d: asm' $OUT/\$name.configure.log
        cmake --build $OUT/\$name > $OUT/\$name.build.log
    }
    build x86_64 -DCMAKE_C_COMPILER=gcc
    $OUT/x86_64/dav1d-asm-check ${CLIPS[*]}
    for arch in aarch64 armv7; do
        if [ \$arch = aarch64 ]; then triple=aarch64-linux-gnu; qemu=qemu-aarch64; flags=''; else
            triple=arm-linux-gnueabihf; qemu=qemu-arm; flags='-march=armv7-a -mfpu=neon -mfloat-abi=hard'; fi
        build \$arch -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=\$arch -DCMAKE_C_COMPILER=clang \
            -DCMAKE_C_COMPILER_TARGET=\$triple -DCMAKE_ASM_COMPILER=clang -DCMAKE_ASM_COMPILER_TARGET=\$triple \
            -DCMAKE_C_FLAGS=\"\$flags\" -DCMAKE_ASM_FLAGS=\"\$flags\" -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
        \$qemu -L /usr/\$triple -cpu max $OUT/\$arch/dav1d-asm-check ${CLIPS[*]}
    done
"
