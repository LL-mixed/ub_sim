#!/bin/zsh
set -euo pipefail

BUILD_OBMM_TESTS=0
if [[ "${1:-}" == "--with-obmm-tests" ]]; then
  BUILD_OBMM_TESTS=1
  shift
fi
if [[ $# -ne 0 ]]; then
  echo "usage: $0 [--with-obmm-tests]" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/qemu_ub_common.sh"

SRC_DIR="$(qemu_ub_source_path "$REPO_ROOT")"
BUILD_DIR="$(qemu_ub_build_path "$REPO_ROOT")"
BIN="$(qemu_ub_bin_path "$REPO_ROOT")"
TARGET_LIST="${QEMU_TARGET_LIST:-aarch64-softmmu}"
DEFAULT_QEMU_BUILD_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"
if (( DEFAULT_QEMU_BUILD_JOBS > 32 )); then
  DEFAULT_QEMU_BUILD_JOBS=32
fi
JOBS="${QEMU_BUILD_JOBS:-$DEFAULT_QEMU_BUILD_JOBS}"
CONFIGURE_ARGS="${QEMU_CONFIGURE_ARGS:---disable-werror}"
RECONFIGURE="${RECONFIGURE:-0}"
STAMP_FILE="$BUILD_DIR/.qemu_build.stamp"
SIM_QEMU_STATICLIB="${SIM_QEMU_STATICLIB:-}"
BUILD_HOST_OS="$(uname -s 2>/dev/null || echo unknown)"
STAT_BIN="${STAT_BIN:-$(command -v stat 2>/dev/null || echo stat)}"

file_signature() {
  local file_path="$1"

  case "$BUILD_HOST_OS" in
    Darwin|FreeBSD)
      "$STAT_BIN" -f '%N:%m:%z' "$file_path"
      ;;
    *)
      "$STAT_BIN" -c '%n:%Y:%s' "$file_path" 2>/dev/null || "$STAT_BIN" -f '%N:%m:%z' "$file_path"
      ;;
  esac
}

file_mtime() {
  local file_path="$1"

  case "$BUILD_HOST_OS" in
    Darwin|FreeBSD)
      "$STAT_BIN" -f '%m' "$file_path"
      ;;
    *)
      "$STAT_BIN" -c '%Y' "$file_path" 2>/dev/null || "$STAT_BIN" -f '%m' "$file_path"
      ;;
  esac
}

qemu_source_signature() {
  local file

  find "$SRC_DIR/hw/ub" "$SRC_DIR/include/hw/ub" -type f \
    \( -name '*.c' -o -name '*.h' -o -name 'meson.build' -o -name 'trace-events' \) \
    -print 2>/dev/null |
    {
      cat
      printf '%s\n' \
        "$SRC_DIR/hw/arm/virt.c" \
        "$SRC_DIR/include/hw/arm/virt.h" \
        "$SRC_DIR/target/arm/cpu.h" \
        "$SRC_DIR/target/arm/helper.h" \
        "$SRC_DIR/target/arm/tcg/helper-a64.c" \
        "$SRC_DIR/target/arm/tcg/tlb_helper.c" \
        "$SRC_DIR/target/arm/tcg/translate-a64.c" \
        "$SRC_DIR/target/arm/tcg/translate.h" \
        "$SRC_DIR/include/sysemu/iommufd.h"
      printf '%s\n' \
        "$SRC_DIR/tests/unit/test-ub-obmm-remote.c" \
        "$SRC_DIR/tests/unit/test-ub-obmm-remote-model.c" \
        "$SRC_DIR/tests/unit/test-ub-async-load.c"
    } |
    while IFS= read -r file; do
      [[ -f "$file" ]] || continue
      file_signature "$file"
    done |
    sort
}

append_configure_arg_once() {
  local arg="$1"
  local positive="${2:-}"
  local negative="${3:-}"

  if [[ -n "$positive" && "$CONFIGURE_ARGS" == *"$positive"* ]]; then
    return
  fi
  if [[ -n "$negative" && "$CONFIGURE_ARGS" == *"$negative"* ]]; then
    return
  fi
  if [[ "$CONFIGURE_ARGS" != *"$arg"* ]]; then
    CONFIGURE_ARGS="${CONFIGURE_ARGS} $arg"
  fi
}

prepend_pkg_config_path() {
  local dir="$1"

  [[ -d "$dir" ]] || return 0
  if [[ -z "${PKG_CONFIG_PATH:-}" ]]; then
    export PKG_CONFIG_PATH="$dir"
  elif [[ ":$PKG_CONFIG_PATH:" != *":$dir:"* ]]; then
    export PKG_CONFIG_PATH="$dir:$PKG_CONFIG_PATH"
  fi
}

