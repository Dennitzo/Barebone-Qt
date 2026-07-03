#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

QT_ROOT="${QT_ROOT:-$HOME/Qt}"
QT_KIT_PATH="${QT_KIT_PATH:-}"
CMAKE_PATH="${CMAKE_PATH:-}"
NINJA_PATH="${NINJA_PATH:-}"

usage() {
  cat <<'USAGE'
Usage: macos/check-environment.sh [options]

Options:
  --qt-root PATH       Qt installation root. Default: $HOME/Qt
  --qt-kit-path PATH   Exact Qt macOS kit path, for example $HOME/Qt/6.11.1/macos
  --cmake-path PATH    CMake executable path
  --ninja-path PATH    Ninja executable path
  -h, --help           Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --qt-root) QT_ROOT="$2"; shift 2 ;;
    --qt-kit-path) QT_KIT_PATH="$2"; shift 2 ;;
    --cmake-path) CMAKE_PATH="$2"; shift 2 ;;
    --ninja-path) NINJA_PATH="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

ROOT_DIR="$(project_root)"
CMAKE_BIN="$(resolve_cmake "$CMAKE_PATH")"
QT_KIT="$(resolve_qt_kit "$QT_ROOT" "$QT_KIT_PATH")"
ensure_qt_components "$QT_KIT" Network Positioning SerialPort WebChannel WebEngineWidgets Widgets
prepend_path_entry "$QT_KIT/bin"
NINJA_BIN="$(resolve_ninja "$NINJA_PATH" || true)"
MACDEPLOYQT_BIN="$(resolve_macdeployqt "$QT_KIT")"

log_step "Barebone-Qt macOS build environment"
printf 'Project: %s\n' "$ROOT_DIR"
printf 'macOS: %s\n' "$(sw_vers -productVersion)"
printf 'Architecture: %s\n' "$(uname -m)"
printf 'CMake: %s\n' "$CMAKE_BIN"
printf 'Qt kit: %s\n' "$QT_KIT"
printf 'macdeployqt: %s\n' "$MACDEPLOYQT_BIN"
if [[ -n "$NINJA_BIN" ]]; then
  printf 'Ninja: %s\n' "$NINJA_BIN"
else
  printf 'Ninja: not found; CMake will use its default generator\n'
fi

log_step "Required Qt components"
for component in Network Positioning SerialPort WebChannel WebEngineWidgets Widgets; do
  printf '%s: %s\n' "$component" "$QT_KIT/lib/cmake/Qt6$component/Qt6${component}Config.cmake"
done
