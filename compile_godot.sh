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
#   ./compile_godot.sh --web --release
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
DEV_BUILD=1
DO_CLEAN=0
ALL_TEMPLATES=0
JOBS=""
UPLOAD_SYMBOLS_URL=""
MACOS_ARCH="universal"   # universal | arm64 | x86_64 (osxcross cross-builds only)
OSXCROSS_SDK="${OSXCROSS_SDK:-}"     # e.g. darwin24.4; required when cross-compiling macOS. The
                                     # osxcross toolchain image exports it, so a build running
                                     # inside that image (CI `container:`) needs no flag.
USE_PODMAN=0             # build linux inside the old-glibc buildroot container
PODMAN_IMAGE="localhost/godot-linux:4.7-f43"
PODMAN_ARCH="x86_64"     # x86_64 | i686 | aarch64 | arm (buildroot SDK arch)
MACOS_PODMAN_IMAGE="localhost/godot-macos:osxcross"  # osxcross toolchain image; see build_container.sh in the osxcross checkout
MACOS_VULKAN_SDK_PATH="${OSXCROSS_VULKAN_SDK_PATH:-}"  # MoltenVK location; set by the osxcross image, or read back out of it under --podman
MACOS_CONTAINER_RESOLVED=0           # guard so the image is only interrogated once per run
OSXCROSS_LINKER="lld"    # linker for osxcross cross-builds; "ld64" opts back out (see build())
OSXCROSS_ROOT="${OSXCROSS_ROOT:-}"  # osxcross install root; required (with its target/bin on PATH) for macOS cross-builds
WEB_NOTHREADS=0          # build the web template with threads=no (no SharedArrayBuffer/COOP+COEP needed)
WEB_DLINK=0              # build the web template with dlink_enabled=yes (GDExtension support)
WEBGPU=0                 # build with webgpu=yes (WebGPU rendering driver)
ENCRYPTION_KEY_FILE=""   # path to a 64-hex-char PCK script encryption key file (e.g. ~/.config/godot/godot.gdkey)
DEFAULT_ENCRYPTION_KEY_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/godot/godot.gdkey"  # used when neither --encryption-key nor SCRIPT_AES256_ENCRYPTION_KEY is set

