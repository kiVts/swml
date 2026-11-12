#!/bin/sh
# Builds the single-file Linux release launcher (swml) with everything linked statically:
# SDL3, Dear ImGui, libdeflate, a minimal HTTPS-only libcurl and OpenSSL. The result runs on
# any x86_64 distro (including SteamOS/Steam Deck) without installing dependencies; Wayland
# client libraries are dlopen'd at runtime when present, X11 is the fallback.
#
#   sh linux/build-release.sh [build-dir]
#
# Needs: cmake, ninja (or make), a C/C++ toolchain, OpenSSL static archives (libssl-dev on
# Debian), zlib static (zlib1g-dev), and the usual SDL build deps (libwayland-dev,
# libxkbcommon-dev, xorg dev headers) for full Wayland+X11 support.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-"$ROOT/build-release"}
CURL_VERSION=8.14.1
PREFIX="$BUILD/static-prefix"

mkdir -p "$BUILD"

# --- minimal static libcurl (HTTPS only; no krb5/ldap/ssh2/rtmp, which lack static archives) ---
if [ ! -f "$PREFIX/lib/libcurl.a" ]; then
    echo "== building static libcurl $CURL_VERSION =="
    curl -sL -o "$BUILD/curl.tar.xz" "https://curl.se/download/curl-$CURL_VERSION.tar.xz"
    tar -C "$BUILD" -xf "$BUILD/curl.tar.xz"
    cmake -S "$BUILD/curl-$CURL_VERSION" -B "$BUILD/curl-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
        -DBUILD_CURL_EXE=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
        -DCURL_USE_OPENSSL=ON -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
        -DCURL_USE_GSSAPI=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
        -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_TELNET=ON \
        -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_IMAP=ON \
        -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_MQTT=ON \
        -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_AWS=ON -DCURL_DISABLE_BASIC_AUTH=OFF \
        -DHTTP_ONLY=ON -DCURL_ZLIB=ON -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
        -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF -DUSE_QUICHE=OFF -DUSE_MSH3=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$PREFIX"
    cmake --build "$BUILD/curl-build" --target install
fi

# --- the launcher, statically linked against it ---
echo "== building swml (static) =="
# Prefer the minimal curl's pkg-config metadata over the system one.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -S "$ROOT" -B "$BUILD/swml" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSWML_BUILD_CORE=OFF -DSWML_STATIC_LINUX=ON \
    -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build "$BUILD/swml" --target swml

echo
echo "Release binary: $BUILD/swml/swml"
file "$BUILD/swml/swml" 2>/dev/null || true
ldd "$BUILD/swml/swml" 2>/dev/null || true