write_macos_pkg_config_shim() {
  local shim="$1"

  cat > "$shim" <<'PY'
#!/usr/bin/env python3
import os
import re
import sys


VERSION = "0.29.2"
OPERATORS = {"=", "==", "!=", "<", ">", "<=", ">="}


def fail(message):
    if "--silence-errors" not in sys.argv:
        print(message, file=sys.stderr)
    sys.exit(1)


def pc_paths():
    return [path for path in os.environ.get("PKG_CONFIG_PATH", "").split(":") if path]


def find_pc(name):
    for directory in pc_paths():
        candidate = os.path.join(directory, f"{name}.pc")
        if os.path.isfile(candidate):
            return candidate
    return None


def joined_lines(path):
    result = []
    pending = ""
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if line.endswith("\\"):
                pending += line[:-1]
                continue
            result.append(pending + line)
            pending = ""
    if pending:
        result.append(pending)
    return result


VAR_REF = re.compile(r"\$\{([^}]+)\}")


def expand(value, variables, depth=0):
    if depth > 20:
        return value

    def replace(match):
        name = match.group(1)
        replacement = variables.get(name, os.environ.get(name, ""))
        return expand(replacement, variables, depth + 1)

    return VAR_REF.sub(replace, value)


cache = {}


def parse_pc(name):
    if name in cache:
        return cache[name]
    path = find_pc(name)
    if not path:
        fail(f"Package {name} was not found in PKG_CONFIG_PATH")
    variables = {}
    fields = {}
    for line in joined_lines(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        eq_at = line.find("=")
        colon_at = line.find(":")
        if eq_at > 0 and (colon_at < 0 or eq_at < colon_at):
            key, value = line.split("=", 1)
            variables[key.strip()] = expand(value.strip(), variables)
            continue
        if colon_at > 0:
            key, value = line.split(":", 1)
            fields[key.strip()] = expand(value.strip(), variables)
    parsed = {"variables": variables, "fields": fields}
    cache[name] = parsed
    return parsed


def package_names(args):
    names = []
    skip_next = False
    for token in args:
        if skip_next:
            skip_next = False
            continue
        if token.startswith("-"):
            if token in {"--define-variable", "--variable"}:
                skip_next = True
            continue
        if token in OPERATORS or re.match(r"^[0-9][0-9A-Za-z_.+-]*$", token):
            continue
        names.extend(part for part in token.split(",") if part)
    return names


def requires(value):
    result = []
    for token in value.replace(",", " ").split():
        if token in OPERATORS or re.match(r"^[0-9][0-9A-Za-z_.+-]*$", token):
            continue
        result.append(token)
    return result


def collect(name, field, include_private, seen=None):
    if seen is None:
        seen = set()
    if name in seen:
        return []
    seen.add(name)
    parsed = parse_pc(name)
    fields = parsed["fields"]
    values = []
    for dep in requires(fields.get("Requires", "")):
        values.extend(collect(dep, field, include_private, seen))
    if include_private:
        for dep in requires(fields.get("Requires.private", "")):
            values.extend(collect(dep, field, include_private, seen))
    values.extend(fields.get(field, "").split())
    if include_private:
        values.extend(fields.get(f"{field}.private", "").split())
    return values


def unique(tokens):
    result = []
    seen = set()
    for token in tokens:
        if token not in seen:
            result.append(token)
            seen.add(token)
    return result


def version_tuple(value):
    parts = re.split(r"[^0-9]+", value)
    return tuple(int(part) for part in parts if part != "")


def main():
    args = sys.argv[1:]
    if not args:
        return 0
    if "--version" in args:
        print(VERSION)
        return 0

    names = package_names(args)
    if "--exists" in args:
        for name in names:
            parse_pc(name)
        return 0

    atleast = next((arg for arg in args if arg.startswith("--atleast-version=")), None)
    if atleast:
        expected = atleast.split("=", 1)[1]
        for name in names:
            found = parse_pc(name)["fields"].get("Version", "0")
            if version_tuple(found) < version_tuple(expected):
                return 1
        return 0

    variable = next((arg.split("=", 1)[1] for arg in args if arg.startswith("--variable=")), None)
    if variable:
        outputs = []
        for name in names:
            parsed = parse_pc(name)
            outputs.append(parsed["variables"].get(variable, parsed["fields"].get(variable, "")))
        print(" ".join(item for item in outputs if item))
        return 0

    if "--modversion" in args:
        print("\n".join(parse_pc(name)["fields"].get("Version", "") for name in names))
        return 0

    include_private = "--static" in args
    outputs = []
    if "--cflags" in args:
        for name in names:
            outputs.extend(collect(name, "Cflags", include_private))
    if "--libs" in args:
        for name in names:
            outputs.extend(collect(name, "Libs", include_private))
    if outputs:
        print(" ".join(unique(outputs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
PY
  chmod +x "$shim"
}

setup_macos_pkg_config_discovery() {
  local brew_prefix=""
  local macos_major=""
  local shim_dir=""
  local shim=""

  [[ "$BUILD_HOST_OS" == "Darwin" ]] || return 0
  if command -v brew >/dev/null 2>&1; then
    brew_prefix="$(brew --prefix 2>/dev/null || true)"
  fi
  if [[ -z "$brew_prefix" && -d /opt/homebrew ]]; then
    brew_prefix="/opt/homebrew"
  elif [[ -z "$brew_prefix" && -d /usr/local/Homebrew ]]; then
    brew_prefix="/usr/local"
  fi
  if [[ -n "$brew_prefix" ]]; then
    prepend_pkg_config_path "$brew_prefix/lib/pkgconfig"
    prepend_pkg_config_path "$brew_prefix/share/pkgconfig"
    macos_major="$(sw_vers -productVersion 2>/dev/null | cut -d. -f1 || true)"
    if [[ -n "$macos_major" ]]; then
      prepend_pkg_config_path "$brew_prefix/Library/Homebrew/os/mac/pkgconfig/$macos_major"
    fi
    prepend_pkg_config_path "$brew_prefix/Library/Homebrew/os/mac/pkgconfig"
  fi
  if [[ -n "${PKG_CONFIG:-}" ]]; then
    return 0
  fi
  if command -v pkg-config >/dev/null 2>&1 || command -v pkgconf >/dev/null 2>&1; then
    return 0
  fi
  shim_dir="$BUILD_DIR/pkg-config-shim"
  mkdir -p "$shim_dir"
  shim="$shim_dir/pkg-config"
  write_macos_pkg_config_shim "$shim"
  export PKG_CONFIG="$shim"
  echo "[build_qemu_binary] macOS build host detected; using in-tree pkg-config shim for Homebrew .pc files" >&2
}

setup_ninja_discovery() {
  local user_base=""
  local user_ninja=""

  if command -v ninja >/dev/null 2>&1; then
    return 0
  fi
  user_base="$(python3 -c 'import site; print(site.getuserbase())' 2>/dev/null || true)"
  user_ninja="$user_base/bin/ninja"
  if [[ -n "$user_base" && -x "$user_ninja" ]]; then
    export PATH="$user_base/bin:$PATH"
  fi
}

apply_host_qemu_configure_args() {
  local host_cc="${CC:-cc}"

  append_configure_arg_once "--disable-docs" "--enable-docs" "--disable-docs"
  case "$BUILD_HOST_OS" in
    Darwin)
      append_configure_arg_once "--disable-zstd" "--enable-zstd" "--disable-zstd"
      echo "[build_qemu_binary] macOS build host detected; using recorded UB QEMU configure profile" >&2
      ;;
    Linux)
      if ! print -r -- '#include <numaif.h>' |
        "$host_cc" -E - >/dev/null 2>&1; then
        append_configure_arg_once \
          "--disable-numa" "--enable-numa" "--disable-numa"
        append_configure_arg_once \
          "--disable-mbind-by-proportion" \
          "--enable-mbind-by-proportion" \
          "--disable-mbind-by-proportion"
      fi
      if ! command -v pkg-config >/dev/null 2>&1 ||
        ! pkg-config --exists liburing; then
        append_configure_arg_once \
          "--disable-linux-io-uring" \
          "--enable-linux-io-uring" \
          "--disable-linux-io-uring"
      fi
      ;;
  esac
}

find_sim_qemu_staticlib() {
  local candidate
  if [[ -n "$SIM_QEMU_STATICLIB" ]]; then
    echo "$SIM_QEMU_STATICLIB"
    return 0
  fi
  for candidate in \
    "$REPO_ROOT/target/release/libsim_qemu.a" \
    "$REPO_ROOT"/target/*/release/libsim_qemu.a(N); do
    if [[ -f "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  echo "$REPO_ROOT/target/release/libsim_qemu.a"
}

build_sim_qemu_staticlib() {
  (
    cd "$REPO_ROOT"
    cargo build --release -p sim-qemu
  )
}

ensure_sim_qemu_link_args() {
  build_sim_qemu_staticlib
  SIM_QEMU_STATICLIB="$(find_sim_qemu_staticlib)"
  if [[ ! -f "$SIM_QEMU_STATICLIB" ]]; then
    echo "[build_qemu_binary] error: missing sim-qemu staticlib: $SIM_QEMU_STATICLIB" >&2
    exit 1
  fi
  if [[ "$CONFIGURE_ARGS" != *"$SIM_QEMU_STATICLIB"* ]]; then
    CONFIGURE_ARGS="${CONFIGURE_ARGS} --extra-ldflags=$SIM_QEMU_STATICLIB"
  fi
}

qemu_build_signature() {
  local qemu_head=""
  local qemu_src_sig=""
  local rust_lib_sig=""
  qemu_head="$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || echo "")"
  qemu_src_sig="$(qemu_source_signature || true)"
  if [[ -f "$SIM_QEMU_STATICLIB" ]]; then
    rust_lib_sig="$(file_signature "$SIM_QEMU_STATICLIB" 2>/dev/null || echo "")"
  fi
  printf 'qemu_head=%s\nqemu_src_sig=%s\ntarget_list=%s\nconfigure_args=%s\nsim_qemu_staticlib=%s\n' \
    "$qemu_head" "$qemu_src_sig" "$TARGET_LIST" "$CONFIGURE_ARGS" "$rust_lib_sig"
}

qemu_build_stamp_matches() {
  [[ -f "$STAMP_FILE" ]] || return 1
  [[ "$(cat "$STAMP_FILE" 2>/dev/null)" == "$(qemu_build_signature)" ]]
}

staticlib_newer_than_qemu_binary() {
  local lib_mtime=""
  local bin_mtime=""

  [[ -n "$SIM_QEMU_STATICLIB" && -f "$SIM_QEMU_STATICLIB" && -e "$BIN" ]] || return 1
  lib_mtime="$(file_mtime "$SIM_QEMU_STATICLIB" 2>/dev/null || echo 0)"
  bin_mtime="$(file_mtime "$BIN" 2>/dev/null || echo 0)"
  [[ "$lib_mtime" == <-> && "$bin_mtime" == <-> ]] || return 1
  (( lib_mtime > bin_mtime ))
}

write_qemu_build_stamp() {
  qemu_build_signature > "$STAMP_FILE"
}

obmm_tests_ready() {
  [[ -x "$BUILD_DIR/tests/unit/test-ub-obmm-remote" &&
     -x "$BUILD_DIR/tests/unit/test-ub-obmm-remote-model" &&
     -x "$BUILD_DIR/tests/unit/test-ub-async-load" ]]
}

TEST_TARGETS_READY=1
if (( BUILD_OBMM_TESTS == 1 )) && ! obmm_tests_ready; then
  TEST_TARGETS_READY=0
fi

if [[ ! -d "$SRC_DIR" ]]; then
  echo "[build_qemu_binary] error: missing QEMU source dir: $SRC_DIR" >&2
  exit 1
fi

apply_host_qemu_configure_args
ensure_sim_qemu_link_args
mkdir -p "$BUILD_DIR"
setup_macos_pkg_config_discovery
setup_ninja_discovery

if [[ "$RECONFIGURE" != "1" && -x "$BIN" ]] &&
   qemu_build_stamp_matches &&
   ! staticlib_newer_than_qemu_binary &&
   qemu_ub_supports_required_opts "$BIN" &&
   (( BUILD_OBMM_TESTS == 0 )) &&
   (( TEST_TARGETS_READY == 1 )); then
  echo "[build_qemu_binary] using existing QEMU binary: $BIN" >&2
  echo "$BIN"
  exit 0
fi

if [[ ! -f "$BUILD_DIR/build.ninja" || "$RECONFIGURE" == "1" ]] || ! qemu_build_stamp_matches; then
  echo "[build_qemu_binary] configuring QEMU in $BUILD_DIR" >&2
  (
    cd "$BUILD_DIR"
    "$SRC_DIR/configure" --target-list="$TARGET_LIST" ${=CONFIGURE_ARGS}
  )
fi

if staticlib_newer_than_qemu_binary; then
  echo "[build_qemu_binary] QEMU binary is older than sim-qemu staticlib; forcing relink" >&2
  rm -f "$BIN"
fi

BUILD_TARGETS=(qemu-system-aarch64)
if (( BUILD_OBMM_TESTS == 1 )); then
  BUILD_TARGETS+=(
    tests/unit/test-ub-obmm-remote
    tests/unit/test-ub-obmm-remote-model
    tests/unit/test-ub-async-load
  )
fi
echo "[build_qemu_binary] building ${BUILD_TARGETS[*]}" >&2
(
  cd "$BUILD_DIR"
  ninja -j"$JOBS" "${BUILD_TARGETS[@]}"
)

if [[ ! -x "$BIN" ]]; then
  echo "[build_qemu_binary] error: missing binary after build: $BIN" >&2
  exit 1
fi

if ! qemu_ub_supports_required_opts "$BIN"; then
  echo "[build_qemu_binary] error: built binary missing required UB options: $BIN" >&2
  exit 1
fi
if (( BUILD_OBMM_TESTS == 1 )) && ! obmm_tests_ready; then
  echo "[build_qemu_binary] error: missing OBMM unit test binaries" >&2
  exit 1
fi

write_qemu_build_stamp

echo "$BIN"