# ----------------------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------------------
usage() {
	cat <<'EOF'
compile_godot.sh - cross-platform Godot build driver

Target platform (default: host OS):
  --linux                Build for Linux (platform=linuxbsd)
  --windows              Build for Windows (platform=windows; needs mingw when cross-compiling)
  --macos                Build for macOS (platform=macos; build on macOS, or cross-compile
                          via osxcross on Linux with --osxcross-sdk)
  --web                  Build for the Web (platform=web; needs an activated Emscripten
                          toolchain, i.e. emcc on PATH via "source emsdk_env.sh").
                          Templates only: combine with --release / --debug-template.

Build type (default: --editor):
  --editor               Editor build (dev_build, clang+mold+ccache, no LTO)
  --no-dev-build         Build the editor without dev_build=yes (smaller/faster, non-dev binary)
  --release              Release export template (production, lto=full, no update check)
  --debug-template       Debug export template (production)
  --all-templates        Build release + debug templates for linux+windows (+macos on a mac
                          or with --osxcross-sdk, +web when emcc is on PATH)

Options:
  --webgpu               Build with webgpu=yes (WebGPU rendering driver). Native builds
                         need a wgpu-native dist at thirdparty/wgpu-native (or set
                         wgpu_native_dir); see drivers/webgpu/SCsub.
  --lto MODE             Override LTO: none | thin | full
  --no-deprecated        Build with deprecated=no (smaller surface; may break old projects)
  --jobs N               Parallel jobs (default: detected core count)
  --upload-symbols URL   After the build, POST each new .debugsymbols (keyed by its GNU
                         Build ID) to a GDCrashCatch analyzer at URL/api/symbols. Needs
                         CC_ADMIN_SECRET in the environment.
  --encryption-key FILE  Bake a PCK script encryption key into the template. FILE must
                         hold exactly 64 hexadecimal characters (e.g.
                         ~/.config/godot/godot.gdkey). Must match the key your export
                         preset encrypts the PCK with, or the game fails to load the .pck
                         with an MD5/decryption-key mismatch. Resolution order when omitted:
                         the SCRIPT_AES256_ENCRYPTION_KEY environment variable, then
                         $XDG_CONFIG_HOME/godot/godot.gdkey (~/.config/godot/godot.gdkey).
                         If none of those exist, the template is built with no encryption
                         (zero key).
  --web-nothreads        Web only: build with threads=no. The resulting template does not
                         need SharedArrayBuffer (no COOP/COEP headers on the web server),
                         at the cost of no thread support. Matches the "nothreads" official
                         template variant; the export preset's Thread Support option must
                         agree with the variant installed.
  --web-dlink            Web only: build with dlink_enabled=yes for GDExtension support
                         (bigger and slower template; matches the "dlink" official variant).
  --osxcross-sdk SDK     osxcross SDK/toolchain id (e.g. darwin24.4) for cross-compiling
                         macOS from Linux. Requires osxcross's target/bin on PATH. Not
                         needed with --macos --podman: the value is read back out of the
                         toolchain image, which knows which SDK it was built against.
  --macos-arch ARCH      universal | arm64 | x86_64 (default: universal) for macOS builds.
  --osxcross-linker LD   Linker for macOS cross-builds (default: lld). osxcross pins
                         cctools' ld64 at version 711, the last Apple open-sourced, which
                         cannot synthesise the ObjC selector stubs clang >= 14 emits for
                         arm64 - a stock cross-link fails with hundreds of undefined
                         _objc_msgSend$... symbols. Pass "ld64" to use osxcross's linker
                         anyway (expect that failure on any ObjC-heavy target).
  --podman               Build inside a container instead of with the host toolchain.
                          For --linux (platform=linuxbsd) that is the godot-linux
                          buildroot image (GCC, glibc 2.28 baseline), so the binary runs
                          on older glibc systems (e.g. Steam Deck); requires the
                          godot-linux:4.7-f43 image, see build-containers/build.sh in the
                          godotengine/build-containers repo.
                          For --macos it is the osxcross toolchain image, which is how you
                          cross-compile macOS on a Linux CI box; build it by running
                          ./build_container.sh in the osxcross checkout.
  --podman-image IMG     Override the linux container image (default: localhost/godot-linux:4.7-f43)
  --podman-arch ARCH     x86_64 | i686 | aarch64 | arm (default: x86_64)
  --macos-podman-image IMG
                         Override the osxcross container image
                         (default: localhost/godot-macos:osxcross)
  --clean                scons --clean for the selected target, then exit
  -h, --help             Show this help

Every build always includes:
  disable_overrides=yes
  debug_symbols=yes separate_debug_symbols=yes   (stripped binary + .debugsymbols side file;
                                                  native platforms only, not web/wasm)
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

# Resolve the PCK script encryption key, if any, into SCRIPT_AES256_ENCRYPTION_KEY so
# scons (core/SCsub) bakes it into the template. Precedence: --encryption-key FILE, then
# an already-exported SCRIPT_AES256_ENCRYPTION_KEY. With neither set, the template builds
# with no encryption (zero key) and will not load a PCK exported with a real key.
#
# The key must be exactly 64 hex chars. Key files (e.g. godot.gdkey) typically carry a
# trailing newline, so we strip whitespace before validating. We export the cleaned value
# so the native-scons and podman paths both see the same canonical key.
resolve_encryption_key() {
	local key=""
	if [ -n "$ENCRYPTION_KEY_FILE" ]; then
		[ -f "$ENCRYPTION_KEY_FILE" ] || err "--encryption-key file not found: $ENCRYPTION_KEY_FILE"
		key="$(tr -d '[:space:]' < "$ENCRYPTION_KEY_FILE")"
	elif [ -n "${SCRIPT_AES256_ENCRYPTION_KEY:-}" ]; then
		key="$(printf '%s' "$SCRIPT_AES256_ENCRYPTION_KEY" | tr -d '[:space:]')"
	elif [ -f "$DEFAULT_ENCRYPTION_KEY_FILE" ]; then
		echo ">>> using default encryption key: $DEFAULT_ENCRYPTION_KEY_FILE" >&2
		key="$(tr -d '[:space:]' < "$DEFAULT_ENCRYPTION_KEY_FILE")"
	else
		echo ">>> no encryption key (--encryption-key / SCRIPT_AES256_ENCRYPTION_KEY unset, no $DEFAULT_ENCRYPTION_KEY_FILE); building unencrypted template" >&2
		return
	fi

	if ! printf '%s' "$key" | grep -qiE '^[0-9a-f]{64}$'; then
		err "encryption key must be exactly 64 hexadecimal characters (got ${#key} chars)"
	fi

	export SCRIPT_AES256_ENCRYPTION_KEY="$key"
	echo ">>> baking PCK script encryption key into the template" >&2
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

	if [ "$plat" = "web" ]; then
		# Web builds compile through Emscripten; an activated emsdk puts emcc on PATH.
		command -v emcc >/dev/null 2>&1 ||
			err "web builds need the Emscripten toolchain on PATH (install emsdk, then 'source emsdk_env.sh')"
	fi

	if [ "$plat" = "macos" ] && [ "$host" != "macos" ]; then
		[ -n "$OSXCROSS_SDK" ] || err "cross-compiling macOS from $host needs --osxcross-sdk (e.g. darwin24.4)"

		# The osxcross toolchain is usually not on PATH in a fresh shell, so look for it in
		# the usual install roots before giving up. Without this the OSXCROSS_ROOT fallback
		# below can never run, since it derives the root from a clang already on PATH.
		if ! command -v "x86_64-apple-${OSXCROSS_SDK}-clang" >/dev/null 2>&1; then
			local candidate
			for candidate in "$OSXCROSS_ROOT" "$HOME/Source/osxcross" "$HOME/osxcross" /opt/osxcross /usr/local/osxcross; do
				[ -n "$candidate" ] || continue
				if [ -x "$candidate/target/bin/x86_64-apple-${OSXCROSS_SDK}-clang" ]; then
					PATH="$candidate/target/bin:$PATH"
					export PATH
					echo ">>> using osxcross toolchain at $candidate/target/bin" >&2
					break
				fi
			done
		fi

		command -v "x86_64-apple-${OSXCROSS_SDK}-clang" >/dev/null 2>&1 ||
			err "osxcross toolchain for sdk '$OSXCROSS_SDK' not found on PATH (expected x86_64-apple-${OSXCROSS_SDK}-clang)"
		if [ -z "$OSXCROSS_ROOT" ]; then
			# scons gates platform=macos support on OSXCROSS_ROOT being set (see
			# platform/macos/detect.py can_build()); derive it from the toolchain on PATH
			# if not already exported (osxcross root is the parent of target/bin).
			local osxcross_bin
			osxcross_bin="$(command -v "x86_64-apple-${OSXCROSS_SDK}-clang")"
			OSXCROSS_ROOT="$(cd "$(dirname "$osxcross_bin")/../.." && pwd)"
		fi
		export OSXCROSS_ROOT
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
		--web) PLATFORM="web" ;;
		--web-nothreads) WEB_NOTHREADS=1 ;;
		--web-dlink) WEB_DLINK=1 ;;
		--editor) BUILD_TYPE="editor" ;;
		--release) BUILD_TYPE="release" ;;
		--debug-template) BUILD_TYPE="debug-template" ;;
		--all-templates) ALL_TEMPLATES=1 ;;
		--lto)
			shift; [ $# -gt 0 ] || err "--lto needs an argument"
			LTO="$1" ;;
		--no-deprecated) NO_DEPRECATED=1 ;;
		--no-dev-build) DEV_BUILD=0 ;;
		--webgpu) WEBGPU=1 ;;
		--jobs)
			shift; [ $# -gt 0 ] || err "--jobs needs an argument"
			JOBS="$1" ;;
		--upload-symbols)
			shift; [ $# -gt 0 ] || err "--upload-symbols needs a URL"
			UPLOAD_SYMBOLS_URL="$1" ;;
		--encryption-key)
			shift; [ $# -gt 0 ] || err "--encryption-key needs a file path"
			ENCRYPTION_KEY_FILE="$1" ;;
		--osxcross-sdk)
			shift; [ $# -gt 0 ] || err "--osxcross-sdk needs an argument"
			OSXCROSS_SDK="$1" ;;
		--macos-arch)
			shift; [ $# -gt 0 ] || err "--macos-arch needs an argument"
			MACOS_ARCH="$1" ;;
		--osxcross-linker)
			shift; [ $# -gt 0 ] || err "--osxcross-linker needs an argument"
			OSXCROSS_LINKER="$1" ;;
		--podman) USE_PODMAN=1 ;;
		--podman-image)
			shift; [ $# -gt 0 ] || err "--podman-image needs an argument"
			PODMAN_IMAGE="$1" ;;
		--macos-podman-image)
			shift; [ $# -gt 0 ] || err "--macos-podman-image needs an argument"
			MACOS_PODMAN_IMAGE="$1" ;;
		--podman-arch)
			shift; [ $# -gt 0 ] || err "--podman-arch needs an argument"
			PODMAN_ARCH="$1" ;;
		--clean) DO_CLEAN=1 ;;
		-h | --help) usage; exit 0 ;;
		*) err "unknown argument: $1 (try --help)" ;;
	esac
	shift
