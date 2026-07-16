/**************************************************************************/
/*  playback_ffmpeg.cpp                                                   */
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

#include "playback_ffmpeg.h"

static const int MP4_IO_BUFFER_SIZE = 64 * 1024;

int VideoStreamPlaybackFFmpeg::_io_read(void *p_opaque, uint8_t *p_buf, int p_size) {
	VideoStreamPlaybackFFmpeg *playback = static_cast<VideoStreamPlaybackFFmpeg *>(p_opaque);
	uint64_t read = playback->file->get_buffer(p_buf, p_size);
	if (read == 0) {
		return AVERROR_EOF;
	}
	return (int)read;
}

int64_t VideoStreamPlaybackFFmpeg::_io_seek(void *p_opaque, int64_t p_offset, int p_whence) {
	VideoStreamPlaybackFFmpeg *playback = static_cast<VideoStreamPlaybackFFmpeg *>(p_opaque);
	int64_t len = (int64_t)playback->file->get_length();
	if (p_whence == AVSEEK_SIZE) {
		return len;
	}
	int64_t pos = -1;
	switch (p_whence & ~AVSEEK_FORCE) {
		case SEEK_SET: {
			pos = p_offset;
		} break;
		case SEEK_CUR: {
			pos = (int64_t)playback->file->get_position() + p_offset;
		} break;
		case SEEK_END: {
			pos = len + p_offset;
		} break;
		default: {
			return -1;
		}
	}
	if (pos < 0 || pos > len) {
		return -1;
	}
	playback->file->seek(pos);
	return pos;
}

double VideoStreamPlaybackFFmpeg::frame_timestamp(AVFrame *p_frame, AVStream *p_stream) const {
	int64_t ts = p_frame->best_effort_timestamp;
	if (ts == AV_NOPTS_VALUE) {
		ts = p_frame->pts;
	}
	if (ts == AV_NOPTS_VALUE) {
		return -1;
	}
	int64_t start = p_stream->start_time != AV_NOPTS_VALUE ? p_stream->start_time : 0;
	return (ts - start) * av_q2d(p_stream->time_base);
}

// Reads one packet from the container into the queue of the stream it belongs to.
bool VideoStreamPlaybackFFmpeg::demux_packet() {
	while (true) {
		int ret = ff->av_read_frame(format_ctx, packet);
		if (ret < 0) {
			return false;
		}
		if (packet->stream_index == video_stream_idx) {
			video_packets.push_back(ff->av_packet_clone(packet));
			ff->av_packet_unref(packet);
			return true;
		}
		if (has_audio && packet->stream_index == audio_stream_idx) {
			audio_packets.push_back(ff->av_packet_clone(packet));
			ff->av_packet_unref(packet);
			return true;
		}
		ff->av_packet_unref(packet);
	}
}

// Blocks until a frame is decoded or the stream ends. Returns 0 on frame, AVERROR_EOF otherwise.
int VideoStreamPlaybackFFmpeg::receive_decoded_frame(AVCodecContext *p_ctx, List<AVPacket *> &p_queue, bool &r_flushed, AVFrame *p_out) {
	while (true) {
		int ret = ff->avcodec_receive_frame(p_ctx, p_out);
		if (ret == 0) {
			return 0;
		}
		if (ret != AVERROR(EAGAIN)) {
			return AVERROR_EOF;
		}
		// Decoder needs input.
		if (!p_queue.is_empty()) {
			AVPacket *pkt = p_queue.front()->get();
			p_queue.pop_front();
			ff->avcodec_send_packet(p_ctx, pkt);
			ff->av_packet_free(&pkt);
		} else if (demux_eof) {
			if (r_flushed) {
				return AVERROR_EOF;
			}
			ff->avcodec_send_packet(p_ctx, nullptr);
			r_flushed = true;
		} else {
			if (!demux_packet()) {
				demux_eof = true;
			}
		}
	}
}

