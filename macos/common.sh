#!/usr/bin/env bash
set -euo pipefail

log_step() {
  printf '\n==> %s\n' "$*"
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

project_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

prepend_path_entry() {
  local entry="${1:-}"
  [[ -n "$entry" && -d "$entry" ]] || return 0
  case ":$PATH:" in
    *":$entry:"*) ;;
    *) export PATH="$entry:$PATH" ;;
  esac
}

remove_safe_directory() {
  local target="${1:-}"
  [[ -n "$target" ]] || return 0

  local root parent full
  root="$(project_root)"
  parent="$(dirname "$target")"
  [[ -d "$parent" ]] || return 0
  full="$(cd "$parent" && pwd)/$(basename "$target")"

  case "$full" in
    "$root"/*) rm -rf "$full" ;;
    *) die "refusing to remove path outside project root: $full" ;;
  esac
}

resolve_cmake() {
  local requested="${1:-}"
  local candidates=()

  [[ -n "$requested" ]] && candidates+=("$requested")
  candidates+=(
    "/Applications/CMake.app/Contents/bin/cmake"
    "$HOME/Qt/Tools/CMake/CMake.app/Contents/bin/cmake"
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  if command -v cmake >/dev/null 2>&1; then
    command -v cmake
    return 0
  fi

  die "cmake not found. Install CMake or pass --cmake-path."
}

resolve_ninja() {
  local requested="${1:-}"
  local candidates=()

  [[ -n "$requested" ]] && candidates+=("$requested")
  if command -v ninja >/dev/null 2>&1; then
    candidates+=("$(command -v ninja)")
  fi
  candidates+=(
    "/opt/homebrew/bin/ninja"
    "$HOME/Qt/Tools/Ninja/ninja"
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

resolve_qt_kit() {
  local qt_root="${1:-$HOME/Qt}"
  local requested="${2:-}"

  if [[ -n "$requested" ]]; then
    [[ -f "$requested/lib/cmake/Qt6/Qt6Config.cmake" ]] || die "Qt6Config.cmake not found in requested kit: $requested"
    cd "$requested" && pwd
    return 0
  fi

  [[ -d "$qt_root" ]] || die "Qt root not found: $qt_root"

  local config
  config="$(find "$qt_root" -path '*/macos/lib/cmake/Qt6/Qt6Config.cmake' -type f 2>/dev/null | sort -r | head -n 1 || true)"
  [[ -n "$config" ]] || die "No Qt6 macOS kit found below $qt_root. Pass --qt-kit-path."

  cd "$(dirname "$config")/../../.." && pwd
}

qt_version_from_kit() {
  local qt_kit="$1"
  basename "$(dirname "$qt_kit")"
}

qt_root_from_kit() {
  local qt_kit="$1"
  cd "$qt_kit/../.." && pwd
}

resolve_maintenance_tool() {
  local qt_root="$1"
  local requested="${QT_MAINTENANCE_TOOL:-}"
  local candidates=()

  [[ -n "$requested" ]] && candidates+=("$requested")
  candidates+=(
    "$qt_root/MaintenanceTool.app/Contents/MacOS/MaintenanceTool"
    "$qt_root/MaintenanceTool"
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

qt_ifw_package_for_component() {
  local qt_kit="$1"
  local component="$2"
  local version compact
  version="$(qt_version_from_kit "$qt_kit")"
  compact="${version//./}"

  case "$component" in
    WebEngineCore|WebEngineQuick|WebEngineWidgets)
      printf 'extensions.qtwebengine.%s.clang_64\n' "$compact"
      ;;
    WebChannel)
      printf 'qt.qt6.%s.addons.qtwebchannel\n' "$compact"
      ;;
    Positioning)
      printf 'qt.qt6.%s.addons.qtpositioning\n' "$compact"
      ;;
    SerialPort)
      printf 'qt.qt6.%s.addons.qtserialport\n' "$compact"
      ;;
    Core|Gui|Network|Widgets)
      printf 'qt.qt6.%s.clang_64\n' "$compact"
      ;;
    *)
      return 1
      ;;
  esac
}

array_contains() {
  local needle="$1"
  shift
  local item
  for item in "$@"; do
    [[ "$item" == "$needle" ]] && return 0
  done
  return 1
}

ensure_qt_components() {
  local qt_kit="$1"
  shift

  local missing=()
  local packages=()
  local component config package

  for component in "$@"; do
    config="$qt_kit/lib/cmake/Qt6$component/Qt6${component}Config.cmake"
    if [[ ! -f "$config" ]]; then
      missing+=("$component")
      package="$(qt_ifw_package_for_component "$qt_kit" "$component" || true)"
      if [[ -n "$package" ]] && ! array_contains "$package" "${packages[@]}"; then
        packages+=("$package")
      fi
    fi
  done

  [[ "${#missing[@]}" -eq 0 ]] && return 0
  [[ "${#packages[@]}" -gt 0 ]] || die "missing Qt components and no installer package mapping exists: ${missing[*]}"

  local qt_root maintenance_tool
  qt_root="$(qt_root_from_kit "$qt_kit")"
  maintenance_tool="$(resolve_maintenance_tool "$qt_root" || true)"
  [[ -n "$maintenance_tool" ]] || die "missing Qt components (${missing[*]}) and Maintenance Tool was not found below $qt_root"

  log_step "Installing missing Qt components: ${missing[*]}"
  "$maintenance_tool" \
    --root "$qt_root" \
    --accept-licenses \
    --accept-messages \
    --accept-obligations \
    --confirm-command \
    install "${packages[@]}"

  for component in "${missing[@]}"; do
    config="$qt_kit/lib/cmake/Qt6$component/Qt6${component}Config.cmake"
    [[ -f "$config" ]] || die "Qt component $component is still missing after Maintenance Tool install"
  done
}

resolve_macdeployqt() {
  local qt_kit="$1"
  local candidates=(
    "$qt_kit/bin/macdeployqt"
    "$qt_kit/bin/macdeployqt6"
  )

  if command -v macdeployqt >/dev/null 2>&1; then
    candidates+=("$(command -v macdeployqt)")
  fi

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  die "macdeployqt not found in Qt kit: $qt_kit"
}

find_app_bundle() {
  local build_dir="$1"
  local install_dir="$2"
  local app_name="${3:-Barebone-Qt}"
  local candidates=(
    "$build_dir/$app_name.app"
    "$install_dir/$app_name.app"
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -d "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  find "$build_dir" "$install_dir" -maxdepth 3 -type d -name "$app_name.app" 2>/dev/null | sort | head -n 1
}