done

[ -n "$PLATFORM" ] || PLATFORM="$(detect_host_platform)"
[ -n "$JOBS" ] || JOBS="$(detect_jobs)"

resolve_encryption_key

# ----------------------------------------------------------------------------------------
# Build command assembly
# ----------------------------------------------------------------------------------------

# Run a single scons build. Args: <platform> <build-type> [arch override]
build() {
	local plat="$1"
	local btype="$2"
	local arch_override="${3:-}"

	# Interrogate the osxcross image before the universal split below, so both arch
	# passes and the lipo step see the same resolved toolchain.
	if [ "$plat" = "macos" ] && [ "$USE_PODMAN" -eq 1 ]; then
		resolve_macos_container_env
	fi

	# macOS has no scons arch=universal; build arm64 + x86_64 separately and lipo them.
	if [ "$plat" = "macos" ] && [ "$MACOS_ARCH" = "universal" ] && [ -z "$arch_override" ]; then
		build "$plat" "$btype" "arm64"
		build "$plat" "$btype" "x86_64"
		lipo_macos_universal "$btype"
		return
	fi

	if [ "$plat" = "web" ] && [ "$btype" = "editor" ]; then
		err "the web platform only builds export templates; use --release or --debug-template with --web"
	fi

	local use_podman=0
	if [ "$USE_PODMAN" -eq 1 ] && { [ "$plat" = "linuxbsd" ] || [ "$plat" = "macos" ]; }; then
		use_podman=1
	else
		check_toolchain "$plat"
	fi

	# Base flags shared by every build.
	local -a args=(
		"platform=$plat"
		"disable_overrides=yes"
		"-j$JOBS"
	)

	# Separate .debugsymbols side files are an ELF/PE concept; wasm has no equivalent and
	# forcing debug symbols into a web template would bloat the shipped .wasm instead.
	if [ "$plat" != "web" ]; then
		args+=(
			"debug_symbols=yes"
			"separate_debug_symbols=yes"
		)
	fi

	if [ "$WEBGPU" -eq 1 ]; then
		args+=("webgpu=yes")
	fi

	if [ "$plat" = "web" ]; then
		if [ "$WEB_NOTHREADS" -eq 1 ]; then
			args+=("threads=no")
		fi
		if [ "$WEB_DLINK" -eq 1 ]; then
			args+=("dlink_enabled=yes")
		fi
	fi

	if [ "$plat" = "macos" ]; then
		args+=("arch=${arch_override:-$MACOS_ARCH}")
		if [ -n "$OSXCROSS_SDK" ]; then
			args+=("osxcross_sdk=$OSXCROSS_SDK")

			# osxcross pins cctools' ld64 at LINKER_VERSION=711, the last version Apple
			# open-sourced (Xcode 12). It cannot synthesise the ObjC selector stubs
			# (_objc_msgSend$foo) that clang >= 14 emits for arm64, so a stock cross-link
			# of an ObjC-heavy target like Godot ends in hundreds of undefined
			# _objc_msgSend$... symbols. Stub support landed in ld64-820/Xcode 14, which
			# was never released as source, so cctools-port will not gain it - LLVM's
			# ld64.lld is the fix, not a workaround. --osxcross-linker ld64 opts out.
			if [ "$OSXCROSS_LINKER" != "ld64" ]; then
				args+=("linkflags=-fuse-ld=$OSXCROSS_LINKER")
			fi
		fi
		# Godot disables the Metal driver for non-arm64, so the x86_64 slice needs Vulkan
		# via MoltenVK. LunarG's macOS SDK installer does not run on Linux; the osxcross
		# image bundles the MoltenVK xcframework instead and reports where it put it.
		if [ -n "$MACOS_VULKAN_SDK_PATH" ]; then
			args+=("vulkan_sdk_path=$MACOS_VULKAN_SDK_PATH")
		fi
	fi

	# The buildroot SDK is per-arch; the osxcross image already got its arch above.
	if [ "$use_podman" -eq 1 ] && [ "$plat" = "linuxbsd" ]; then
		args+=("arch=$PODMAN_ARCH")
	fi

	case "$btype" in
		editor)
			args+=("target=editor")
			if [ "$DEV_BUILD" -eq 1 ]; then
				args+=("dev_build=yes")
			fi
			# Fast iteration toolchain only makes sense on a native Linux host build.
			if [ "$plat" = "linuxbsd" ] && [ "$use_podman" -eq 0 ]; then
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

	# LTO: explicit override wins; otherwise native templates default to full, editor to
	# none. Web templates pass nothing and let production=yes pick Emscripten's default;
	# forcing lto=full there makes wasm-ld linking extremely slow and memory hungry.
	if [ -n "$LTO" ]; then
		args+=("lto=$LTO")
	elif [ "$btype" != "editor" ] && [ "$plat" != "web" ]; then
		args+=("lto=full")
	fi

	if [ "$NO_DEPRECATED" -eq 1 ]; then
		args+=("deprecated=no")
	fi

	if [ "$DO_CLEAN" -eq 1 ]; then
		args+=("--clean")
	fi

	if [ "$use_podman" -eq 1 ] && [ "$plat" = "macos" ]; then
		run_podman_macos_scons "${args[@]}"
	elif [ "$use_podman" -eq 1 ]; then
		run_podman_scons "${args[@]}"
	else
		echo ">>> scons ${args[*]}"
		scons "${args[@]}"
	fi

	if [ "$DO_CLEAN" -eq 0 ]; then
		if [ "$plat" = "web" ]; then
			report_web_template "$btype"
		else
			report_debug_symbols "$plat"
		fi
	fi
}

