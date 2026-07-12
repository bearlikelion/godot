/**************************************************************************/
/*  godot_webgpu.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#ifdef WEBGPU_ENABLED

// Umbrella header for the WebGPU driver. The driver codes against the
// standard webgpu.h C API, which is provided by two backends:
// - Web (Emscripten): the emdawnwebgpu port, a thin wrapper over the
//   browser's WebGPU JS API.
// - Native (WEBGPU_NATIVE_ENABLED): wgpu-native, the C API of Rust wgpu.
// Both track the same webgpu-headers generation; backend-specific
// extensions are only available behind the defines below.

#include <webgpu/webgpu.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#ifdef WEBGPU_NATIVE_ENABLED
// wgpu-native extensions: wgpuDevicePoll, wgpuQueueSubmitForIndex,
// wgpuSetLogCallback, wgpuDeviceCreateShaderModuleSpirV, ...
#include <webgpu/wgpu.h>
#endif

// Define WEBGPU_VERBOSE to enable diagnostic console.log prints in the
// browser. Disabled by default for production builds.
// #define WEBGPU_VERBOSE

#if defined(WEBGPU_VERBOSE) && defined(__EMSCRIPTEN__)
#define WEBGPU_DIAG(...) EM_ASM(__VA_ARGS__)
#define WEBGPU_DIAG_INT(...) EM_ASM_INT(__VA_ARGS__)
#else
#define WEBGPU_DIAG(...) ((void)0)
#define WEBGPU_DIAG_INT(...) 0
#endif

#endif // WEBGPU_ENABLED
