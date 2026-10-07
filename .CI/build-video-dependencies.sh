#!/usr/bin/env bash
set -euo pipefail

workspace="${MOLTORINO_VIDEO_WORKSPACE:-${RUNNER_TEMP:-$PWD}/video-dependencies}"
prefix="${MOLTORINO_VIDEO_PREFIX:-$workspace/install}"
mkdir -p "$workspace" "$prefix/lib" "$prefix/include" "$prefix/support/media/licenses"
workspace="$(cd "$workspace" && pwd)"
prefix="$(cd "$prefix" && pwd)"

fetch() {
    local name="$1"
    local url="$2"
    local hash="$3"
    if [ ! -f "$workspace/$name.archive" ]; then
        curl --fail --location --retry 3 --connect-timeout 30 --max-time 600 "$url" -o "$workspace/$name.archive.part"
        mv "$workspace/$name.archive.part" "$workspace/$name.archive"
    fi
    printf '%s  %s\n' "$hash" "$workspace/$name.archive" | shasum -a 256 -c -
    if [ ! -d "$workspace/$name" ]; then
        mkdir "$workspace/$name"
        tar -xf "$workspace/$name.archive" --strip-components=1 -C "$workspace/$name"
    fi
}
fetch vpx https://github.com/webmproject/libvpx/archive/refs/tags/v1.16.0.tar.gz \
    7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c
fetch webm https://github.com/webmproject/libwebm/archive/refs/tags/libwebm-1.0.0.32.tar.gz \
    7fd5e085bda9f8031cf2ad2a1e52d9b7b29cba9c0b96ad2ce794ce89e4249eb8
fetch yuv https://github.com/lemenkov/libyuv/archive/4cd90347e78ff76755df2107009e900374aee9cd.tar.gz \
    d401d9610c17aa2d5bb6ba279af478dd70d29ed52484a01d51469d255792c50a
fetch dav1d https://downloads.videolan.org/videolan/dav1d/1.5.3/dav1d-1.5.3.tar.xz \
    732010aa5ef461fa93355ed2c6c5fedb48ddc4b74e697eaabe8907eaeb943011
fetch ffmpeg https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz \
    de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

architectures=("$(uname -m)")
make_command=make
if [ "$(uname -s)" = FreeBSD ]; then
    make_command=gmake
fi
if [ "$(uname -s)" = Darwin ]; then
    architectures=(arm64 x86_64)
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
fi
for architecture in "${architectures[@]}"; do
    output="$workspace/install-$architecture"
    mkdir -p "$output/lib" "$output/include" "$workspace/vpx-$architecture" "$workspace/ffmpeg-$architecture"
    cmake_options=(
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX="$output"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    )
    vpx_options=()
    ffmpeg_options=()
    meson_options=()
    if [ "$(uname -s)" = Darwin ]; then
        cmake_options+=(
            -DCMAKE_SYSTEM_NAME=Darwin
            -DCMAKE_SYSTEM_PROCESSOR="$architecture"
            -DCMAKE_OSX_ARCHITECTURES="$architecture"
            -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
        )
        export CC="clang -arch $architecture"
        export CXX="clang++ -arch $architecture"
        vpx_options+=(--target="$architecture-darwin20-gcc")
        ffmpeg_options+=(
            --enable-cross-compile
            --target-os=darwin
            --arch="$architecture"
            --cc=clang
            --extra-cflags="-arch $architecture -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
            --extra-ldflags="-arch $architecture -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
        )
        cpu="$architecture"
        [ "$architecture" != arm64 ] || cpu=aarch64
        cat > "$workspace/meson-$architecture.ini" <<EOF
