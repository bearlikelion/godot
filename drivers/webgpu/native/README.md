# Native WebGPU backend (wgpu-native)

On desktop platforms the WebGPU driver links against
[wgpu-native](https://github.com/gfx-rs/wgpu-native), the C API of the Rust
wgpu implementation. On the web the same driver uses the browser's WebGPU
through Emscripten's emdawnwebgpu port instead; both expose the standard
`webgpu.h` from the same webgpu-headers generation.

## Providing the library

The build expects a wgpu-native "dist" layout at `thirdparty/wgpu-native`
(gitignored), or wherever `wgpu_native_dir=<path>` points:

```
include/webgpu/webgpu.h
include/webgpu/wgpu.h
lib/libwgpu_native.a        (Linux/macOS; wgpu_native.lib on Windows MSVC)
```

Two ways to fill it:

1. **Release zip**: download the matching platform zip from
   https://github.com/gfx-rs/wgpu-native/releases and unpack it to
   `thirdparty/wgpu-native`.

2. **Local build** (what the dev machine does):

   ```sh
   cd /path/to/wgpu-native
   git submodule update --init
   cargo build --release
   mkdir -p <godot>/thirdparty/wgpu-native/include/webgpu <godot>/thirdparty/wgpu-native/lib
   cp ffi/webgpu-headers/webgpu.h ffi/wgpu.h <godot>/thirdparty/wgpu-native/include/webgpu/
   cp target/release/libwgpu_native.a <godot>/thirdparty/wgpu-native/lib/
   ```

## Building Godot

```sh
scons platform=linuxbsd target=editor webgpu=yes
bin/godot.linuxbsd.editor.x86_64 --rendering-driver webgpu --rendering-method mobile
```

Windows and macOS builds follow the same pattern (`platform=windows` /
`platform=macos` with `webgpu=yes`) but need a wgpu-native dist built for
that target platform in `wgpu_native_dir` (`wgpu_native.lib` for MSVC,
`libwgpu_native.a` for MinGW and macOS). X11 and Wayland are both wired
on Linux; select with `--display-driver x11|wayland`.

`webgpu_precompile_wgsl` defaults to `auto`: the build-time SPIR-V to WGSL
table is generated for web builds only (it needs the `tint_convert_cli` and
`glslangValidator` host tools). Native builds ship an empty table and rely on
the runtime Tint translation plus its in-memory cache. Pass
`webgpu_precompile_wgsl=yes` to precompile on native too.

## Header pinning

wgpu-native pins webgpu-headers commit `673658bc` (v29.x); emdawnwebgpu
tracks Dawn's copy of the same generation. Treat "bump emsdk" and "bump
wgpu-native" as a paired operation and re-check
`drivers/webgpu/godot_webgpu.h` for divergences when doing either.