void VideoStreamPlaybackFFmpeg::queue_audio_frame(AVFrame *p_frame) {
	int max_out_samples = ff->swr_get_out_samples(swr_ctx, p_frame->nb_samples);
	if (max_out_samples <= 0) {
		return;
	}
	resample_buffer.resize((uint32_t)max_out_samples * channels);
	uint8_t *out[1] = { (uint8_t *)resample_buffer.ptr() };
	int out_samples = ff->swr_convert(swr_ctx, out, max_out_samples, (const uint8_t **)p_frame->extended_data, p_frame->nb_samples);
	if (out_samples <= 0) {
		return;
	}

	int skip = 0;
	if (audio_skip_until >= 0) {
		double t = frame_timestamp(p_frame, format_ctx->streams[audio_stream_idx]);
		if (t >= 0) {
			double end = t + (double)out_samples / mix_rate;
			if (end <= audio_skip_until) {
				return;
			}
			if (t < audio_skip_until) {
				skip = MIN((int)((audio_skip_until - t) * mix_rate), out_samples - 1);
			}
		}
		audio_skip_until = -1;
	}

	uint32_t base = audio_fifo.size();
	uint32_t count = (uint32_t)(out_samples - skip) * channels;
	audio_fifo.resize(base + count);
	memcpy(audio_fifo.ptr() + base, resample_buffer.ptr() + (uint32_t)skip * channels, count * sizeof(float));
}

void VideoStreamPlaybackFFmpeg::write_video_frame(AVFrame *p_frame) {
	sws_ctx = ff->sws_getCachedContext(sws_ctx, p_frame->width, p_frame->height, (AVPixelFormat)p_frame->format,
			size.x, size.y, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws_ctx) {
		return;
	}
	uint8_t *dst[4] = { frame_data.ptrw(), nullptr, nullptr, nullptr };
	int dst_stride[4] = { size.x * 4, 0, 0, 0 };
	ff->sws_scale(sws_ctx, p_frame->data, p_frame->linesize, 0, p_frame->height, dst, dst_stride);
	present_rgba_frame();
}

void VideoStreamPlaybackFFmpeg::flush_packet_queues() {
	for (AVPacket *pkt : video_packets) {
		ff->av_packet_free(&pkt);
	}
	video_packets.clear();
	for (AVPacket *pkt : audio_packets) {
		ff->av_packet_free(&pkt);
	}
	audio_packets.clear();
}