[binaries]
c = ['clang', '-arch', '$architecture']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'darwin'
cpu_family = '$cpu'
cpu = '$cpu'
endian = 'little'
EOF
        meson_options+=(--cross-file "$workspace/meson-$architecture.ini")
    fi
    (
        cd "$workspace/vpx-$architecture"
        "$workspace/vpx/configure" --prefix="$output" --disable-shared --enable-pic \
            --disable-examples --disable-tools --disable-docs --disable-unit-tests \
            --disable-vp9-encoder --disable-vp9-decoder "${vpx_options[@]}"
        "$make_command" -j4
        "$make_command" install
    )
    cmake -S "$workspace/webm" -B "$workspace/webm-$architecture" "${cmake_options[@]}" \
        -DBUILD_SHARED_LIBS=OFF -DENABLE_WEBM_PARSER=ON -DENABLE_WEBMTS=OFF \
        -DENABLE_WEBMINFO=OFF -DENABLE_SAMPLE_PROGRAMS=OFF -DENABLE_TESTS=OFF
    cmake --build "$workspace/webm-$architecture" --target webm --parallel 4
    cmake --install "$workspace/webm-$architecture"
    cmake -S "$workspace/yuv" -B "$workspace/yuv-$architecture" "${cmake_options[@]}" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON \
        -DCMAKE_CXX_STANDARD=17 -DCMAKE_CXX_STANDARD_REQUIRED=ON
    cmake --build "$workspace/yuv-$architecture" --target yuv --parallel 4
    cp "$workspace/yuv-$architecture/libyuv.a" "$output/lib/"
    cp -R "$workspace/yuv/include/." "$output/include/"
    meson setup "$workspace/dav1d-$architecture" "$workspace/dav1d" --prefix="$output" --libdir=lib \
        --buildtype=release --default-library=static -Denable_tools=false -Denable_tests=false "${meson_options[@]}"
    meson compile -C "$workspace/dav1d-$architecture" -j4
    meson install -C "$workspace/dav1d-$architecture"
    (
        cd "$workspace/ffmpeg-$architecture"
        PKG_CONFIG_PATH="$output/lib/pkgconfig" "$workspace/ffmpeg/configure" \
            --prefix="$output" --disable-autodetect --disable-everything --disable-shared --enable-static \
            --disable-doc --disable-debug --disable-network --disable-avdevice --disable-swresample --disable-postproc \
            --enable-ffmpeg --enable-ffprobe --enable-libvpx --enable-libdav1d --enable-zlib \
            --enable-decoder=h264,hevc,vp8,vp9,libdav1d,mpeg4,mjpeg,png,gif,webp \
            --enable-encoder=libvpx_vp8 --enable-demuxer=mov,matroska,avi,gif,image2,webp_anim \
            --enable-muxer=webm --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mjpeg,png,gif,webp \
            --enable-protocol=file,pipe --enable-filter=scale,fps,setpts,setsar,format,transpose,hflip,vflip \
            --pkg-config-flags=--static "${ffmpeg_options[@]}"
        "$make_command" -j4 ffmpeg ffprobe
    )
done
for library in vpx webm yuv; do
    if [ "$(uname -s)" = Darwin ]; then
        lipo "$workspace/install-arm64/lib/lib$library.a" "$workspace/install-x86_64/lib/lib$library.a" \
            -create -output "$prefix/lib/lib$library.a"
    else
        cp "$output/lib/lib$library.a" "$prefix/lib/"
    fi
done
cp -R "$output/include/." "$prefix/include/"
for program in ffmpeg ffprobe; do
    if [ "$(uname -s)" = Darwin ]; then
        lipo "$workspace/ffmpeg-arm64/$program" "$workspace/ffmpeg-x86_64/$program" \
            -create -output "$prefix/support/media/$program"
    else
        cp "$workspace/ffmpeg-$architecture/$program" "$prefix/support/media/"
    fi
done
for package in vpx webm yuv dav1d ffmpeg; do
    mkdir -p "$prefix/support/media/licenses/$package"
    for license in "$workspace/$package"/LICENSE* "$workspace/$package"/COPYING* "$workspace/$package"/PATENTS*; do
        [ ! -f "$license" ] || cp "$license" "$prefix/support/media/licenses/$package/"
    done
done
cp "$(dirname "$0")/../resources/licenses/ffmpeg-source.txt" "$prefix/support/media/licenses/ffmpeg/"
if [ -n "${GITHUB_ENV:-}" ]; then
    printf 'MOLTORINO_VIDEO_PREFIX=%s\n' "$prefix" >> "$GITHUB_ENV"
fi
echo "Video dependencies: $prefix"