# The web build zips itself into bin/ (godot.web.<target>.wasm32[.nothreads][.dlink].zip).
# The editor only picks it up from the export templates directory under the matching
# official name, so print exactly where to install it.
report_web_template() {
	local btype="$1"
	local target_suffix install_name
	case "$btype" in
		release) target_suffix="template_release"; install_name="web_release.zip" ;;
		debug-template) target_suffix="template_debug"; install_name="web_debug.zip" ;;
		*) err "internal: unknown build type '$btype'" ;;
	esac

	local variant=""
	if [ "$WEB_NOTHREADS" -eq 1 ]; then
		variant=".nothreads"
		install_name="web_nothreads_${install_name#web_}"
	fi
	if [ "$WEB_DLINK" -eq 1 ]; then
		variant="${variant}.dlink"
		install_name="web_dlink_${install_name#web_}"
	fi

	local zip="bin/godot.web.${target_suffix}.wasm32${variant}.zip"
	[ -f "$zip" ] || err "expected web template zip not found: $zip"

	# The editor looks for templates under <major>.<minor>[.<patch>].<status>, mirroring
	# VERSION_FULL_CONFIG (patch is omitted when 0). Read it from version.py in the repo root.
	local version_dir
	version_dir="$(python3 -c '
import version
number = f"{version.major}.{version.minor}"
if version.patch != 0:
    number += f".{version.patch}"
