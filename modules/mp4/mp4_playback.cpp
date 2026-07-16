/**************************************************************************/
/*  mp4_playback.cpp                                                      */
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

#include "mp4_playback.h"

#include "core/config/project_settings.h"
#include "scene/resources/image_texture.h"

double VideoStreamPlaybackMP4::get_time() const {
	return time - delay_compensation;
}

bool VideoStreamPlaybackMP4::send_audio() {
	uint32_t pending = audio_fifo.size() - audio_fifo_start;
	if (pending > 0) {
		if (!mix_callback) {
			audio_fifo.clear();
			audio_fifo_start = 0;
			return true;
		}
		int mixed = mix_callback(mix_udata, audio_fifo.ptr() + audio_fifo_start, pending / channels);
		audio_fifo_start += (uint32_t)mixed * channels;
		if (audio_fifo_start == audio_fifo.size()) {
			audio_fifo.clear();
			audio_fifo_start = 0;
		} else {
			return false;
		}
	}
	return true;
}

// Creates the initial black frame texture once the video size is known.
void VideoStreamPlaybackMP4::setup_texture() {
	frame_data.resize(size.x * size.y * 4);
	memset(frame_data.ptrw(), 0, frame_data.size());
	Ref<Image> img = Image::create_from_data(size.x, size.y, false, Image::FORMAT_RGBA8, frame_data);
	texture->set_image(img);
}

// Uploads the current contents of frame_data to the texture.
void VideoStreamPlaybackMP4::present_rgba_frame() {
	Ref<Image> img = Image::create_from_data(size.x, size.y, false, Image::FORMAT_RGBA8, frame_data);
	if (img.is_valid()) {
		texture->update(img);
	}
}

void VideoStreamPlaybackMP4::play() {
	if (playing) {
		return;
	}
	playing = true;
	delay_compensation = GLOBAL_GET("audio/video/video_delay_compensation_ms");
	delay_compensation /= 1000.0;
}

void VideoStreamPlaybackMP4::stop() {
	playing = false;
	seek(0);
}

bool VideoStreamPlaybackMP4::is_playing() const {
	return playing;
}

void VideoStreamPlaybackMP4::set_paused(bool p_paused) {
	paused = p_paused;
}

bool VideoStreamPlaybackMP4::is_paused() const {
	return paused;
}

double VideoStreamPlaybackMP4::get_length() const {
	return length;
}

double VideoStreamPlaybackMP4::get_playback_position() const {
	return get_time();
}

Ref<Texture2D> VideoStreamPlaybackMP4::get_texture() const {
	return texture;
}

int VideoStreamPlaybackMP4::get_channels() const {
	return channels;
}

int VideoStreamPlaybackMP4::get_mix_rate() const {
	return mix_rate;
}

void VideoStreamPlaybackMP4::set_audio_track(int p_idx) {
	audio_track = p_idx;
}

VideoStreamPlaybackMP4::VideoStreamPlaybackMP4() {
	texture.instantiate();
}