Error VideoStreamPlaybackFFmpeg::open_file(const String &p_file) {
	clear();

	ff = ffmpeg_symbols();
	ERR_FAIL_NULL_V_MSG(ff, ERR_UNAVAILABLE, "System FFmpeg libraries not found, can't play MP4 videos.");

	file = FileAccess::open(p_file, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_OPEN, vformat("Couldn't open video file '%s'.", p_file));
	file_name = p_file;

	uint8_t *io_buffer = (uint8_t *)ff->av_malloc(MP4_IO_BUFFER_SIZE);
	avio_ctx = ff->avio_alloc_context(io_buffer, MP4_IO_BUFFER_SIZE, 0, this, _io_read, nullptr, _io_seek);
	if (!avio_ctx) {
		ff->av_free(io_buffer);
		clear();
		ERR_FAIL_V_MSG(ERR_OUT_OF_MEMORY, "Couldn't allocate AVIO context.");
	}

	format_ctx = ff->avformat_alloc_context();
	format_ctx->pb = avio_ctx;
	int ret = ff->avformat_open_input(&format_ctx, file_name.utf8().get_data(), nullptr, nullptr);
	if (ret < 0) {
		clear();
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, vformat("FFmpeg couldn't open '%s'.", p_file));
	}
	ret = ff->avformat_find_stream_info(format_ctx, nullptr);
	if (ret < 0) {
		clear();
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, vformat("Couldn't find stream info in '%s'.", p_file));
	}

	video_stream_idx = ff->av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (video_stream_idx < 0) {
		clear();
		ERR_FAIL_V_MSG(ERR_INVALID_DATA, vformat("No video stream found in '%s'.", p_file));
	}

	// Pick the requested audio track, falling back to the first one.
	audio_stream_idx = -1;
	int audio_count = 0;
	for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
		if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (audio_stream_idx == -1 || audio_count == audio_track) {
				audio_stream_idx = i;
			}
			audio_count++;
		}
	}

	AVStream *video_stream = format_ctx->streams[video_stream_idx];
	const AVCodec *video_codec = ff->avcodec_find_decoder(video_stream->codecpar->codec_id);
	if (!video_codec) {
		clear();
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("No decoder available for video codec in '%s'.", p_file));
	}
	video_ctx = ff->avcodec_alloc_context3(video_codec);
	ff->avcodec_parameters_to_context(video_ctx, video_stream->codecpar);
	video_ctx->thread_count = 0;
	if (ff->avcodec_open2(video_ctx, video_codec, nullptr) < 0) {
		clear();
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("Couldn't open video decoder for '%s'.", p_file));
	}

	size = Point2i(video_stream->codecpar->width, video_stream->codecpar->height);
	if (size.x <= 0 || size.y <= 0) {
		clear();
		ERR_FAIL_V_MSG(ERR_INVALID_DATA, vformat("Invalid video dimensions in '%s'.", p_file));
	}

	if (audio_stream_idx >= 0) {
		AVStream *audio_stream = format_ctx->streams[audio_stream_idx];
		const AVCodec *audio_codec = ff->avcodec_find_decoder(audio_stream->codecpar->codec_id);
		if (audio_codec) {
			audio_ctx = ff->avcodec_alloc_context3(audio_codec);
			ff->avcodec_parameters_to_context(audio_ctx, audio_stream->codecpar);
			if (ff->avcodec_open2(audio_ctx, audio_codec, nullptr) == 0) {
				channels = audio_ctx->ch_layout.nb_channels >= 2 ? 2 : 1;
				mix_rate = audio_ctx->sample_rate;
				AVChannelLayout out_layout;
				ff->av_channel_layout_default(&out_layout, channels);
				ret = ff->swr_alloc_set_opts2(&swr_ctx, &out_layout, AV_SAMPLE_FMT_FLT, mix_rate,
						&audio_ctx->ch_layout, audio_ctx->sample_fmt, audio_ctx->sample_rate, 0, nullptr);
				ff->av_channel_layout_uninit(&out_layout);
				if (ret == 0 && ff->swr_init(swr_ctx) == 0) {
					has_audio = true;
				}
			}
		}
		if (!has_audio) {
			WARN_PRINT(vformat("Couldn't open audio decoder for '%s', playing without audio.", p_file));
			if (audio_ctx) {
				ff->avcodec_free_context(&audio_ctx);
			}
			if (swr_ctx) {
				ff->swr_free(&swr_ctx);
			}
			channels = 0;
			mix_rate = 0;
			audio_stream_idx = -1;
		}
	}

	frame = ff->av_frame_alloc();
	audio_frame = ff->av_frame_alloc();
	pending_frame = ff->av_frame_alloc();
	packet = ff->av_packet_alloc();

	setup_texture();

	if (format_ctx->duration != AV_NOPTS_VALUE) {
		length = format_ctx->duration / (double)AV_TIME_BASE;
	} else if (video_stream->duration != AV_NOPTS_VALUE) {
		length = video_stream->duration * av_q2d(video_stream->time_base);
	}

	if (video_stream->avg_frame_rate.num > 0 && video_stream->avg_frame_rate.den > 0) {
		frame_duration = av_q2d(av_inv_q(video_stream->avg_frame_rate));
	} else {
		frame_duration = 1.0 / 30.0;
	}

	seek(0);

	return OK;
}

