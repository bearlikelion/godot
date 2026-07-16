/**************************************************************************/
/*  playback_mf.cpp                                                       */
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

#include "playback_mf.h"

#include "core/os/mutex.h"
#include "core/string/print_string.h"

#include <mferror.h>

// The 100ns units Media Foundation timestamps use.
static const double MF_TICKS_PER_SECOND = 10000000.0;

template <typename T>
static void mf_release(T *&p_com) {
	if (p_com) {
		p_com->Release();
		p_com = nullptr;
	}
}

/* Dynamically loaded Media Foundation entry points. */

struct MFApi {
	decltype(&MFStartup) startup = nullptr;
	decltype(&MFShutdown) shutdown = nullptr;
	decltype(&MFCreateMediaType) create_media_type = nullptr;
	decltype(&MFCreateAttributes) create_attributes = nullptr;
	decltype(&MFCreateAsyncResult) create_async_result = nullptr;
	decltype(&MFInvokeCallback) invoke_callback = nullptr;
	decltype(&MFCreateSourceReaderFromByteStream) create_source_reader_from_byte_stream = nullptr;
};

static MFApi *mf_api() {
	static Mutex mutex;
	static MFApi api;
	static bool attempted = false;
	static bool available = false;

	MutexLock lock(mutex);
	if (!attempted) {
		attempted = true;
		HMODULE mfplat = LoadLibraryW(L"mfplat.dll");
		HMODULE mfreadwrite = LoadLibraryW(L"mfreadwrite.dll");
		if (mfplat && mfreadwrite) {
			api.startup = (decltype(api.startup))(void *)GetProcAddress(mfplat, "MFStartup");
			api.shutdown = (decltype(api.shutdown))(void *)GetProcAddress(mfplat, "MFShutdown");
			api.create_media_type = (decltype(api.create_media_type))(void *)GetProcAddress(mfplat, "MFCreateMediaType");
			api.create_attributes = (decltype(api.create_attributes))(void *)GetProcAddress(mfplat, "MFCreateAttributes");
			api.create_async_result = (decltype(api.create_async_result))(void *)GetProcAddress(mfplat, "MFCreateAsyncResult");
			api.invoke_callback = (decltype(api.invoke_callback))(void *)GetProcAddress(mfplat, "MFInvokeCallback");
			api.create_source_reader_from_byte_stream = (decltype(api.create_source_reader_from_byte_stream))(void *)GetProcAddress(mfreadwrite, "MFCreateSourceReaderFromByteStream");
		}
		if (api.startup && api.shutdown && api.create_media_type && api.create_attributes &&
				api.create_async_result && api.invoke_callback && api.create_source_reader_from_byte_stream) {
			available = SUCCEEDED(api.startup(MF_VERSION, MFSTARTUP_LITE));
		}
		if (!available) {
			print_verbose("mp4: Media Foundation unavailable (Windows N without the Media Feature Pack?), MP4 playback disabled.");
		}
	}
	return available ? &api : nullptr;
}

/* Minimal COM object holding the byte count of a completed read. */

class MFReadCount : public IUnknown {
	LONG ref_count = 1;

public:
	const ULONG count;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
		if (riid == IID_IUnknown) {
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_count); }
	ULONG STDMETHODCALLTYPE Release() override {
		ULONG rc = InterlockedDecrement(&ref_count);
		if (rc == 0) {
			delete this;
		}
		return rc;
	}

	explicit MFReadCount(ULONG p_count) :
			count(p_count) {}
	virtual ~MFReadCount() {}
};

/* IMFByteStream over Godot's FileAccess, so videos inside PCKs work.
   Media Foundation may read from worker threads; a mutex serializes access. */

