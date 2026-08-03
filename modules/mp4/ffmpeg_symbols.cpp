/**************************************************************************/
/*  ffmpeg_symbols.cpp                                                    */
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

#include "ffmpeg_symbols.h"

#include "core/os/mutex.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"

#ifdef MP4_FFMPEG_STATIC

// FFmpeg is linked statically into the binary; the symbol table just points
// at the real functions, so playback is always available.
const FFmpegSymbols *ffmpeg_symbols() {
	static Mutex mutex;
	static FFmpegSymbols symbols;
	static bool initialized = false;

	MutexLock lock(mutex);
	if (!initialized) {
		initialized = true;
#define FFMPEG_ASSIGN_SYMBOL(m_name) symbols.m_name = &::m_name;
		FFMPEG_SYMBOLS(FFMPEG_ASSIGN_SYMBOL)
#undef FFMPEG_ASSIGN_SYMBOL
		// Demuxer/decoder warnings (e.g. about the custom IO protocol) are noise here.
		symbols.av_log_set_level(AV_LOG_ERROR);
	}
	return &symbols;
}

#else // dlopen the system libraries at runtime

#include <dlfcn.h>
#include <stdio.h>

struct FFmpegLib {
	const char *name;
	int major;
};

// Majors baked in from the vendored headers; a runtime library with a
// different major has incompatible struct layouts and must be rejected.
static const FFmpegLib ffmpeg_libs[] = {
	{ "avutil", LIBAVUTIL_VERSION_MAJOR },
	{ "swresample", LIBSWRESAMPLE_VERSION_MAJOR },
	{ "swscale", LIBSWSCALE_VERSION_MAJOR },
	{ "avcodec", LIBAVCODEC_VERSION_MAJOR },
	{ "avformat", LIBAVFORMAT_VERSION_MAJOR },
};

static void *open_ffmpeg_lib(const FFmpegLib &p_lib) {
	char soname[64];
	snprintf(soname, sizeof(soname), "lib%s.so.%d", p_lib.name, p_lib.major);
	void *handle = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		// Unversioned dev symlink; the version check below rejects mismatches.
		snprintf(soname, sizeof(soname), "lib%s.so", p_lib.name);
		handle = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
	}
	return handle;
}

static bool load_ffmpeg(FFmpegSymbols &r_symbols) {
	const int lib_count = sizeof(ffmpeg_libs) / sizeof(ffmpeg_libs[0]);
	void *handles[lib_count] = {};

	for (int i = 0; i < lib_count; i++) {
		handles[i] = open_ffmpeg_lib(ffmpeg_libs[i]);
		if (!handles[i]) {
			print_verbose(vformat("mp4: system lib%s.so.%d not found, MP4 playback unavailable.", ffmpeg_libs[i].name, ffmpeg_libs[i].major));
			for (int j = 0; j < i; j++) {
				dlclose(handles[j]);
			}
			return false;
		}
	}

	bool ok = true;
#define FFMPEG_RESOLVE_SYMBOL(m_name)                                                       \
	if (ok) {                                                                               \
		void *sym = nullptr;                                                                \
		for (int i = 0; i < lib_count && !sym; i++) {                                       \
			sym = dlsym(handles[i], #m_name);                                               \
		}                                                                                   \
		if (sym) {                                                                          \
			r_symbols.m_name = reinterpret_cast<decltype(r_symbols.m_name)>(sym);           \
		} else {                                                                            \
			print_verbose("mp4: missing FFmpeg symbol " #m_name ", MP4 playback unavailable."); \
			ok = false;                                                                     \
		}                                                                                   \
	}
	FFMPEG_SYMBOLS(FFMPEG_RESOLVE_SYMBOL)
#undef FFMPEG_RESOLVE_SYMBOL

	if (ok) {
		// The soname usually guarantees this, but the unversioned fallback doesn't.
		const unsigned int majors[lib_count] = {
			r_symbols.avutil_version() >> 16,
			r_symbols.swresample_version() >> 16,
			r_symbols.swscale_version() >> 16,
			r_symbols.avcodec_version() >> 16,
			r_symbols.avformat_version() >> 16,
		};
		for (int i = 0; i < lib_count; i++) {
			if (majors[i] != (unsigned int)ffmpeg_libs[i].major) {
				print_verbose(vformat("mp4: system lib%s major version %d doesn't match the expected %d, MP4 playback unavailable.", ffmpeg_libs[i].name, majors[i], ffmpeg_libs[i].major));
				ok = false;
			}
		}
	}

	if (!ok) {
		for (int i = 0; i < lib_count; i++) {
			dlclose(handles[i]);
		}
		return false;
	}

	// Demuxer/decoder warnings (e.g. about the custom IO protocol) are noise here.
	r_symbols.av_log_set_level(AV_LOG_ERROR);
	return true;
}

const FFmpegSymbols *ffmpeg_symbols() {
	static Mutex mutex;
	static FFmpegSymbols symbols;
	static bool attempted = false;
	static bool available = false;

	MutexLock lock(mutex);
	if (!attempted) {
		attempted = true;
		available = load_ffmpeg(symbols);
	}
	return available ? &symbols : nullptr;
}

#endif // MP4_FFMPEG_STATIC
