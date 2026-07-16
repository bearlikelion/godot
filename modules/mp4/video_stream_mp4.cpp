/**************************************************************************/
/*  video_stream_mp4.cpp                                                  */
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

#include "video_stream_mp4.h"

#include "core/object/class_db.h"

#if defined(LINUXBSD_ENABLED)
#include "playback_ffmpeg.h"
#elif defined(WINDOWS_ENABLED)
#include "playback_mf.h"
#endif

void VideoStreamMP4::_bind_methods() {}

Ref<VideoStreamPlayback> VideoStreamMP4::instantiate_playback() {
	Ref<VideoStreamPlaybackMP4> pb;
#if defined(LINUXBSD_ENABLED)
	pb = Ref<VideoStreamPlaybackMP4>(memnew(VideoStreamPlaybackFFmpeg));
#elif defined(WINDOWS_ENABLED)
	pb = Ref<VideoStreamPlaybackMP4>(memnew(VideoStreamPlaybackMF));
#else
	return Ref<VideoStreamPlayback>();
#endif
	pb->set_audio_track(audio_track);
	if (pb->open_file(file) != OK) {
		return Ref<VideoStreamPlayback>();
	}
	return pb;
}

Ref<Resource> ResourceFormatLoaderMP4::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		if (r_error) {
			*r_error = ERR_CANT_OPEN;
		}
		return Ref<Resource>();
	}

	Ref<VideoStreamMP4> stream;
	stream.instantiate();
	stream->set_file(p_path);

	if (r_error) {
		*r_error = OK;
	}

	return stream;
}

void ResourceFormatLoaderMP4::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("mp4");
	p_extensions->push_back("m4v");
	p_extensions->push_back("mov");
}

bool ResourceFormatLoaderMP4::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "VideoStream");
}

String ResourceFormatLoaderMP4::get_resource_type(const String &p_path) const {
	if (p_path.has_extension("mp4") || p_path.has_extension("m4v") || p_path.has_extension("mov")) {
		return "VideoStreamMP4";
	}
	return "";
}
