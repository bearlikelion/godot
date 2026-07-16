/**************************************************************************/
/*  mp4_playback.h                                                        */
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
#include "core/templates/local_vector.h"
#include "scene/resources/video_stream.h"

class ImageTexture;

// Backend-independent part of MP4 playback: frame texture, decoded-audio
// FIFO with mixer backpressure, and the play/pause/track bookkeeping.
// Backends implement open_file/update/seek on top of it.
class VideoStreamPlaybackMP4 : public VideoStreamPlayback {
	GDCLASS(VideoStreamPlaybackMP4, VideoStreamPlayback);

protected:
	Ref<FileAccess> file;
	String file_name;

	Point2i size;
	Vector<uint8_t> frame_data;
	Ref<ImageTexture> texture;

	// Interleaved float samples waiting to be accepted by the mixer.
	LocalVector<float> audio_fifo;
	uint32_t audio_fifo_start = 0;

	int channels = 0;
	int mix_rate = 0;
	int audio_track = 0;

	bool playing = false;
	bool paused = false;
	bool has_audio = false;

	bool video_ready = false;
	bool video_done = false;
	bool audio_done = false;

	double time = 0;
	double length = 0;
	double frame_duration = 0;
	double next_frame_time = 0;
	double current_frame_time = -1;
	double delay_compensation = 0;
	double audio_skip_until = -1;

	double get_time() const;
	bool send_audio();
	void setup_texture();
	void present_rgba_frame();

public:
	virtual Error open_file(const String &p_file) = 0;

	virtual void play() override;
	virtual void stop() override;
	virtual bool is_playing() const override;

	virtual void set_paused(bool p_paused) override;
	virtual bool is_paused() const override;

	virtual double get_length() const override;
	virtual double get_playback_position() const override;

	virtual Ref<Texture2D> get_texture() const override;

	virtual int get_channels() const override;
	virtual int get_mix_rate() const override;

	virtual void set_audio_track(int p_idx) override;

	VideoStreamPlaybackMP4();
};
