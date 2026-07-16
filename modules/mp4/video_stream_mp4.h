/**************************************************************************/
/*  video_stream_mp4.h                                                    */
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

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"
#include "scene/resources/video_stream.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if LIBAVFORMAT_VERSION_MAJOR < 59
#error "The mp4 module requires FFmpeg 5.1 or later."
#endif

class ImageTexture;

class VideoStreamPlaybackMP4 : public VideoStreamPlayback {
	GDCLASS(VideoStreamPlaybackMP4, VideoStreamPlayback);

	Ref<FileAccess> file;
	String file_name;

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

	Point2i size;
	Vector<uint8_t> frame_data;
	Ref<ImageTexture> texture;

	// Interleaved float samples waiting to be accepted by the mixer.
	LocalVector<float> audio_fifo;
	uint32_t audio_fifo_start = 0;
	LocalVector<float> resample_buffer;

	int channels = 0;
	int mix_rate = 0;
	int audio_track = 0;

	bool playing = false;
	bool paused = false;
	bool has_audio = false;

	bool video_ready = false;
	bool video_done = false;
	bool audio_done = false;
	bool demux_eof = false;
	bool video_flushed = false;
	bool audio_flushed = false;

	double time = 0;
	double length = 0;
	double frame_duration = 0;
	double next_frame_time = 0;
	double current_frame_time = -1;
	double delay_compensation = 0;
	double audio_skip_until = -1;

	static int _io_read(void *p_opaque, uint8_t *p_buf, int p_size);
	static int64_t _io_seek(void *p_opaque, int64_t p_offset, int p_whence);

	double get_time() const;
	double frame_timestamp(AVFrame *p_frame, AVStream *p_stream) const;
	bool demux_packet();
	int receive_decoded_frame(AVCodecContext *p_ctx, List<AVPacket *> &p_queue, bool &r_flushed, AVFrame *p_out);
	void queue_audio_frame(AVFrame *p_frame);
	void write_video_frame(AVFrame *p_frame);
	void flush_packet_queues();
	bool send_audio();

protected:
	void clear();

public:
	Error open_file(const String &p_file);

	virtual void play() override;
	virtual void stop() override;
	virtual bool is_playing() const override;

	virtual void set_paused(bool p_paused) override;
	virtual bool is_paused() const override;

	virtual double get_length() const override;

	virtual double get_playback_position() const override;
	virtual void seek(double p_time) override;

	virtual Ref<Texture2D> get_texture() const override;
	virtual void update(double p_delta) override;

	virtual int get_channels() const override;
	virtual int get_mix_rate() const override;

	virtual void set_audio_track(int p_idx) override;

	VideoStreamPlaybackMP4();
	~VideoStreamPlaybackMP4();
};

class VideoStreamMP4 : public VideoStream {
	GDCLASS(VideoStreamMP4, VideoStream);

protected:
	static void _bind_methods();

public:
	Ref<VideoStreamPlayback> instantiate_playback() override {
		Ref<VideoStreamPlaybackMP4> pb = memnew(VideoStreamPlaybackMP4);
		pb->set_audio_track(audio_track);
		if (pb->open_file(file) != OK) {
			return Ref<VideoStreamPlayback>();
		}
		return pb;
	}

	void set_audio_track(int p_track) override { audio_track = p_track; }

	VideoStreamMP4() { audio_track = 0; }
};

class ResourceFormatLoaderMP4 : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderMP4, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
