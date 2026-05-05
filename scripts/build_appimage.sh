#!/usr/bin/env bash
# Build waywallen end-to-end and produce a single-file AppImage at:
#     <repo>/waywallen-x86_64.AppImage
#
# Audience: users unfamiliar with cmake / cargo / linuxdeploy.
# Prerequisites:
#   1. conda (Miniconda recommended: https://docs.conda.io/projects/miniconda/)
#   2. rustup (https://rustup.rs/) — restart the shell after install
# Usage (works from anywhere inside the repo):
#   ./scripts/build_appimage.sh   first run takes ~15–30 min (creates conda env, builds qtgrpc, packs AppImage)
#   ./scripts/build_appimage.sh   re-running performs an incremental rebuild + repack
#
# Optional environment variables:
#   WAYWALLEN_CONDA_ENV     conda env name, default "waywallen"

set -euo pipefail

# Script lives in <repo>/scripts/, so PROJECT_DIR is one level up.
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_NAME="${WAYWALLEN_CONDA_ENV:-waywallen}"
BUILD_DIR="$PROJECT_DIR/build/clang-release"
APPDIR="$PROJECT_DIR/build/AppDir"
INSTALL_DIR="$APPDIR/usr"          # AppDir's /usr is the cmake install prefix
TOOLS_DIR="$PROJECT_DIR/build/_tools"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- Compute the version string baked into the AppImage filename ----
# Pull the canonical version from Cargo.toml; refine with git metadata so
# successive dev builds at the same version don't all overwrite each other.
# Override the entire tag with WAYWALLEN_BUILD_VERSION=foo for one-off names.
WAYWALLEN_VERSION="$(awk -F'"' '/^version *= *"/ { print $2; exit }' "$PROJECT_DIR/Cargo.toml")"
[[ -n "$WAYWALLEN_VERSION" ]] || fail "could not parse version from Cargo.toml"

if [[ -n "${WAYWALLEN_BUILD_VERSION:-}" ]]; then
    BUILD_TAG="$WAYWALLEN_BUILD_VERSION"
elif git -C "$PROJECT_DIR" rev-parse --short=7 HEAD >/dev/null 2>&1; then
    SHA="$(git -C "$PROJECT_DIR" rev-parse --short=7 HEAD)"
    DIRTY=""
    git -C "$PROJECT_DIR" diff --quiet --ignore-submodules HEAD 2>/dev/null || DIRTY="-dirty"
    if [[ -z "$DIRTY" ]] \
        && git -C "$PROJECT_DIR" describe --tags --exact-match --match "v$WAYWALLEN_VERSION" \
                >/dev/null 2>&1; then
        BUILD_TAG="$WAYWALLEN_VERSION"
    else
        BUILD_TAG="$WAYWALLEN_VERSION-g$SHA$DIRTY"
    fi
else
    BUILD_TAG="$WAYWALLEN_VERSION"
fi

APPIMAGE_OUT="$PROJECT_DIR/waywallen-$BUILD_TAG-x86_64.AppImage"
step "Building AppImage tagged as $BUILD_TAG"

# ---- 1. Check required tools ----
command -v conda >/dev/null \
    || fail "conda not found. Install Miniconda first: https://docs.conda.io/projects/miniconda/"
command -v cargo >/dev/null \
    || fail "cargo not found. Install rustup first: https://rustup.rs/  Then restart your shell and re-run."

# ---- 2. Set up the conda environment ----
# Make `conda activate` available inside this script.
# Note: conda's profile script is not friendly to `set -u`; disable it briefly.
set +u
# shellcheck disable=SC1091
source "$(conda info --base)/etc/profile.d/conda.sh"
set -u

ENV_FILE="$PROJECT_DIR/environment.yml"
[[ -f "$ENV_FILE" ]] || fail "missing $ENV_FILE"

