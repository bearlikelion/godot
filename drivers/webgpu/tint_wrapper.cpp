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
#include <unordered_map>
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

// Scan the (preprocessed) SPIR-V for descriptor-bound variables that are
// actually referenced from function bodies, and emit one
// "//BINDING_USED:group,binding" WGSL comment line per used binding.
// The driver uses these to narrow bind group layout visibility to the
// stages that really use each binding; browsers enforce per-stage resource
// limits (16 samplers per stage) against layout visibility, and glslang
// declares every uniform in every stage. A stray operand collision can
// only add a false positive, which merely widens visibility (safe).
static std::string _binding_usage_comments(const uint32_t *p_words, size_t p_word_count) {
	if (p_word_count < 5) {
		return {};
	}

	struct BindInfo {
		uint32_t group = UINT32_MAX;
		uint32_t binding = UINT32_MAX;
	};
	std::unordered_map<uint32_t, BindInfo> decorations; // id -> set/binding
	std::unordered_map<uint32_t, BindInfo> vars; // var id -> resolved set/binding

	constexpr uint16_t OP_DECORATE_L = 71;
	constexpr uint16_t OP_VARIABLE_L = 59;
	constexpr uint16_t OP_FUNCTION_L = 54;
	constexpr uint32_t DECO_BINDING = 33;
	constexpr uint32_t DECO_DESCRIPTOR_SET = 34;

	size_t pos = 5;
	bool in_functions = false;
	std::unordered_map<uint32_t, bool> used_ids;
	while (pos < p_word_count) {
		uint32_t w0 = p_words[pos];
		uint32_t wc = w0 >> 16;
		uint16_t op = (uint16_t)(w0 & 0xFFFF);
		if (wc == 0 || pos + wc > p_word_count) {
			break;
		}

		if (op == OP_DECORATE_L && wc >= 4) {
			uint32_t target = p_words[pos + 1];
			uint32_t deco = p_words[pos + 2];
			if (deco == DECO_DESCRIPTOR_SET) {
				decorations[target].group = p_words[pos + 3];
			} else if (deco == DECO_BINDING) {
				decorations[target].binding = p_words[pos + 3];
			}
		} else if (op == OP_VARIABLE_L && wc >= 3 && !in_functions) {
			uint32_t result_id = p_words[pos + 2];
			auto it = decorations.find(result_id);
			if (it != decorations.end() && it->second.group != UINT32_MAX && it->second.binding != UINT32_MAX) {
				vars[result_id] = it->second;
			}
		} else if (op == OP_FUNCTION_L) {
			in_functions = true;
		} else if (in_functions) {
			for (uint32_t i = 1; i < wc; i++) {
				uint32_t word = p_words[pos + i];
				if (vars.count(word)) {
					used_ids[word] = true;
				}
			}
		}
		pos += wc;
	}

	std::string out;
	for (const auto &kv : used_ids) {
		const BindInfo &bi = vars[kv.first];
		out += "//BINDING_USED:" + std::to_string(bi.group) + "," + std::to_string(bi.binding) + "\n";
	}
	return out;
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

	// Prepend per-binding usage metadata for the driver's visibility logic.
	const std::string full = _binding_usage_comments(p_spirv_words, p_word_count) + result.Get();
	char *out = (char *)malloc(full.size() + 1);
	if (!out) {
		return nullptr;
	}
	memcpy(out, full.c_str(), full.size() + 1);
	return out;
}
