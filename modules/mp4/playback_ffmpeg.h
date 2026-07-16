/**************************************************************************/
/*  playback_ffmpeg.h                                                     */
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

#include "ffmpeg_symbols.h"
#include "mp4_playback.h"

#include "core/templates/list.h"

// MP4 playback decoded by the system FFmpeg libraries, loaded at runtime
// with dlopen (see ffmpeg_symbols.cpp). Used on Linux/BSD.
class VideoStreamPlaybackFFmpeg : public VideoStreamPlaybackMP4 {
	GDCLASS(VideoStreamPlaybackFFmpeg, VideoStreamPlaybackMP4);

	const FFmpegSymbols *ff = nullptr;

	AVIOContext *avio_ctx = nullptr;
	AVFormatContext *format_ctx = nullptr;
	AVCodecContext *video_ctx = nullptr;
	AVCodecContext *audio_ctx = nullptr;
	SwsContext *sws_ctx = nullptr;
	SwrContext *swr_ctx = nullptr;

	AVFrame *frame = nullptr;
	AVFrame *audio_frame = nullptr;
	AVFrame *pending_frame = nullptr;
	AVPacket *packet = nullptr;

	List<AVPacket *> video_packets;
	List<AVPacket *> audio_packets;

	int video_stream_idx = -1;
	int audio_stream_idx = -1;

	LocalVector<float> resample_buffer;

	bool demux_eof = false;
	bool video_flushed = false;
	bool audio_flushed = false;

	static int _io_read(void *p_opaque, uint8_t *p_buf, int p_size);
	static int64_t _io_seek(void *p_opaque, int64_t p_offset, int p_whence);

	double frame_timestamp(AVFrame *p_frame, AVStream *p_stream) const;
	bool demux_packet();
	int receive_decoded_frame(AVCodecContext *p_ctx, List<AVPacket *> &p_queue, bool &r_flushed, AVFrame *p_out);
	void queue_audio_frame(AVFrame *p_frame);
	void write_video_frame(AVFrame *p_frame);
	void flush_packet_queues();

protected:
	void clear();

public:
	virtual Error open_file(const String &p_file) override;

	virtual void seek(double p_time) override;
	virtual void update(double p_delta) override;

	VideoStreamPlaybackFFmpeg();
	~VideoStreamPlaybackFFmpeg();
};