void VideoStreamPlaybackFFmpeg::update(double p_delta) {
	if (!format_ctx || !playing || paused) {
		return;
	}

	time += p_delta;

	double comp_time = get_time();
	bool audio_ready = false;

	// Decode until the mixer buffer is full and the next video frame is ready.
	while ((!audio_ready && !audio_done) || (!video_ready && !video_done)) {
		while (!audio_ready && !audio_done) {
			if (!send_audio()) {
				audio_ready = true;
				break;
			}
			int ret = receive_decoded_frame(audio_ctx, audio_packets, audio_flushed, audio_frame);
			if (ret == 0) {
				queue_audio_frame(audio_frame);
				ff->av_frame_unref(audio_frame);
			} else {
				audio_done = true;
			}
		}

		while (!video_ready && !video_done) {
			int ret = receive_decoded_frame(video_ctx, video_packets, video_flushed, frame);
			if (ret != 0) {
				video_done = true;
				break;
			}
			double t = frame_timestamp(frame, format_ctx->streams[video_stream_idx]);
			if (t < 0) {
				t = next_frame_time + frame_duration;
			}
			if (t > comp_time) {
				ff->av_frame_unref(pending_frame);
				ff->av_frame_move_ref(pending_frame, frame);
				next_frame_time = t;
				video_ready = true;
			} else {
				// Too late to display, drop it and catch up.
				ff->av_frame_unref(frame);
			}
		}
	}

	if (!video_ready && video_done && audio_done) {
		stop();
		return;
	}

	// Wait for the last frame to end before rendering the next one.
	if (video_ready && comp_time >= current_frame_time) {
		write_video_frame(pending_frame);
		ff->av_frame_unref(pending_frame);
		video_ready = false;
		current_frame_time = next_frame_time;
	}
}

void VideoStreamPlaybackFFmpeg::seek(double p_time) {
	if (!format_ctx) {
		return;
	}
	if (length > 0 && p_time >= length) {
		return;
	}
	p_time = MAX(p_time, 0.0);

	ff->avcodec_flush_buffers(video_ctx);
	if (has_audio) {
		ff->avcodec_flush_buffers(audio_ctx);
	}
	flush_packet_queues();
	audio_fifo.clear();
	audio_fifo_start = 0;
	ff->av_frame_unref(pending_frame);

	video_ready = false;
	video_done = false;
	audio_done = !has_audio;
	demux_eof = false;
	video_flushed = false;
	audio_flushed = false;
	next_frame_time = 0;
	current_frame_time = -1;

	int64_t ts = (int64_t)(p_time * AV_TIME_BASE);
	if (format_ctx->start_time != AV_NOPTS_VALUE) {
		ts += format_ctx->start_time;
	}
	ff->av_seek_frame(format_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);

	time = p_time;
	audio_skip_until = p_time;
}

void VideoStreamPlaybackFFmpeg::clear() {
	if (ff) {
		flush_packet_queues();
		if (packet) {
			ff->av_packet_free(&packet);
		}
		if (frame) {
			ff->av_frame_free(&frame);
		}
		if (audio_frame) {
			ff->av_frame_free(&audio_frame);
		}
		if (pending_frame) {
			ff->av_frame_free(&pending_frame);
		}
		if (sws_ctx) {
			ff->sws_freeContext(sws_ctx);
			sws_ctx = nullptr;
		}
		if (swr_ctx) {
			ff->swr_free(&swr_ctx);
		}
		if (video_ctx) {
			ff->avcodec_free_context(&video_ctx);
		}
		if (audio_ctx) {
			ff->avcodec_free_context(&audio_ctx);
		}
		if (format_ctx) {
			ff->avformat_close_input(&format_ctx);
		}
		if (avio_ctx) {
			ff->av_free(avio_ctx->buffer);
			ff->avio_context_free(&avio_ctx);
		}
	}
	file.unref();

	audio_fifo.clear();
	audio_fifo_start = 0;
	video_stream_idx = -1;
	audio_stream_idx = -1;
	channels = 0;
	mix_rate = 0;
	has_audio = false;
	playing = false;
	paused = false;
	video_ready = false;
	video_done = false;
	audio_done = false;
	demux_eof = false;
	video_flushed = false;
	audio_flushed = false;
	time = 0;
	length = 0;
	next_frame_time = 0;
	current_frame_time = -1;
	audio_skip_until = -1;
}

VideoStreamPlaybackFFmpeg::VideoStreamPlaybackFFmpeg() {
}

VideoStreamPlaybackFFmpeg::~VideoStreamPlaybackFFmpeg() {
	clear();
}
