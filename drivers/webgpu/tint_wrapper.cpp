/**************************************************************************/
/*  tint_wrapper.cpp                                                      */
/**************************************************************************/
/*                       This file is part of:                            */
/*                           GODOT ENGINE                                 */
/*                      https://godotengine.org                           */
/**************************************************************************/
/* Compiled with C++20 in the Tint build environment.  Wraps Tint's       */
/* SPIR-V reader + WGSL writer behind a simple C-compatible interface     */
/* so that the main Godot driver code (C++17) never includes Tint headers.*/
/**************************************************************************/

#include "tint_wrapper.h"

#include "src/tint/api/tint.h"
#include "src/tint/lang/wgsl/writer/common/options.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <csetjmp>
#include <csignal>

// Tint internal compiler errors (TINT_ICE / TINT_UNIMPLEMENTED / failed
// TINT_ASSERT) print a message and then __builtin_trap(), killing the
// process. On native we intercept the resulting SIGILL/SIGTRAP/SIGABRT
// while a conversion is in flight and long-jump back, turning process
// death into a per-shader conversion failure. Leaks whatever Tint had
// allocated for that conversion, which is acceptable for a rare error
// path. On the web the trap remains fatal (wasm `unreachable` cannot be
// intercepted).
static thread_local sigjmp_buf _tint_ice_jmp;
static thread_local volatile bool _tint_ice_armed = false;

static void _tint_trap_handler(int p_sig) {
	if (_tint_ice_armed) {
		_tint_ice_armed = false;
		siglongjmp(_tint_ice_jmp, 1);
	}
	// Not a guarded Tint conversion: restore default disposition and re-raise.
	signal(p_sig, SIG_DFL);
	raise(p_sig);
}

static void _install_trap_handlers() {
	static bool installed = false;
	if (installed) {
		return;
	}
	installed = true;
	struct sigaction sa = {};
	sa.sa_handler = _tint_trap_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGTRAP, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
}
#endif // !__EMSCRIPTEN__

void tint_wrapper_initialize() {
	tint::Initialize();
}

char *tint_wrapper_spirv_to_wgsl(const uint32_t *p_spirv_words, size_t p_word_count, char **r_error) {
	std::vector<uint32_t> words(p_spirv_words, p_spirv_words + p_word_count);

	// Allow all WGSL extensions and language features so Tint can emit
	// constructs like readonly storage textures without validation errors.
	tint::wgsl::writer::Options wgsl_options;
	wgsl_options.allowed_features = tint::wgsl::AllowedFeatures::Everything();
	// Godot's GLSL shaders use textureSample/dpdx in non-uniform control flow
	// (valid in Vulkan, but WGSL requires uniform control flow for derivatives).
	// This inserts `diagnostic(off, derivative_uniformity)` in the output.
	wgsl_options.allow_non_uniform_derivatives = true;

#ifndef __EMSCRIPTEN__
	_install_trap_handlers();
	if (sigsetjmp(_tint_ice_jmp, 1) != 0) {
		// A Tint internal compiler error trapped; surface it as a failure.
		if (r_error) {
			const char *msg = "Tint internal compiler error (trapped); shader cannot be converted to WGSL";
			char *err = (char *)malloc(strlen(msg) + 1);
			if (err) {
				strcpy(err, msg);
			}
			*r_error = err;
		}
		return nullptr;
	}
	_tint_ice_armed = true;
#endif

	auto result = tint::SpirvToWgsl(words, wgsl_options);

#ifndef __EMSCRIPTEN__
	_tint_ice_armed = false;
#endif

	if (result != tint::Success) {
		if (r_error) {
			const std::string &reason = result.Failure().reason;
			char *err = (char *)malloc(reason.size() + 1);
			if (err) {
				memcpy(err, reason.c_str(), reason.size() + 1);
			}
			*r_error = err;
		}
		return nullptr;
	}

	const std::string &wgsl = result.Get();
	char *out = (char *)malloc(wgsl.size() + 1);
	if (!out) {
		return nullptr;
	}
	memcpy(out, wgsl.c_str(), wgsl.size() + 1);
	return out;
}
