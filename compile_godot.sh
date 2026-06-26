#!/usr/bin/env bash
#
# compile_godot.sh - reusable cross-platform build driver for this Godot fork.
#
# Wraps SCons with sane, hardened defaults so the editor and export templates can be
# built the same way every time. All builds carry the project's standing requirements:
#   - disable_overrides=yes        (override.cfg support compiled out)
#   - debug_symbols + separate_debug_symbols=yes
#                                  (stripped binary + side .debug for crash symbolication)
#
# Usage:
#   ./compile_godot.sh --editor --linux
#   ./compile_godot.sh --windows --release
#   ./compile_godot.sh --all-templates
#
# See --help for the full flag list.

set -euo pipefail

# ----------------------------------------------------------------------------------------
# Defaults
# ----------------------------------------------------------------------------------------
PLATFORM=""          # linuxbsd | windows | macos (default: host)
BUILD_TYPE="editor"  # editor | release | debug-template
LTO=""               # "" (auto from build type) | none | thin | full
NO_DEPRECATED=0
DO_CLEAN=0
ALL_TEMPLATES=0
JOBS=""

# ----------------------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------------------
usage() {
	cat <<'EOF'
compile_godot.sh - cross-platform Godot build driver

Target platform (default: host OS):
  --linux                Build for Linux (platform=linuxbsd)
  --windows              Build for Windows (platform=windows; needs mingw when cross-compiling)
  --macos                Build for macOS (platform=macos; build on macOS)

Build type (default: --editor):
  --editor               Editor build (dev_build, clang+mold+ccache, no LTO)
  --release              Release export template (production, lto=full, no update check)
  --debug-template       Debug export template (production)
  --all-templates        Build release + debug templates for linux+windows (+macos on a mac)

Options:
  --lto MODE             Override LTO: none | thin | full
  --no-deprecated        Build with deprecated=no (smaller surface; may break old projects)
  --jobs N               Parallel jobs (default: detected core count)
  --clean                scons --clean for the selected target, then exit
  -h, --help             Show this help

Every build always includes:
  disable_overrides=yes
  debug_symbols=yes separate_debug_symbols=yes   (stripped binary + .debug side file)
EOF
}

err() {
	echo "error: $*" >&2
	exit 1
}

detect_host_platform() {
	case "$(uname -s)" in
		Linux) echo "linuxbsd" ;;
		Darwin) echo "macos" ;;
		MINGW* | MSYS* | CYGWIN*) echo "windows" ;;
		*) err "unsupported host OS: $(uname -s)" ;;
	esac
}

detect_jobs() {
	if command -v nproc >/dev/null 2>&1; then
		nproc
	elif command -v sysctl >/dev/null 2>&1; then
		sysctl -n hw.ncpu
	else
		echo 4
	fi
}

# Pre-flight toolchain checks for the chosen platform.
check_toolchain() {
	local plat="$1"
	command -v scons >/dev/null 2>&1 || err "scons not found on PATH"

	local host
	host="$(detect_host_platform)"

	if [ "$plat" = "windows" ] && [ "$host" != "windows" ]; then
		# Cross-compiling to Windows from a non-Windows host needs a mingw toolchain.
		if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 &&
			! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
			err "cross-compiling for Windows needs the mingw-w64 toolchain (x86_64-w64-mingw32-g++)"
		fi
	fi

	if [ "$plat" = "macos" ] && [ "$host" != "macos" ]; then
		err "macOS builds must run on macOS (osxcross/MoltenVK not assumed here)"
	fi
}

# ----------------------------------------------------------------------------------------
# Argument parsing
# ----------------------------------------------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
		--linux) PLATFORM="linuxbsd" ;;
		--windows) PLATFORM="windows" ;;
		--macos) PLATFORM="macos" ;;
		--editor) BUILD_TYPE="editor" ;;
		--release) BUILD_TYPE="release" ;;
		--debug-template) BUILD_TYPE="debug-template" ;;
		--all-templates) ALL_TEMPLATES=1 ;;
		--lto)
			shift; [ $# -gt 0 ] || err "--lto needs an argument"
			LTO="$1" ;;
		--no-deprecated) NO_DEPRECATED=1 ;;
		--jobs)
			shift; [ $# -gt 0 ] || err "--jobs needs an argument"
			JOBS="$1" ;;
		--clean) DO_CLEAN=1 ;;
		-h | --help) usage; exit 0 ;;
		*) err "unknown argument: $1 (try --help)" ;;
	esac
	shift
done

[ -n "$PLATFORM" ] || PLATFORM="$(detect_host_platform)"
[ -n "$JOBS" ] || JOBS="$(detect_jobs)"

# ----------------------------------------------------------------------------------------
# Build command assembly
# ----------------------------------------------------------------------------------------

# Run a single scons build. Args: <platform> <build-type>
build() {
	local plat="$1"
	local btype="$2"

	check_toolchain "$plat"

	# Base flags shared by every build.
	local -a args=(
		"platform=$plat"
		"disable_overrides=yes"
		"debug_symbols=yes"
		"separate_debug_symbols=yes"
		"-j$JOBS"
	)

	case "$btype" in
		editor)
			args+=(
				"target=editor"
				"dev_build=yes"
			)
			# Fast iteration toolchain only makes sense on a Linux host build.
			if [ "$plat" = "linuxbsd" ]; then
				args+=(
					"use_llvm=yes"
					"linker=mold"
					"c_compiler_launcher=ccache"
					"cpp_compiler_launcher=ccache"
				)
			fi
			;;
		release)
			args+=(
				"target=template_release"
				"production=yes"
				"engine_update_check=no"
			)
			;;
		debug-template)
			args+=(
				"target=template_debug"
				"production=yes"
			)
			;;
		*)
			err "internal: unknown build type '$btype'"
			;;
	esac

	# LTO: explicit override wins; otherwise templates default to full, editor to none.
	if [ -n "$LTO" ]; then
		args+=("lto=$LTO")
	elif [ "$btype" != "editor" ]; then
		args+=("lto=full")
	fi

	if [ "$NO_DEPRECATED" -eq 1 ]; then
		args+=("deprecated=no")
	fi

	if [ "$DO_CLEAN" -eq 1 ]; then
		args+=("--clean")
	fi

	echo ">>> scons ${args[*]}"
	scons "${args[@]}"

	if [ "$DO_CLEAN" -eq 0 ]; then
		report_debug_symbols "$plat"
	fi
}

# Point out where the .debug side files landed so they can be archived for crash decoding.
report_debug_symbols() {
	local plat="$1"
	echo ">>> debug symbol files (archive these to symbolicate crash reports):"
	# Godot writes the separated symbols next to the binary in bin/.
	find bin -maxdepth 1 -name '*.debug' -newermt '-2 minutes' -print 2>/dev/null || true
}

# ----------------------------------------------------------------------------------------
# Dispatch
# ----------------------------------------------------------------------------------------
if [ "$ALL_TEMPLATES" -eq 1 ]; then
	host="$(detect_host_platform)"
	platforms=("linuxbsd" "windows")
	if [ "$host" = "macos" ]; then
		platforms+=("macos")
	fi
	for p in "${platforms[@]}"; do
		build "$p" "release"
		build "$p" "debug-template"
	done
else
	build "$PLATFORM" "$BUILD_TYPE"
fi

echo ">>> done."