if conda env list | awk 'NF && $1 !~ /^#/ {print $1}' | grep -qx "$ENV_NAME"; then
    step "Updating conda env: $ENV_NAME (sync to environment.yml)"
    conda env update -n "$ENV_NAME" -f "$ENV_FILE" --prune
else
    step "Creating conda env: $ENV_NAME (install per environment.yml)"
    conda env create -n "$ENV_NAME" -f "$ENV_FILE"
fi

step "Activating env: $ENV_NAME"
set +u
conda activate "$ENV_NAME"
set -u

# ---- 2.4 Build a minimal FFmpeg into the conda env (replaces conda-forge's ffmpeg) ----
bash "$PROJECT_DIR/scripts/build_ffmpeg.sh"

# ---- 2.5 Build the Qt6Protobuf module from source (conda-forge has no qtgrpc package) ----
QT_VER="$("$CONDA_PREFIX/bin/qmake6" -query QT_VERSION)"
if [[ ! -f "$CONDA_PREFIX/lib/cmake/Qt6Protobuf/Qt6ProtobufConfig.cmake" ]]; then
    step "Building qtgrpc v$QT_VER from source (one-shot; installs into $CONDA_PREFIX)"
    QTGRPC_SRC="$PROJECT_DIR/build/_qtgrpc-src"
    QTGRPC_BUILD="$PROJECT_DIR/build/_qtgrpc-build"
    rm -rf "$QTGRPC_SRC" "$QTGRPC_BUILD"
    git clone --depth 1 --branch "v$QT_VER" \
        https://code.qt.io/qt/qtgrpc.git "$QTGRPC_SRC"
    cmake -S "$QTGRPC_SRC" -B "$QTGRPC_BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_SYSROOT="$CONDA_BUILD_SYSROOT" \
        -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
        -DCMAKE_INSTALL_PREFIX="$CONDA_PREFIX" \
        -DQT_FEATURE_grpc=OFF \
        -DBUILD_TESTING=OFF \
        -DQT_BUILD_EXAMPLES=OFF \
        -DQT_BUILD_TESTS=OFF
    cmake --build   "$QTGRPC_BUILD" --parallel
    cmake --install "$QTGRPC_BUILD"
fi

# ---- 3. CMake configure ----
step "CMake configure (daemon + UI + image/video renderer plugins)"
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_SYSROOT="$CONDA_BUILD_SYSROOT" \
    `# Under sysroot 2.28 pthread lives in libpthread, not libc — pthread must
     # be enabled globally, otherwise C++20 PCMs produced by rstd / qextra etc.
     # disagree on pthread state and clang reports module-file-config-mismatch
     # when one imports the other.` \
    -DCMAKE_C_FLAGS_INIT="-pthread" \
    -DCMAKE_CXX_FLAGS_INIT="-pthread" \
    -DCMAKE_LINKER=lld \
    -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DWAYWALLEN_BUILD_DAEMON=ON \
    -DWAYWALLEN_BUILD_UI=ON \
    -DWAYWALLEN_BUILD_PLUGINS=ON \
    -DWAYWALLEN_BUILD_IMAGE_PLUGIN=ON \
    -DWAYWALLEN_BUILD_VIDEO_PLUGIN=ON

# ---- 4. Build + install ----
step "Compiling (first build ~10–20 min; subsequent runs are incremental and fast)"
cmake --build "$BUILD_DIR" --parallel

step "Installing into AppDir: $APPDIR"
rm -rf "$APPDIR"
cmake --install "$BUILD_DIR"

# ---- 4.5 Build open-wallpaper-engine (waywallen-wescene-renderer) ----
# Pinned commit; bump explicitly when integrating new owe changes.
OWE_COMMIT="bc4cbea4b61a1956c4b1ce4bf1420b9e1df49380"
OWE_SRC="$PROJECT_DIR/build/_owe-src"
OWE_BUILD="$PROJECT_DIR/build/_owe-build"