print(f"{number}.{version.status}")
' 2>/dev/null || true)"
	[ -n "$version_dir" ] || version_dir="<version>"

	echo ">>> web template: $zip"
	echo ">>> install it for the editor with:"
	echo "    mkdir -p ~/.local/share/godot/export_templates/${version_dir}"
	echo "    cp $zip ~/.local/share/godot/export_templates/${version_dir}/${install_name}"
}

# Combine the arm64 + x86_64 macOS binaries scons just built into one universal (fat)
# binary via lipo, matching the official godot-build-scripts approach (scons has no
# arch=universal of its own).
lipo_macos_universal() {
	local btype="$1"
	local target_suffix
	case "$btype" in
		editor) target_suffix="editor" ;;
		release) target_suffix="template_release" ;;
		debug-template) target_suffix="template_debug" ;;
		*) err "internal: unknown build type '$btype'" ;;
	esac

	local arm64_bin="bin/godot.macos.${target_suffix}.arm64"
	local x86_64_bin="bin/godot.macos.${target_suffix}.x86_64"
	local universal_bin="bin/godot.macos.${target_suffix}.universal"

	[ -f "$arm64_bin" ] || err "expected arm64 binary not found: $arm64_bin"
	[ -f "$x86_64_bin" ] || err "expected x86_64 binary not found: $x86_64_bin"

	echo ">>> lipo -create $arm64_bin $x86_64_bin -output $universal_bin"
	run_lipo -create "$arm64_bin" "$x86_64_bin" -output "$universal_bin"
	echo ">>> universal binary: $universal_bin"
	run_lipo -info "$universal_bin"
}

