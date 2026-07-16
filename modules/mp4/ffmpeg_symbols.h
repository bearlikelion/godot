/**************************************************************************/
/*  ffmpeg_symbols.h                                                      */
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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// Every FFmpeg function the mp4 module calls. Resolved at runtime with dlopen
// so the engine has no link-time dependency on FFmpeg; if the libraries are
// missing (or the wrong major version), MP4 playback is unavailable but
// nothing else breaks.
#define FFMPEG_SYMBOLS(X)                    \
	/* avutil */                             \
	X(av_malloc)                             \
	X(av_free)                               \
	X(av_frame_alloc)                        \
	X(av_frame_free)                         \
	X(av_frame_unref)                        \
	X(av_frame_move_ref)                     \
	X(av_channel_layout_default)             \
	X(av_channel_layout_uninit)              \
	X(av_log_set_level)                      \
	X(avutil_version)                        \
	/* avcodec */                            \
	X(av_packet_alloc)                       \
	X(av_packet_free)                        \
	X(av_packet_clone)                       \
	X(av_packet_unref)                       \
	X(avcodec_find_decoder)                  \
	X(avcodec_alloc_context3)                \
	X(avcodec_free_context)                  \
	X(avcodec_parameters_to_context)         \
	X(avcodec_open2)                         \
	X(avcodec_send_packet)                   \
	X(avcodec_receive_frame)                 \
	X(avcodec_flush_buffers)                 \
	X(avcodec_version)                       \
	/* avformat */                           \
	X(avformat_alloc_context)                \
	X(avformat_open_input)                   \
	X(avformat_close_input)                  \
	X(avformat_find_stream_info)             \
	X(av_find_best_stream)                   \
	X(av_read_frame)                         \
	X(av_seek_frame)                         \
	X(avio_alloc_context)                    \
	X(avio_context_free)                     \
	X(avformat_version)                      \
	/* swscale */                            \
	X(sws_getCachedContext)                  \
	X(sws_scale)                             \
	X(sws_freeContext)                       \
	X(swscale_version)                       \
	/* swresample */                         \
	X(swr_alloc_set_opts2)                   \
	X(swr_init)                              \
	X(swr_free)                              \
	X(swr_convert)                           \
	X(swr_get_out_samples)                   \
	X(swresample_version)

struct FFmpegSymbols {
#define FFMPEG_DECLARE_SYMBOL(m_name) decltype(&::m_name) m_name = nullptr;
	FFMPEG_SYMBOLS(FFMPEG_DECLARE_SYMBOL)
#undef FFMPEG_DECLARE_SYMBOL
};

// Returns the resolved symbol table, or nullptr when the system FFmpeg
// libraries are absent or their major versions don't match the vendored
// headers the module was compiled against. Thread-safe, loads once.
const FFmpegSymbols *ffmpeg_symbols();
