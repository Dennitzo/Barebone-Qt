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
DIST_DIR="${DIST_DIR:-}"
ARTIFACT_NAME="${ARTIFACT_NAME:-}"
JOBS="${JOBS:-}"
CLEAN=0

usage() {
  cat <<'USAGE'
Usage: macos/package-artifact.sh [options]

Builds the macOS app bundle, deploys Qt runtime files with macdeployqt,
and creates a zip artifact.

Options:
  -c, --configuration TYPE  CMake build type. Default: Release
  --qt-root PATH           Qt installation root. Default: $HOME/Qt
  --qt-kit-path PATH       Exact Qt macOS kit path, for example $HOME/Qt/6.11.1/macos
  --cmake-path PATH        CMake executable path
  --ninja-path PATH        Ninja executable path
  --build-dir PATH         Build directory
  --install-dir PATH       Install directory
  --dist-dir PATH          Staging directory. Default: build/dist
  --artifact-name NAME     Artifact basename without .zip
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
    --dist-dir) DIST_DIR="$2"; shift 2 ;;
    --artifact-name) ARTIFACT_NAME="$2"; shift 2 ;;
    -j|--jobs) JOBS="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

ROOT_DIR="$(project_root)"
QT_KIT="$(resolve_qt_kit "$QT_ROOT" "$QT_KIT_PATH")"
QT_VERSION="$(qt_version_from_kit "$QT_KIT")"
ARCH="$(uname -m)"
[[ -n "$BUILD_DIR" ]] || BUILD_DIR="$ROOT_DIR/build/macos-$QT_VERSION-$ARCH"
[[ -n "$INSTALL_DIR" ]] || INSTALL_DIR="$BUILD_DIR/install"
[[ -n "$DIST_DIR" ]] || DIST_DIR="$ROOT_DIR/build/dist"
[[ -n "$ARTIFACT_NAME" ]] || ARTIFACT_NAME="Barebone-Qt-macOS-$ARCH-$BUILD_TYPE"

build_args=(-c "$BUILD_TYPE" --qt-root "$QT_ROOT" --build-dir "$BUILD_DIR" --install-dir "$INSTALL_DIR")
[[ -n "$QT_KIT_PATH" ]] && build_args+=(--qt-kit-path "$QT_KIT_PATH")
[[ -n "$CMAKE_PATH" ]] && build_args+=(--cmake-path "$CMAKE_PATH")
[[ -n "$NINJA_PATH" ]] && build_args+=(--ninja-path "$NINJA_PATH")
[[ -n "$JOBS" ]] && build_args+=(-j "$JOBS")
[[ "$CLEAN" -eq 1 ]] && build_args+=(--clean)

"$SCRIPT_DIR/build-app.sh" "${build_args[@]}"

QT_KIT="$(resolve_qt_kit "$QT_ROOT" "$QT_KIT_PATH")"
prepend_path_entry "$QT_KIT/bin"
MACDEPLOYQT_BIN="$(resolve_macdeployqt "$QT_KIT")"

APP_BUNDLE="$(find_app_bundle "$BUILD_DIR" "$INSTALL_DIR" Barebone-Qt)"
[[ -n "$APP_BUNDLE" && -d "$APP_BUNDLE" ]] || die "Barebone-Qt.app was not found below $BUILD_DIR or $INSTALL_DIR"

STAGE_DIR="$DIST_DIR/$ARTIFACT_NAME"
STAGE_APP="$STAGE_DIR/Barebone-Qt.app"
ARTIFACT_DIR="$ROOT_DIR/artifacts"
ARTIFACT_PATH="$ARTIFACT_DIR/$ARTIFACT_NAME.zip"

log_step "Staging Barebone-Qt.app"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR" "$ARTIFACT_DIR"
cp -R "$APP_BUNDLE" "$STAGE_DIR/"

log_step "Deploying Qt runtime with macdeployqt"
"$MACDEPLOYQT_BIN" "$STAGE_APP" -verbose=1 -always-overwrite

log_step "Packaging Barebone-Qt"
rm -f "$ARTIFACT_PATH"
(
  cd "$STAGE_DIR"
  /usr/bin/ditto -c -k --sequesterRsrc --keepParent "Barebone-Qt.app" "$ARTIFACT_PATH"
)

printf 'Artifact: %s\n' "$ARTIFACT_PATH"
