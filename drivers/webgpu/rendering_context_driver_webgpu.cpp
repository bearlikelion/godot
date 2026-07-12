/**************************************************************************/
/*  rendering_context_driver_webgpu.cpp                                   */
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

#include "rendering_context_driver_webgpu.h"
#include "rendering_device_driver_webgpu.h"

RenderingContextDriverWebGPU::RenderingContextDriverWebGPU() {
}

RenderingContextDriverWebGPU::~RenderingContextDriverWebGPU() {
	if (queue) {
		wgpuQueueRelease(queue);
		queue = nullptr;
	}
	if (device) {
		wgpuDeviceRelease(device);
		device = nullptr;
	}
	if (adapter) {
		wgpuAdapterRelease(adapter);
		adapter = nullptr;
	}
	if (instance) {
		wgpuInstanceRelease(instance);
		instance = nullptr;
	}
}

Error RenderingContextDriverWebGPU::initialize() {
	// Create a WGPUInstance. Needed for surface creation and for delivering
	// async callbacks (buffer map readback, query results, etc.) via
	// poll_events(). The backend acquires the adapter/device afterwards.
	WGPUInstanceDescriptor inst_desc = {};
	instance = wgpuCreateInstance(&inst_desc);
	if (!instance) {
		WARN_PRINT("WebGPU: wgpuCreateInstance returned null — async readback may not work.");
	}

	Error err = _acquire_device();
	if (err != OK) {
		return err;
	}
	ERR_FAIL_COND_V_MSG(device == nullptr, ERR_CANT_CREATE, "WebGPU: Backend did not acquire a device.");

	queue = wgpuDeviceGetQueue(device);
	ERR_FAIL_COND_V_MSG(queue == nullptr, ERR_CANT_CREATE, "WebGPU: Failed to get device queue.");

	return OK;
}

#ifdef WEBGPU_NATIVE_ENABLED

static void _wgpu_log_callback(WGPULogLevel p_level, WGPUStringView p_message, void *p_userdata) {
	String msg = String::utf8(p_message.data, p_message.length);
	switch (p_level) {
		case WGPULogLevel_Error:
			ERR_PRINT(vformat("wgpu: %s", msg));
			break;
		case WGPULogLevel_Warn:
			WARN_PRINT(vformat("wgpu: %s", msg));
			break;
		default:
			print_verbose(vformat("wgpu: %s", msg));
			break;
	}
}

static void _wgpu_uncaptured_error_callback(WGPUDevice const *p_device, WGPUErrorType p_type, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	ERR_PRINT(vformat("WebGPU uncaptured error (type %d): %s", (int)p_type, String::utf8(p_message.data, p_message.length)));
}

static void _wgpu_device_lost_callback(WGPUDevice const *p_device, WGPUDeviceLostReason p_reason, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	if (p_reason == WGPUDeviceLostReason_Destroyed) {
		return; // Expected on shutdown.
	}
	ERR_PRINT(vformat("WebGPU device lost (reason %d): %s", (int)p_reason, String::utf8(p_message.data, p_message.length)));
}

struct WebGPUAdapterRequest {
	WGPUAdapter adapter = nullptr;
	bool done = false;
	String error;
};

static void _wgpu_request_adapter_callback(WGPURequestAdapterStatus p_status, WGPUAdapter p_adapter, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	WebGPUAdapterRequest *req = (WebGPUAdapterRequest *)p_userdata1;
	req->done = true;
	if (p_status == WGPURequestAdapterStatus_Success) {
		req->adapter = p_adapter;
	} else {
		req->error = String::utf8(p_message.data, p_message.length);
	}
}

struct WebGPUDeviceRequest {
	WGPUDevice device = nullptr;
	bool done = false;
	String error;
};

static void _wgpu_request_device_callback(WGPURequestDeviceStatus p_status, WGPUDevice p_device, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	WebGPUDeviceRequest *req = (WebGPUDeviceRequest *)p_userdata1;
	req->done = true;
	if (p_status == WGPURequestDeviceStatus_Success) {
		req->device = p_device;
	} else {
		req->error = String::utf8(p_message.data, p_message.length);
	}
}

#endif // WEBGPU_NATIVE_ENABLED

