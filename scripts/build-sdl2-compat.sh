#!/bin/bash

set -xe
	  
cd $(dirname "${BASH_SOURCE[0]}")/..
cd "./$1"
ROOT="`pwd`"
MSYSTEM_LOWER="$(printf '%s' "${MSYSTEM:-}" | tr '[:upper:]' '[:lower:]')"

SDL_VER=3.4.0
SDL_COMPAT_VER=2.32.64
URL=https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VER}/SDL3-${SDL_VER}.tar.gz
FILE=SDL3-${SDL_VER}.tar.gz
DIR=SDL3-${SDL_VER}

if [ ! -d "$DIR" ]; then
	curl -L "$URL" -O
	tar -xf "$FILE"
fi

cd "$DIR" || exit 1
INSTALL_PREFIX="${INSTALL_PREFIX:=/usr}"
WINDOWS_ARM_CMAKE_ARGS=()
WINDOWS_ARM_SHARED_LINKER_FLAGS=()
if [[ "$MSYSTEM_LOWER" == "clangarm64" ]]; then
    WINDOWS_ARM_CMAKE_ARGS=(
        -DCMAKE_C_COMPILER=clang
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=ARM64
    )
    CLANG_RT_BUILTINS_DIR=""
    if command -v clang >/dev/null 2>&1; then
        CLANG_RT_BUILTINS_LIB=$(clang --print-file-name=libclang_rt.builtins-aarch64.a)
        if [[ -n "$CLANG_RT_BUILTINS_LIB" && -f "$CLANG_RT_BUILTINS_LIB" ]]; then
            CLANG_RT_BUILTINS_DIR=$(dirname "$CLANG_RT_BUILTINS_LIB")
        fi
    fi
    WINDOWS_ARM_SHARED_LINKER_FLAGS=(
        -DCMAKE_SHARED_LINKER_FLAGS="-L${CLANG_RT_BUILTINS_DIR:-/clangarm64/lib} -lclang_rt.builtins-aarch64"
    )
fi
cmake \
    -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF \
    "${WINDOWS_ARM_CMAKE_ARGS[@]}" \
    "${WINDOWS_ARM_SHARED_LINKER_FLAGS[@]}"

cmake --build build --config RelWithDebInfo --clean-first
cmake --install build --prefix "$INSTALL_PREFIX"
cd ..
URL=https://github.com/libsdl-org/sdl2-compat/releases/download/release-${SDL_COMPAT_VER}/sdl2-compat-${SDL_COMPAT_VER}.tar.gz
FILE=sdl2-compat-${SDL_COMPAT_VER}.tar.gz
DIR2=sdl2-compat-${SDL_COMPAT_VER}

if [ ! -d "$DIR2" ]; then
	curl -L "$URL" -O
	tar -xf "$FILE"
fi

cd "$DIR2" || exit 1
# sdl2-compat dlopen()s libSDL3.so.0 and the library's own RUNPATH is searched
# first. A build RUNPATH pointing at the build machine's SDL3 directory
# (/usr/lib/x86_64-linux-gnu) made the AppImage load the host's SDL3 instead of
# the bundled one, and sdl2-compat aborts at startup when that one is too old.
# Without it, packaging (linuxdeploy) sets $ORIGIN and the bundled SDL3 is used.
INSTALL_PREFIX="${INSTALL_PREFIX:=/usr}"
WINDOWS_ARM_CMAKE_ARGS=()
WINDOWS_ARM_SHARED_LINKER_FLAGS=()
if [[ "$MSYSTEM_LOWER" == "clangarm64" ]]; then
    WINDOWS_ARM_CMAKE_ARGS=(
        -DCMAKE_C_COMPILER=clang
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_SYSTEM_PROCESSOR=ARM64
    )
    CLANG_RT_BUILTINS_DIR=""
    if command -v clang >/dev/null 2>&1; then
        CLANG_RT_BUILTINS_LIB=$(clang --print-file-name=libclang_rt.builtins-aarch64.a)
        if [[ -n "$CLANG_RT_BUILTINS_LIB" && -f "$CLANG_RT_BUILTINS_LIB" ]]; then
            CLANG_RT_BUILTINS_DIR=$(dirname "$CLANG_RT_BUILTINS_LIB")
        fi
    fi
    WINDOWS_ARM_SHARED_LINKER_FLAGS=(
        -DCMAKE_SHARED_LINKER_FLAGS="-L${CLANG_RT_BUILTINS_DIR:-/clangarm64/lib} -lclang_rt.builtins-aarch64"
    )
fi
cmake \
    -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX" \
    -DSDL2COMPAT_TESTS=OFF \
    -DCMAKE_SKIP_BUILD_RPATH=ON \
    -DCMAKE_SKIP_INSTALL_RPATH=ON \
    "${WINDOWS_ARM_CMAKE_ARGS[@]}" \
    "${WINDOWS_ARM_SHARED_LINKER_FLAGS[@]}"

cmake --build build --config RelWithDebInfo --clean-first
cmake --install build --prefix "$INSTALL_PREFIX"
