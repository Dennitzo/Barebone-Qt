#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

BUILD_TYPE="${BUILD_TYPE:-Release}"
QT_ROOT="${QT_ROOT:-$HOME/Qt}"
QT_KIT_PATH="${QT_KIT_PATH:-}"
CMAKE_PATH="${CMAKE_PATH:-}"
NINJA_PATH="${NINJA_PATH:-}"
BUILD_DIR="${BUILD_DIR:-}"
INSTALL_DIR="${INSTALL_DIR:-}"
JOBS="${JOBS:-}"
CLEAN=0

usage() {
  cat <<'USAGE'
Usage: macos/build-app.sh [options]

Options:
  -c, --configuration TYPE  CMake build type. Default: Release
  --qt-root PATH           Qt installation root. Default: $HOME/Qt
  --qt-kit-path PATH       Exact Qt macOS kit path, for example $HOME/Qt/6.11.1/macos
  --cmake-path PATH        CMake executable path
  --ninja-path PATH        Ninja executable path
  --build-dir PATH         Build directory
  --install-dir PATH       Install directory
  -j, --jobs N             Parallel build job count
  --clean                  Remove the build directory before configuring
  -h, --help               Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--configuration) BUILD_TYPE="$2"; shift 2 ;;
    --qt-root) QT_ROOT="$2"; shift 2 ;;
    --qt-kit-path) QT_KIT_PATH="$2"; shift 2 ;;
    --cmake-path) CMAKE_PATH="$2"; shift 2 ;;
    --ninja-path) NINJA_PATH="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    -j|--jobs) JOBS="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

ROOT_DIR="$(project_root)"
CMAKE_BIN="$(resolve_cmake "$CMAKE_PATH")"
QT_KIT="$(resolve_qt_kit "$QT_ROOT" "$QT_KIT_PATH")"
ensure_qt_components "$QT_KIT" Network Positioning SerialPort WebChannel WebEngineWidgets Widgets
prepend_path_entry "$QT_KIT/bin"

QT_VERSION="$(qt_version_from_kit "$QT_KIT")"
ARCH="$(uname -m)"
[[ -n "$BUILD_DIR" ]] || BUILD_DIR="$ROOT_DIR/build/macos-$QT_VERSION-$ARCH"
[[ -n "$INSTALL_DIR" ]] || INSTALL_DIR="$BUILD_DIR/install"

if [[ "$CLEAN" -eq 1 ]]; then
  remove_safe_directory "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR" "$INSTALL_DIR"

cmake_args=(
  -S "$ROOT_DIR"
  -B "$BUILD_DIR"
  "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
  "-DCMAKE_PREFIX_PATH=$QT_KIT"
  "-DCMAKE_INSTALL_PREFIX=$INSTALL_DIR"
)

NINJA_BIN="$(resolve_ninja "$NINJA_PATH" || true)"
if [[ -n "$NINJA_BIN" ]]; then
  prepend_path_entry "$(dirname "$NINJA_BIN")"
  cmake_args+=(
    -G Ninja
    "-DCMAKE_MAKE_PROGRAM=$NINJA_BIN"
  )
fi

log_step "Configuring Barebone-Qt with Qt $QT_VERSION for macOS"
"$CMAKE_BIN" "${cmake_args[@]}"

build_args=(--build "$BUILD_DIR" --config "$BUILD_TYPE")
if [[ -n "$JOBS" ]]; then
  build_args+=(--parallel "$JOBS")
else
  build_args+=(--parallel)
fi

log_step "Building Barebone-Qt ($BUILD_TYPE)"
"$CMAKE_BIN" "${build_args[@]}"

log_step "Installing Barebone-Qt app bundle"
"$CMAKE_BIN" --install "$BUILD_DIR" --config "$BUILD_TYPE"

APP_BUNDLE="$(find_app_bundle "$BUILD_DIR" "$INSTALL_DIR" Barebone-Qt)"
[[ -n "$APP_BUNDLE" && -d "$APP_BUNDLE" ]] || die "Barebone-Qt.app was not found below $BUILD_DIR or $INSTALL_DIR"
printf 'macOS app bundle: %s\n' "$APP_BUNDLE"