# lipo is a Mach-O tool: on a mac it is the system one, on a Linux host it comes from
# osxcross (build.sh symlinks target/bin/lipo), and under --podman it only exists inside
# the toolchain image. Paths are repo-relative, and the image mounts the repo at the same
# working directory, so the arguments carry over unchanged.
run_lipo() {
	if [ "$USE_PODMAN" -eq 1 ]; then
		podman run --rm \
			-v "$(pwd)":/root/godot:z \
			-w /root/godot \
			"$MACOS_PODMAN_IMAGE" \
			lipo "$@"
	else
		command -v lipo >/dev/null 2>&1 || err "lipo not found on PATH (expected from osxcross)"
		lipo "$@"
	fi
}

# Run a scons build inside the godot-linux buildroot container (GCC, old glibc baseline)
# so the resulting binary works on systems with older glibc than this host's (e.g. Steam
# Deck). Mounts the repo read-write so build artifacts land directly in bin/.
run_podman_scons() {
	command -v podman >/dev/null 2>&1 || err "podman not found on PATH"
	podman image exists "$PODMAN_IMAGE" ||
		err "podman image '$PODMAN_IMAGE' not found; build it from godotengine/build-containers (Dockerfile.base + Dockerfile.linux)"

	local sdk_dir="${PODMAN_ARCH}-godot-linux-gnu_sdk-buildroot"
	if [ "$PODMAN_ARCH" = "arm" ]; then
		sdk_dir="arm-godot-linux-gnueabihf_sdk-buildroot"
	fi

	# The container starts with a clean environment, so the script encryption key resolved
	# on the host (SCRIPT_AES256_ENCRYPTION_KEY) must be forwarded explicitly with -e or the
	# template would be built unencrypted (zero key) and fail to load an encrypted PCK.
	local -a podman_env=()
	if [ -n "${SCRIPT_AES256_ENCRYPTION_KEY:-}" ]; then
		podman_env+=("-e" "SCRIPT_AES256_ENCRYPTION_KEY")
	fi

	echo ">>> podman run (image=$PODMAN_IMAGE, arch=$PODMAN_ARCH) scons $*"
	podman run --rm \
		-v "$(pwd)":/root/godot:z \
		-w /root/godot \
		"${podman_env[@]}" \
		"$PODMAN_IMAGE" \
		bash -c "export PATH=\"/root/${sdk_dir}/bin:\$PATH\" && scons $(printf '%q ' "$@")"
}