class GodotMFByteStream : public IMFByteStream {
	LONG ref_count = 1;
	Ref<FileAccess> file;
	Mutex mutex;

public:
	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
		if (riid == IID_IUnknown || riid == IID_IMFByteStream) {
			*ppv = static_cast<IMFByteStream *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_count); }
	ULONG STDMETHODCALLTYPE Release() override {
		ULONG rc = InterlockedDecrement(&ref_count);
		if (rc == 0) {
			delete this;
		}
		return rc;
	}

	// IMFByteStream
	HRESULT STDMETHODCALLTYPE GetCapabilities(DWORD *pdwCapabilities) override {
		*pdwCapabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetLength(QWORD *pqwLength) override {
		MutexLock lock(mutex);
		*pqwLength = file->get_length();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetLength(QWORD) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetCurrentPosition(QWORD *pqwPosition) override {
		MutexLock lock(mutex);
		*pqwPosition = file->get_position();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetCurrentPosition(QWORD qwPosition) override {
		MutexLock lock(mutex);
		if (qwPosition > file->get_length()) {
			return E_INVALIDARG;
		}
		file->seek(qwPosition);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE IsEndOfStream(BOOL *pfEndOfStream) override {
		MutexLock lock(mutex);
		*pfEndOfStream = file->get_position() >= file->get_length();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Read(BYTE *pb, ULONG cb, ULONG *pcbRead) override {
		MutexLock lock(mutex);
		*pcbRead = (ULONG)file->get_buffer(pb, cb);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE BeginRead(BYTE *pb, ULONG cb, IMFAsyncCallback *pCallback, IUnknown *punkState) override {
		if (!pb || !pCallback) {
			return E_INVALIDARG;
		}
		ULONG read = 0;
		Read(pb, cb, &read);
		MFApi *mf = mf_api();
		if (!mf) {
			return E_FAIL;
		}
		MFReadCount *read_count = new MFReadCount(read);
		IMFAsyncResult *result = nullptr;
		HRESULT hr = mf->create_async_result(read_count, pCallback, punkState, &result);
		read_count->Release();
		if (FAILED(hr)) {
			return hr;
		}
		hr = mf->invoke_callback(result);
		result->Release();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE EndRead(IMFAsyncResult *pResult, ULONG *pcbRead) override {
		if (!pResult || !pcbRead) {
			return E_INVALIDARG;
		}
		IUnknown *unk = nullptr;
		// Spelled GetObject so the windows.h macro (GetObjectA/W) matches however
		// the SDK headers declared the method.
		HRESULT hr = pResult->GetObject(&unk);
		if (FAILED(hr) || !unk) {
			return E_INVALIDARG;
		}
		*pcbRead = static_cast<MFReadCount *>(unk)->count;
		unk->Release();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Write(const BYTE *, ULONG, ULONG *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE BeginWrite(const BYTE *, ULONG, IMFAsyncCallback *, IUnknown *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE EndWrite(IMFAsyncResult *, ULONG *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset, DWORD, QWORD *pqwCurrentPosition) override {
		MutexLock lock(mutex);
		int64_t base = SeekOrigin == msoCurrent ? (int64_t)file->get_position() : 0;
		int64_t pos = base + llSeekOffset;
		if (pos < 0 || pos > (int64_t)file->get_length()) {
			return E_INVALIDARG;
		}
		file->seek(pos);
		if (pqwCurrentPosition) {
			*pqwCurrentPosition = pos;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Flush() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

	explicit GodotMFByteStream(Ref<FileAccess> p_file) :
			file(p_file) {}
	virtual ~GodotMFByteStream() {}
};

/* VideoStreamPlaybackMF */

double VideoStreamPlaybackMF::sample_timestamp(IMFSample *p_sample) const {
	LONGLONG t = 0;
	if (FAILED(p_sample->GetSampleTime(&t))) {
		return -1;
	}
	return t / MF_TICKS_PER_SECOND;
}

// Re-reads frame size/stride/rate; also called when the reader signals a format change.
void VideoStreamPlaybackMF::refresh_video_format() {
	IMFMediaType *type = nullptr;
	if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) {
		return;
	}
	UINT32 w = 0, h = 0;
	MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
	frame_stride = (int32_t)MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, 0);
	UINT32 fps_num = 0, fps_den = 0;
	MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &fps_num, &fps_den);
	if (fps_num > 0 && fps_den > 0) {
		frame_duration = (double)fps_den / fps_num;
	} else if (frame_duration <= 0) {
		frame_duration = 1.0 / 30.0;
	}
	type->Release();

	if (w > 0 && h > 0 && ((int)w != size.x || (int)h != size.y)) {
		size = Point2i(w, h);
		setup_texture();
	}
}

void VideoStreamPlaybackMF::queue_audio_sample(IMFSample *p_sample) {
	IMFMediaBuffer *buffer = nullptr;
	if (FAILED(p_sample->ConvertToContiguousBuffer(&buffer))) {
		return;
	}
	BYTE *data = nullptr;
	DWORD len = 0;
	if (FAILED(buffer->Lock(&data, nullptr, &len))) {
		buffer->Release();
		return;
	}
	int out_samples = len / (sizeof(float) * channels);
	const float *samples = (const float *)data;

	int skip = 0;
	if (audio_skip_until >= 0 && out_samples > 0) {
		double t = sample_timestamp(p_sample);
		if (t >= 0) {
			double end = t + (double)out_samples / mix_rate;
			if (end <= audio_skip_until) {
				buffer->Unlock();
				buffer->Release();
				return;
			}
			if (t < audio_skip_until) {
				skip = MIN((int)((audio_skip_until - t) * mix_rate), out_samples - 1);
			}
		}
		audio_skip_until = -1;
	}

	if (out_samples > skip) {
		uint32_t base = audio_fifo.size();
		uint32_t count = (uint32_t)(out_samples - skip) * channels;
		audio_fifo.resize(base + count);
		memcpy(audio_fifo.ptr() + base, samples + (uint32_t)skip * channels, count * sizeof(float));
	}

	buffer->Unlock();
	buffer->Release();
}

// Copies an RGB32 (BGRX) sample into frame_data as RGBA and uploads it.
void VideoStreamPlaybackMF::write_video_sample(IMFSample *p_sample) {
	IMFMediaBuffer *buffer = nullptr;
	if (FAILED(p_sample->ConvertToContiguousBuffer(&buffer))) {
		return;
	}
	BYTE *data = nullptr;
	DWORD len = 0;
	if (FAILED(buffer->Lock(&data, nullptr, &len))) {
		buffer->Release();
		return;
	}

	int32_t stride = frame_stride != 0 ? frame_stride : size.x * 4;
	// A negative stride means the frame is stored bottom-up.
	bool bottom_up = stride < 0;
	uint32_t abs_stride = bottom_up ? -stride : stride;

	if ((uint64_t)abs_stride * size.y <= len) {
		uint8_t *dst = frame_data.ptrw();
		for (int y = 0; y < size.y; y++) {
			const uint8_t *src = data + (uint64_t)(bottom_up ? size.y - 1 - y : y) * abs_stride;
			for (int x = 0; x < size.x; x++) {
				dst[0] = src[2];
				dst[1] = src[1];
				dst[2] = src[0];
				dst[3] = 255;
				dst += 4;
				src += 4;
			}
		}
		present_rgba_frame();
	}

	buffer->Unlock();
	buffer->Release();
}

Error VideoStreamPlaybackMF::open_file(const String &p_file) {
	clear();

	MFApi *mf = mf_api();
	ERR_FAIL_NULL_V_MSG(mf, ERR_UNAVAILABLE, "Media Foundation not available, can't play MP4 videos.");

	file = FileAccess::open(p_file, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_OPEN, vformat("Couldn't open video file '%s'.", p_file));
	file_name = p_file;

	byte_stream = new GodotMFByteStream(file);

	IMFAttributes *attrs = nullptr;
	mf->create_attributes(&attrs, 1);
	if (attrs) {
		attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
	}
	HRESULT hr = mf->create_source_reader_from_byte_stream(byte_stream, attrs, &reader);
	mf_release(attrs);
	if (FAILED(hr)) {
		clear();
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, vformat("Media Foundation couldn't open '%s'.", p_file));
	}

	reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

	// Ask for BGRX output; the reader inserts the decoder and converter.
	IMFMediaType *video_type = nullptr;
	mf->create_media_type(&video_type);
	video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	video_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, video_type);
	video_type->Release();
	if (FAILED(hr)) {
		clear();
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("No decodable video stream in '%s'.", p_file));
	}
	refresh_video_format();
	if (size.x <= 0 || size.y <= 0) {
		clear();
		ERR_FAIL_V_MSG(ERR_INVALID_DATA, vformat("Invalid video dimensions in '%s'.", p_file));
	}

	// Audio: float PCM, downmixed to stereo when the reader supports it.
	if (SUCCEEDED(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
		IMFMediaType *audio_type = nullptr;
		mf->create_media_type(&audio_type);
		audio_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		audio_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
		audio_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audio_type);
		if (FAILED(hr)) {
			// Retry letting the reader keep the source channel layout.
			audio_type->DeleteItem(MF_MT_AUDIO_NUM_CHANNELS);
			hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audio_type);
		}
		audio_type->Release();
		if (SUCCEEDED(hr) && SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &audio_type))) {
			channels = MFGetAttributeUINT32(audio_type, MF_MT_AUDIO_NUM_CHANNELS, 0);
			mix_rate = MFGetAttributeUINT32(audio_type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
			audio_type->Release();
			has_audio = channels > 0 && channels <= 8 && mix_rate > 0;
		}
		if (!has_audio) {
			WARN_PRINT(vformat("Couldn't decode audio in '%s', playing without audio.", p_file));
			reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
			channels = 0;
			mix_rate = 0;
		}
	}

	PROPVARIANT var;
	memset(&var, 0, sizeof(var));
	if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
		if (var.vt == VT_UI8) {
			length = var.uhVal.QuadPart / MF_TICKS_PER_SECOND;
		}
		PropVariantClear(&var);
	}

	seek(0);

	return OK;
}

void VideoStreamPlaybackMF::update(double p_delta) {
	if (!reader || !playing || paused) {
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
			DWORD flags = 0;
			LONGLONG ts = 0;
			IMFSample *sample = nullptr;
			HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &sample);
			if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
				mf_release(sample);
				audio_done = true;
				break;
			}
			if (sample) {
				queue_audio_sample(sample);
				sample->Release();
			}
		}

		while (!video_ready && !video_done) {
			DWORD flags = 0;
			LONGLONG ts = 0;
			IMFSample *sample = nullptr;
			HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
			if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
				mf_release(sample);
				video_done = true;
				break;
			}
			if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
				refresh_video_format();
			}
			if (!sample) {
				continue;
			}
			double t = sample_timestamp(sample);
			if (t < 0) {
				t = next_frame_time + frame_duration;
			}
			if (t > comp_time) {
				mf_release(pending_sample);
				pending_sample = sample;
				next_frame_time = t;
				video_ready = true;
			} else {
				// Too late to display, drop it and catch up.
				sample->Release();
			}
		}
	}

	if (!video_ready && video_done && audio_done) {
		stop();
		return;
	}

	// Wait for the last frame to end before rendering the next one.
	if (video_ready && comp_time >= current_frame_time) {
		write_video_sample(pending_sample);
		mf_release(pending_sample);
		video_ready = false;
		current_frame_time = next_frame_time;
	}
}