Error RenderingContextDriverWebGPU::_acquire_device() {
#ifdef WEBGPU_NATIVE_ENABLED
	ERR_FAIL_NULL_V_MSG(instance, ERR_CANT_CREATE, "WebGPU: No instance to request an adapter from.");

	wgpuSetLogCallback(_wgpu_log_callback, nullptr);

	// wgpu-native fires request callbacks synchronously inside the call; the
	// bounded drain loops below are future-proofing in case it goes async.
	WebGPUAdapterRequest adapter_req;
	{
		WGPURequestAdapterOptions options = {};
		options.powerPreference = WGPUPowerPreference_HighPerformance;
		WGPURequestAdapterCallbackInfo cb = {};
		cb.mode = WGPUCallbackMode_AllowProcessEvents;
		cb.callback = _wgpu_request_adapter_callback;
		cb.userdata1 = &adapter_req;
		wgpuInstanceRequestAdapter(instance, &options, cb);
		for (uint32_t i = 0; i < 1000 && !adapter_req.done; i++) {
			wgpuInstanceProcessEvents(instance);
		}
	}
	ERR_FAIL_NULL_V_MSG(adapter_req.adapter, ERR_CANT_CREATE, vformat("WebGPU: Failed to request adapter: %s", adapter_req.error));
	adapter = adapter_req.adapter;

	// Fill device_info from the adapter.
	WGPUAdapterInfo info = {};
	if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
		device_info.name = String::utf8(info.device.data, info.device.length);
		device_info.vendor = info.vendorID;
		switch (info.adapterType) {
			case WGPUAdapterType_DiscreteGPU:
				device_info.type = DEVICE_TYPE_DISCRETE_GPU;
				break;
			case WGPUAdapterType_IntegratedGPU:
				device_info.type = DEVICE_TYPE_INTEGRATED_GPU;
				break;
			case WGPUAdapterType_CPU:
				device_info.type = DEVICE_TYPE_CPU;
				break;
			default:
				device_info.type = DEVICE_TYPE_OTHER;
				break;
		}
		print_verbose(vformat("WebGPU: Adapter: %s (driver: %s)", device_info.name, String::utf8(info.description.data, info.description.length)));
		wgpuAdapterInfoFreeMembers(info);
	} else {
		device_info.name = "WebGPU Device";
		device_info.vendor = Vendor::VENDOR_UNKNOWN;
		device_info.type = DEVICE_TYPE_OTHER;
	}

	// Mirror the optional features the web JS shell requests (engine.js),
	// where this header generation has an enum for them.
	const WGPUFeatureName optional_features[] = {
		WGPUFeatureName_TimestampQuery,
		WGPUFeatureName_TextureFormatsTier1,
		WGPUFeatureName_TextureFormatsTier2,
		WGPUFeatureName_Float32Filterable,
		WGPUFeatureName_Float32Blendable,
		WGPUFeatureName_RG11B10UfloatRenderable,
		WGPUFeatureName_ClipDistances,
		WGPUFeatureName_DualSourceBlending,
		WGPUFeatureName_Depth32FloatStencil8,
		WGPUFeatureName_TextureCompressionBC,
		WGPUFeatureName_TextureCompressionETC2,
		WGPUFeatureName_TextureCompressionASTC,
	};
	LocalVector<WGPUFeatureName> required_features;
	for (WGPUFeatureName feature : optional_features) {
		if (wgpuAdapterHasFeature(adapter, feature)) {
			required_features.push_back(feature);
		}
	}

	// Request the limits the adapter offers (the JS shell maxes the ones
	// Godot cares about; requesting the full adapter limits is equivalent).
	WGPULimits adapter_limits = WGPU_LIMITS_INIT;
	bool has_limits = wgpuAdapterGetLimits(adapter, &adapter_limits) == WGPUStatus_Success;

	WebGPUDeviceRequest device_req;
	{
		WGPUDeviceDescriptor desc = {};
		desc.label = WGPUStringView{ "Godot WebGPU Device", WGPU_STRLEN };
		desc.requiredFeatureCount = required_features.size();
		desc.requiredFeatures = required_features.ptr();
		desc.requiredLimits = has_limits ? &adapter_limits : nullptr;
		desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
		desc.deviceLostCallbackInfo.callback = _wgpu_device_lost_callback;
		desc.uncapturedErrorCallbackInfo.callback = _wgpu_uncaptured_error_callback;

		WGPURequestDeviceCallbackInfo cb = {};
		cb.mode = WGPUCallbackMode_AllowProcessEvents;
		cb.callback = _wgpu_request_device_callback;
		cb.userdata1 = &device_req;
		wgpuAdapterRequestDevice(adapter, &desc, cb);
		for (uint32_t i = 0; i < 1000 && !device_req.done; i++) {
			wgpuInstanceProcessEvents(instance);
		}
	}
	ERR_FAIL_NULL_V_MSG(device_req.device, ERR_CANT_CREATE, vformat("WebGPU: Failed to request device: %s", device_req.error));
	device = device_req.device;

	print_verbose("WebGPU: Native device acquired via wgpu-native.");
	return OK;