step "Fetching open-wallpaper-engine @ $OWE_COMMIT"
if [[ ! -d "$OWE_SRC/.git" ]]; then
    git clone https://github.com/waywallen/open-wallpaper-engine.git "$OWE_SRC"
fi
# Try a single-SHA fetch first (cheap if the server supports it), fall back
# to a full fetch otherwise; then hard-reset to the pinned commit so the
# tree matches even if a previous run left it on something else.
git -C "$OWE_SRC" fetch --quiet origin "$OWE_COMMIT" 2>/dev/null \
    || git -C "$OWE_SRC" fetch --quiet origin
git -C "$OWE_SRC" -c advice.detachedHead=false reset --hard "$OWE_COMMIT"

step "CMake configure: open-wallpaper-engine"
# CMAKE_PREFIX_PATH puts $INSTALL_DIR ahead of $CONDA_PREFIX so owe's
# `find_package(waywallen-bridge REQUIRED)` resolves to the bridge we just
# installed in step 4.
cmake -S "$OWE_SRC" -B "$OWE_BUILD" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_SYSROOT="$CONDA_BUILD_SYSROOT" \
    -DCMAKE_C_FLAGS_INIT="-pthread" \
    -DCMAKE_CXX_FLAGS_INIT="-pthread" \
    -DCMAKE_LINKER=lld \
    -DCMAKE_PREFIX_PATH="$INSTALL_DIR;$CONDA_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DBUILD_WAYWALLEN_HOST=ON \
    -DBUILD_QML=OFF

step "Compiling open-wallpaper-engine"
cmake --build   "$OWE_BUILD" --parallel
cmake --install "$OWE_BUILD"

# # ---- 5. Fetch linuxdeploy / appimagetool (cached on first run under build/_tools) ----
mkdir -p "$TOOLS_DIR"
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT="$TOOLS_DIR/linuxdeploy_plugin_qt"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
download_if_missing() {
    local url="$1" dest="$2"
    if [[ ! -x "$dest" ]]; then
        step "Downloading $(basename "$dest")"
        curl -fsSL --retry 3 -o "$dest" "$url"
        chmod +x "$dest"
    fi
}
download_if_missing \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
    "$LINUXDEPLOY"
download_if_missing \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
    "$LINUXDEPLOY_QT"
download_if_missing \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" \
    "$APPIMAGETOOL"

# ---- 6. Custom AppRun (launches the daemon and points it at the bundled UI / display backend) ----
APPRUN_TMP="$(mktemp -t waywallen-AppRun.XXXXXX)"
trap 'rm -f "$APPRUN_TMP"' EXIT
cat > "$APPRUN_TMP" <<'APPEOF'
#!/usr/bin/env bash
# AppImage entry point: launch the daemon, which spawns the bundled UI and
# display backend.
# Layout follows the qt.conf generated by linuxdeploy-plugin-qt:
#   usr/lib/      -> Qt shared libs + our libqml_material.so
#   usr/plugins/  -> Qt platform plugins / wayland-* / imageformats / etc.
#   usr/qml/      -> all QML modules (Qt's own + Qcm/Material + waywallen/ui)
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$HERE/usr/plugins:${QT_PLUGIN_PATH:-}"
export QML2_IMPORT_PATH="$HERE/usr/qml:${QML2_IMPORT_PATH:-}"
export QML_IMPORT_PATH="$QML2_IMPORT_PATH"
exec "$HERE/usr/bin/waywallen" \
    --ui "$HERE/usr/bin/waywallen-ui" \
    --plugin "$HERE/usr/share/waywallen" \
    "$@"
APPEOF
chmod +x "$APPRUN_TMP"

# ---- 7. linuxdeploy stages dependencies into AppDir (no packaging yet, so we can prune in between) ----
step "linuxdeploy: staging dependencies into AppDir"
DESKTOP_FILE="$INSTALL_DIR/share/applications/org.waywallen.waywallen.desktop"
ICON_FILE="$INSTALL_DIR/share/icons/hicolor/scalable/apps/org.waywallen.waywallen.svg"
[[ -f "$DESKTOP_FILE" ]] || fail "missing .desktop file: $DESKTOP_FILE"
[[ -f "$ICON_FILE"   ]] || fail "missing icon: $ICON_FILE"