void VideoStreamPlaybackMF::seek(double p_time) {
	if (!reader) {
		return;
	}
	if (length > 0 && p_time >= length) {
		return;
	}
	p_time = MAX(p_time, 0.0);

	PROPVARIANT var;
	memset(&var, 0, sizeof(var));
	var.vt = VT_I8;
	var.hVal.QuadPart = (LONGLONG)(p_time * MF_TICKS_PER_SECOND);
	reader->SetCurrentPosition(GUID_NULL, var);

	mf_release(pending_sample);
	audio_fifo.clear();
	audio_fifo_start = 0;

	video_ready = false;
	video_done = false;
	audio_done = !has_audio;
	next_frame_time = 0;
	current_frame_time = -1;

	time = p_time;
	audio_skip_until = p_time;
}

void VideoStreamPlaybackMF::clear() {
	mf_release(pending_sample);
	mf_release(reader);
	if (byte_stream) {
		byte_stream->Release();
		byte_stream = nullptr;
	}
	file.unref();

	audio_fifo.clear();
	audio_fifo_start = 0;
	channels = 0;
	mix_rate = 0;
	has_audio = false;
	playing = false;
	paused = false;
	video_ready = false;
	video_done = false;
	audio_done = false;
	time = 0;
	length = 0;
	frame_stride = 0;
	next_frame_time = 0;
	current_frame_time = -1;
	audio_skip_until = -1;
}

VideoStreamPlaybackMF::VideoStreamPlaybackMF() {
}

VideoStreamPlaybackMF::~VideoStreamPlaybackMF() {
	clear();
}
