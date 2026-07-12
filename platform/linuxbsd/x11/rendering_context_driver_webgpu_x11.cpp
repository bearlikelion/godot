/**************************************************************************/
/*  rendering_context_driver_webgpu_x11.cpp                               */
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

#include "rendering_context_driver_webgpu_x11.h"

#include "core/variant/variant.h"

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPUX11::surface_create(const void *p_platform_data) {
	const WindowPlatformData *wpd = (const WindowPlatformData *)p_platform_data;
	ERR_FAIL_NULL_V(wpd, SurfaceID());
	ERR_FAIL_NULL_V(get_instance(), SurfaceID());

	WGPUSurfaceSourceXlibWindow xlib_source = {};
	xlib_source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
	xlib_source.display = wpd->display;
	xlib_source.window = (uint64_t)wpd->window;

	WGPUSurfaceDescriptor surface_desc = {};
	surface_desc.nextInChain = (WGPUChainedStruct *)&xlib_source;

	WGPUSurface wgpu_surface = wgpuInstanceCreateSurface(get_instance(), &surface_desc);
	ERR_FAIL_NULL_V_MSG(wgpu_surface, SurfaceID(), "WebGPU: Failed to create surface from X11 window.");

	return _register_surface(wgpu_surface);
}

#endif // WEBGPU_ENABLED