pushd $TOOLS_DIR
$LINUXDEPLOY_QT --appimage-extract
$LINUXDEPLOY --appimage-extract
LINUXDEPLOY=$TOOLS_DIR/squashfs-root/AppRun
popd

cd "$PROJECT_DIR/build"
PATH="$TOOLS_DIR:$PATH" \
LD_LIBRARY_PATH="$INSTALL_DIR/lib:$CONDA_PREFIX/lib" \
QMAKE="$CONDA_PREFIX/bin/qmake6" \
EXTRA_PLATFORM_PLUGINS="libqwayland.so" \
EXTRA_QT_PLUGINS="wayland-decoration-client;wayland-shell-integration" \
"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --plugin qt \
    --executable "$INSTALL_DIR/bin/waywallen" \
    --executable "$INSTALL_DIR/bin/waywallen-ui" \
    --executable "$INSTALL_DIR/bin/waywallen-display-layer-shell" \
    --executable "$INSTALL_DIR/bin/waywallen-image-renderer" \
    --executable "$INSTALL_DIR/bin/waywallen-video-renderer" \
    --executable "$INSTALL_DIR/bin/waywallen-wescene-renderer" \
    --desktop-file "$DESKTOP_FILE" \
    --icon-file "$ICON_FILE" \
    --custom-apprun "$APPRUN_TMP"

cp -rv "$CONDA_PREFIX/lib/qt6/plugins/wayland-graphics-integration-client" "$APPDIR/usr/plugins/"
cp -v "$CONDA_PREFIX/lib/libstdc++.so.6" "$APPDIR/usr/lib/"
cp -v "$CONDA_PREFIX/lib/libgcc_s.so.1" "$APPDIR/usr/lib/"
cp -rv "$APPDIR/usr/lib/qt6/qml/." "$APPDIR/usr/qml/"
rm -rf "$APPDIR/usr/lib/qt6"

# ---- 8. Drop unused QuickControls2 styles (native libs + QML modules) ----
step "Pruning unused QuickControls2 styles"
# Each name targets BOTH:
#   usr/lib/libQt6QuickControls2<Style>*.so*    (style + StyleImpl shared libs)
#   usr/qml/QtQuick/Controls/<Style>/           (QML module dir for the style)
QUICKCONTROLS2_PRUNE=(Basic Fusion FluentWinUI3 Imagine Material Universal designer)
for style in "${QUICKCONTROLS2_PRUNE[@]}"; do
    for libdir in "$APPDIR/usr/lib" "$APPDIR/usr/lib64"; do
        [[ -d "$libdir" ]] || continue
        find "$libdir" -maxdepth 1 -type f \
            -name "libQt6QuickControls2${style}*.so*" -print -delete 2>/dev/null || true
    done
    rm -rfv "$APPDIR/usr/qml/QtQuick/Controls/${style}" 2>/dev/null || true
done

# ---- 9. Pack the AppImage ----
step "Packing AppImage"
rm -f "$APPIMAGE_OUT"
PATH="$TOOLS_DIR:$PATH" \
ARCH=x86_64 \
"$APPIMAGETOOL" --appimage-extract-and-run \
    --no-appstream \
    "$APPDIR" "$APPIMAGE_OUT"
[[ -f "$APPIMAGE_OUT" ]] || fail "AppImage build failed"

cat <<EOF

Build complete: $APPIMAGE_OUT

Run it:
    chmod +x "$APPIMAGE_OUT"   # if not already executable
    "$APPIMAGE_OUT"

Rebuild: re-run ./scripts/build_appimage.sh (incremental rebuild + repack).
EOF