#else
	ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "WebGPU: No backend implementation for device acquisition on this platform.");
#endif
}

void RenderingContextDriverWebGPU::poll_events(bool p_wait) {
#ifdef WEBGPU_NATIVE_ENABLED
	// On wgpu-native, callbacks are only delivered by wgpuDevicePoll;
	// wgpuInstanceProcessEvents alone never fires map/work-done callbacks.
	if (device) {
		wgpuDevicePoll(device, p_wait ? 1 : 0, nullptr);
		return;
	}
#endif
	if (instance) {
		wgpuInstanceProcessEvents(instance);
	}
}

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPU::_register_surface(WGPUSurface p_surface) {
	SurfaceID id = next_surface_id++;
	Surface &surface = surfaces[id];
	surface.handle = p_surface;
	surface.width = 0;
	surface.height = 0;
	surface.needs_resize = true;
	return id;
}

const RenderingContextDriver::Device &RenderingContextDriverWebGPU::device_get(uint32_t p_device_index) const {
	DEV_ASSERT(p_device_index == 0);
	return device_info;
}

uint32_t RenderingContextDriverWebGPU::device_get_count() const {
	return 1; // Browser exposes a single GPU context.
}

bool RenderingContextDriverWebGPU::device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const {
	return true; // Single device always supports the canvas surface.
}

RenderingDeviceDriver *RenderingContextDriverWebGPU::driver_create() {
	return memnew(RenderingDeviceDriverWebGPU(this));
}

void RenderingContextDriverWebGPU::driver_free(RenderingDeviceDriver *p_driver) {
	memdelete(p_driver);
}

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPU::surface_create(const void *p_platform_data) {
	DEV_ASSERT(false && "Surface creation should not be called on the platform-agnostic version of the driver.");
	return SurfaceID();
}

void RenderingContextDriverWebGPU::surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) {
	ERR_FAIL_COND(!surfaces.has(p_surface));
	Surface &surface = surfaces[p_surface];
	if (surface.width != p_width || surface.height != p_height) {
		surface.width = p_width;
		surface.height = p_height;
		surface.needs_resize = true;
	}
}

void RenderingContextDriverWebGPU::surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) {
	ERR_FAIL_COND(!surfaces.has(p_surface));
	surfaces[p_surface].vsync_mode = p_vsync_mode;
}

DisplayServerEnums::VSyncMode RenderingContextDriverWebGPU::surface_get_vsync_mode(SurfaceID p_surface) const {
	ERR_FAIL_COND_V(!surfaces.has(p_surface), DisplayServerEnums::VSYNC_ENABLED);
	return surfaces[p_surface].vsync_mode;
}

uint32_t RenderingContextDriverWebGPU::surface_get_width(SurfaceID p_surface) const {
	ERR_FAIL_COND_V(!surfaces.has(p_surface), 0);
	return surfaces[p_surface].width;
}

uint32_t RenderingContextDriverWebGPU::surface_get_height(SurfaceID p_surface) const {
	ERR_FAIL_COND_V(!surfaces.has(p_surface), 0);
	return surfaces[p_surface].height;
}

void RenderingContextDriverWebGPU::surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) {
	ERR_FAIL_COND(!surfaces.has(p_surface));
	surfaces[p_surface].needs_resize = p_needs_resize;
}

bool RenderingContextDriverWebGPU::surface_get_needs_resize(SurfaceID p_surface) const {
	ERR_FAIL_COND_V(!surfaces.has(p_surface), false);
	return surfaces[p_surface].needs_resize;
}

void RenderingContextDriverWebGPU::surface_destroy(SurfaceID p_surface) {
	ERR_FAIL_COND(!surfaces.has(p_surface));
	Surface &surface = surfaces[p_surface];
	if (surface.handle) {
		wgpuSurfaceRelease(surface.handle);
	}
	surfaces.erase(p_surface);
}

bool RenderingContextDriverWebGPU::is_debug_utils_enabled() const {
	return false; // No debug utils in browser WebGPU.
}

WGPUSurface RenderingContextDriverWebGPU::surface_get_handle(SurfaceID p_surface) const {
	const Surface *s = surfaces.getptr(p_surface);
	ERR_FAIL_COND_V(s == nullptr, nullptr);
	return s->handle;
}

#endif // WEBGPU_ENABLED