# Read the toolchain triple and the bundled MoltenVK path back out of the osxcross image.
# The image was built against one specific macOS SDK and its wrappers are named after the
# matching triple (arm64-apple-darwin24.4-cc and friends), so asking it beats making the
# caller repeat a value that is already fixed at image build time.
resolve_macos_container_env() {
	[ "$MACOS_CONTAINER_RESOLVED" -eq 1 ] && return
	MACOS_CONTAINER_RESOLVED=1

	command -v podman >/dev/null 2>&1 || err "podman not found on PATH"
	podman image exists "$MACOS_PODMAN_IMAGE" ||
		err "podman image '$MACOS_PODMAN_IMAGE' not found; build it by running ./build_container.sh in the osxcross checkout"

	local probe sdk mvk
	probe="$(podman run --rm "$MACOS_PODMAN_IMAGE" \
		sh -c 'printf "%s\n%s\n" "${OSXCROSS_SDK:-}" "${OSXCROSS_VULKAN_SDK_PATH:-}"')" ||
		err "cannot run '$MACOS_PODMAN_IMAGE'"
	sdk="$(printf '%s' "$probe" | sed -n '1p')"
	mvk="$(printf '%s' "$probe" | sed -n '2p')"

	[ -n "$sdk" ] ||
		err "image '$MACOS_PODMAN_IMAGE' does not set OSXCROSS_SDK; rebuild it with build_container.sh"

	# A mismatch here would fail deep inside scons with a missing compiler binary, so catch
	# it while the message can still say what went wrong.
	if [ -n "$OSXCROSS_SDK" ] && [ "$OSXCROSS_SDK" != "$sdk" ]; then
		err "--osxcross-sdk $OSXCROSS_SDK does not match the image's toolchain ($sdk); drop the flag or rebuild the image against that SDK"
	fi

	OSXCROSS_SDK="$sdk"
	[ -n "$MACOS_VULKAN_SDK_PATH" ] || MACOS_VULKAN_SDK_PATH="$mvk"

	echo ">>> osxcross image $MACOS_PODMAN_IMAGE: toolchain $OSXCROSS_SDK, linker $OSXCROSS_LINKER"
}

# Run a scons build inside the osxcross toolchain image. The image already exports
# OSXCROSS_ROOT (which platform/macos/detect.py gates macOS support on) and puts
# target/bin on PATH, so nothing has to be set up here.
run_podman_macos_scons() {
	# The container starts with a clean environment, so a script encryption key resolved on
	# the host has to be forwarded explicitly or the template is silently built with a zero
	# key and will not load an encrypted PCK.
	local -a podman_env=()
	if [ -n "${SCRIPT_AES256_ENCRYPTION_KEY:-}" ]; then
		podman_env+=("-e" "SCRIPT_AES256_ENCRYPTION_KEY")
	fi

	echo ">>> podman run (image=$MACOS_PODMAN_IMAGE) scons $*"
	podman run --rm \
		-v "$(pwd)":/root/godot:z \
		-w /root/godot \
		"${podman_env[@]}" \
		"$MACOS_PODMAN_IMAGE" \
		bash -c "scons $(printf '%q ' "$@")"
}

