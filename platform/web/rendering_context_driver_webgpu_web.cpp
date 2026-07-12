/**************************************************************************/
/*  rendering_context_driver_webgpu_web.cpp                               */
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

#ifdef WEBGPU_ENABLED

#include "rendering_context_driver_webgpu_web.h"

#include "core/variant/variant.h"

// html5_webgpu.h was removed in Emscripten 5.x when USE_WEBGPU was dropped.
// The device is imported from JS using the emdawnwebgpu port's Dawn API.
#include <emscripten/emscripten.h>

Error RenderingContextDriverWebGPUWeb::_acquire_device() {
	// The HTML shell pre-initializes a GPUDevice and stores it in Module.preinitializedWebGPUDevice.
	// We use the emdawnwebgpu port's WebGPU.importJsDevice() to wrap it in a C WGPUDevice handle.
	// Note: emdawnwebgpu is a thin JS wrapper around the browser's WebGPU API.
	// SPIR-V is NOT supported. We use Tint (C++, linked in) for SPIR-V to WGSL
	// conversion in shader_create_from_container() instead.
	device = (WGPUDevice)(uintptr_t)EM_ASM_PTR({
		var d = Module["preinitializedWebGPUDevice"];
		if (!d) { return 0; }
		return WebGPU["importJsDevice"](d);
	});
	ERR_FAIL_COND_V_MSG(device == nullptr, ERR_CANT_CREATE, "WebGPU: Failed to get pre-initialized device. Ensure JS shell calls navigator.gpu.requestDevice() before WASM.");

	// The browser exposes a single anonymous GPU context.
	device_info.name = "WebGPU Device";
	device_info.vendor = Vendor::VENDOR_UNKNOWN;
	device_info.type = DEVICE_TYPE_INTEGRATED_GPU;

	print_verbose("WebGPU: Device imported from JS successfully.");
	return OK;
}

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPUWeb::surface_create(const void *p_platform_data) {
	// p_platform_data is expected to contain a canvas selector string (e.g., "#canvas").
	// For the web platform, we use the default canvas "#canvas".
	const char *canvas_selector = "#canvas";
	if (p_platform_data != nullptr) {
		// TODO: Extract canvas selector from platform data if provided.
		// For now, use default.
	}

	// Emscripten 5.x / emdawnwebgpu renamed this struct.
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_desc = {};
	canvas_desc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
	canvas_desc.selector = WGPUStringView{ canvas_selector, WGPU_STRLEN };

	WGPUSurfaceDescriptor surface_desc = {};
	surface_desc.nextInChain = (WGPUChainedStruct *)&canvas_desc;

	// Note: We need an instance to create a surface. If we don't have one,
	// create a minimal one. In Emscripten, the instance is a lightweight wrapper.
	if (get_instance() == nullptr) {
		WGPUInstanceDescriptor inst_desc = {};
		instance = wgpuCreateInstance(&inst_desc);
	}

	WGPUSurface wgpu_surface = wgpuInstanceCreateSurface(get_instance(), &surface_desc);
	ERR_FAIL_COND_V_MSG(wgpu_surface == nullptr, 0, "WebGPU: Failed to create surface from canvas.");

	return _register_surface(wgpu_surface);
}

#endif // WEBGPU_ENABLED
