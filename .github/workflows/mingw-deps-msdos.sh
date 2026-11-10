#!/usr/bin/env bash
# The extra static libraries 86Box (the MS-DOS desktop's core) needs for Windows, on top of
# mingw-deps.sh: zstd, libpng, freetype and libsndfile. Run after mingw-deps.sh with the same
# <prefix>. Also writes <prefix>/shim/Windows.h: 86Box includes <Windows.h>, which MinGW names
# <windows.h>, and a Linux file system tells them apart.
set -euo pipefail

PREFIX=${1:?usage: mingw-deps-msdos.sh <prefix>}
PREFIX=$(cd "$PREFIX" && pwd)
WORK=$PREFIX/src
JOBS=$(nproc)

ZSTD_SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
LIBPNG_SHA256=60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e
FREETYPE_SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
LIBSNDFILE_SHA256=3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e

mkdir -p "$WORK" "$PREFIX/shim"
cd "$WORK"
printf '#include <windows.h>\n' > "$PREFIX/shim/Windows.h"

fetch() { # <url> <sha256> <file>
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c -
}

CMAKE_DEP=(-G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$PREFIX/toolchain.cmake"
           "-DCMAKE_INSTALL_PREFIX=$PREFIX" "-DCMAKE_PREFIX_PATH=$PREFIX" -DBUILD_SHARED_LIBS=OFF)

fetch https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz "$ZSTD_SHA256" zstd.tar.gz
tar -xzf zstd.tar.gz
cmake -S zstd-1.5.6/build/cmake -B zstd-build "${CMAKE_DEP[@]}" -DZSTD_BUILD_PROGRAMS=OFF \
    -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON
cmake --build zstd-build -j"$JOBS"
cmake --install zstd-build

fetch https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz "$LIBPNG_SHA256" libpng.tar.xz
tar -xJf libpng.tar.xz
cmake -S libpng-1.6.44 -B libpng-build "${CMAKE_DEP[@]}" -DPNG_SHARED=OFF -DPNG_STATIC=ON \
    -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_EXECUTABLES=OFF
cmake --build libpng-build -j"$JOBS"
cmake --install libpng-build

fetch https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz "$FREETYPE_SHA256" freetype.tar.xz
tar -xJf freetype.tar.xz
cmake -S freetype-2.13.3 -B freetype-build "${CMAKE_DEP[@]}" -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_PNG=ON -DFT_REQUIRE_ZLIB=ON
cmake --build freetype-build -j"$JOBS"
cmake --install freetype-build

fetch https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz \
    "$LIBSNDFILE_SHA256" libsndfile.tar.xz
tar -xJf libsndfile.tar.xz
cmake -S libsndfile-1.2.2 -B libsndfile-build "${CMAKE_DEP[@]}" -DENABLE_EXTERNAL_LIBS=OFF \
    -DENABLE_MPEG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build libsndfile-build -j"$JOBS"
cmake --install libsndfile-build

echo "86Box's libraries installed in $PREFIX"