# Point out where the .debugsymbols side files landed so they can be archived for
# crash decoding. Godot emits "<binary>.debugsymbols" (objcopy --only-keep-debug +
# --add-gnu-debuglink) on Linux and Windows/MinGW. Optionally publishes them to a
# GDCrashCatch analyzer keyed by GNU Build ID.
report_debug_symbols() {
	local plat="$1"
	echo ">>> debug symbol files (archive these to symbolicate crash reports):"
	local found=0
	while IFS= read -r sym; do
		found=1
		echo "    $sym"
		if [ -n "$UPLOAD_SYMBOLS_URL" ]; then
			upload_symbols_file "$sym" "$plat"
		fi
	done < <(find bin -maxdepth 1 -name '*.debugsymbols' -newermt '-2 minutes' -print 2>/dev/null)
	[ "$found" -eq 1 ] || echo "    (none found)"
}

# Extract the GNU Build ID from the stripped binary that pairs with a .debugsymbols
# file, then POST the symbols to the analyzer's /api/symbols endpoint.
upload_symbols_file() {
	local sym="$1"
	local plat="$2"
	local binary="${sym%.debugsymbols}"

	[ -n "${CC_ADMIN_SECRET:-}" ] || err "--upload-symbols needs CC_ADMIN_SECRET in the environment"
	command -v curl >/dev/null 2>&1 || err "--upload-symbols needs curl"

	# Extract just the hex Build ID. The note line wraps differently across
	# readelf versions, so grep the 40-hex-char (SHA1) token rather than a column.
	local build_id=""
	local reader=""
	command -v llvm-readelf >/dev/null 2>&1 && reader="llvm-readelf"
	[ -z "$reader" ] && command -v readelf >/dev/null 2>&1 && reader="readelf"
	if [ -n "$reader" ]; then
		build_id="$("$reader" -n "$binary" 2>/dev/null | grep -ioE '[0-9a-f]{40}' | head -1)"
	fi
	if [ -z "$build_id" ]; then
		echo "    ! could not read Build ID from $binary; skipping upload"
		return
	fi

	echo "    uploading symbols (build_id=$build_id) to $UPLOAD_SYMBOLS_URL/api/symbols"
	curl -fsS -H "X-CrashCatch-Admin: $CC_ADMIN_SECRET" \
		-F "build_id=$build_id" -F "platform=$plat" \
		-F "file=@$sym" \
		"$UPLOAD_SYMBOLS_URL/api/symbols" >/dev/null \
		&& echo "    ok" \
		|| echo "    ! upload failed"
}

# ----------------------------------------------------------------------------------------
# Dispatch
# ----------------------------------------------------------------------------------------
if [ "$ALL_TEMPLATES" -eq 1 ]; then
	host="$(detect_host_platform)"
	platforms=("linuxbsd" "windows")
	if [ "$host" = "macos" ] || [ -n "$OSXCROSS_SDK" ]; then
		platforms+=("macos")
	elif [ "$USE_PODMAN" -eq 1 ] && command -v podman >/dev/null 2>&1 &&
		podman image exists "$MACOS_PODMAN_IMAGE" 2>/dev/null; then
		# --podman on a Linux host can cover macOS too, but only if the osxcross image is
		# actually there; otherwise leave it out rather than failing a multi-platform run.
		platforms+=("macos")
	else
		echo ">>> skipping macOS templates (no --osxcross-sdk, and no $MACOS_PODMAN_IMAGE image for --podman)" >&2
	fi
	if command -v emcc >/dev/null 2>&1; then
		platforms+=("web")
	else
		echo ">>> skipping web templates (emcc not on PATH; 'source emsdk_env.sh' to include them)" >&2
	fi
	for p in "${platforms[@]}"; do
		build "$p" "release"
		build "$p" "debug-template"
	done
else
	build "$PLATFORM" "$BUILD_TYPE"
fi

echo ">>> done."
