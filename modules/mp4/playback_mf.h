/**************************************************************************/
/*  playback_mf.h                                                         */
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

#include "mp4_playback.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

class GodotMFByteStream;

// MP4 playback decoded by Windows Media Foundation. The OS ships the H.264/
// H.265/AAC decoders, so nothing codec-related is compiled into the engine.
// mfplat.dll/mfreadwrite.dll are loaded dynamically so Windows N editions
// (no Media Feature Pack) still boot; MP4 playback just reports unavailable.
class VideoStreamPlaybackMF : public VideoStreamPlaybackMP4 {
	GDCLASS(VideoStreamPlaybackMF, VideoStreamPlaybackMP4);

	GodotMFByteStream *byte_stream = nullptr;
	IMFSourceReader *reader = nullptr;

	IMFSample *pending_sample = nullptr;

	int32_t frame_stride = 0;

	bool audio_eos = false;
	bool video_eos = false;

	double sample_timestamp(IMFSample *p_sample) const;
	void refresh_video_format();
	void queue_audio_sample(IMFSample *p_sample);
	void write_video_sample(IMFSample *p_sample);

protected:
	void clear();

public:
	virtual Error open_file(const String &p_file) override;

	virtual void seek(double p_time) override;
	virtual void update(double p_delta) override;

	VideoStreamPlaybackMF();
	~VideoStreamPlaybackMF();
};
