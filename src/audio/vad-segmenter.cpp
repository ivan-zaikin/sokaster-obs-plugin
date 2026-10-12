/*
sokaster — an AI co-host inside OBS Studio
Copyright (C) 2026 sokaster <tnixton@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "vad-segmenter.hpp"

#include <obs.h>
#include <plugin-support.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace sokaster {
namespace {

constexpr int kSampleRate = SileroVad::kSampleRate;
constexpr int kWindow = SileroVad::kFrameSize;
constexpr int kWindowMs = kWindow * 1000 / kSampleRate;

int samples_to_ms(size_t samples)
{
	return static_cast<int>(samples * 1000 / kSampleRate);
}

/* ISO-8601 UTC with milliseconds and a trailing Z. */
std::string iso8601_utc(int offset_ms)
{
	const auto now = std::chrono::system_clock::now() + std::chrono::milliseconds(offset_ms);
	const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
	const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();

	const std::time_t secs = std::chrono::system_clock::to_time_t(seconds);
	std::tm utc = {};
#ifdef _WIN32
	gmtime_s(&utc, &secs);
#else
	gmtime_r(&secs, &utc);
#endif

	/* strftime rather than one snprintf of seven ints: -Wformat-truncation
	 * does not know gmtime already bounded the fields. */
	char stamp[24];
	if (std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
		return {};

	char buffer[40];
	std::snprintf(buffer, sizeof(buffer), "%s.%03dZ", stamp, static_cast<int>(millis));
	return buffer;
}

/* Mono 16-bit PCM in a 44-byte RIFF header, written byte by byte: RIFF is
 * little-endian whatever the machine underneath. */
std::vector<uint8_t> encode_wav(const std::vector<float> &samples)
{
	const uint32_t data_size = static_cast<uint32_t>(samples.size()) * 2;
	std::vector<uint8_t> out;
	out.reserve(44 + data_size);

	const auto put = [&out](uint32_t value, int bytes) {
		for (int i = 0; i < bytes; ++i)
			out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
	};
	const auto tag = [&out](const char *name) {
		out.insert(out.end(), name, name + 4);
	};

	tag("RIFF");
	put(36 + data_size, 4);
	tag("WAVE");
	tag("fmt ");
	put(16, 4);              /* fmt chunk size */
	put(1, 2);               /* PCM */
	put(1, 2);               /* mono */
	put(kSampleRate, 4);     /* sample rate */
	put(kSampleRate * 2, 4); /* byte rate */
	put(2, 2);               /* block align */
	put(16, 2);              /* bits per sample */
	tag("data");
	put(data_size, 4);

	/* Hard clip: past unity is already distorted, wrapping would be noise. A
	 * NaN from a broken filter would survive clamp and convert undefined. */
	for (float sample : samples) {
		const float clipped = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
		put(static_cast<uint16_t>(static_cast<int16_t>(clipped * 32767.0f)), 2);
	}

	return out;
}

} // namespace

VadSegmenter::VadSegmenter(SileroVad &vad, SegmentHandler on_segment) : vad_(vad), on_segment_(std::move(on_segment))
{
	window_.reserve(kWindow);
	pre_roll_.assign(static_cast<size_t>(kSampleRate) * kPreRollMs / 1000, 0.0f);
	segment_.reserve(static_cast<size_t>(kSampleRate) * 4);
}

void VadSegmenter::push(const float *samples, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		const float sample = samples[i];

		pre_roll_[pre_roll_head_] = sample;
		pre_roll_head_ = (pre_roll_head_ + 1) % pre_roll_.size();
		pre_roll_filled_ = std::min(pre_roll_filled_ + 1, pre_roll_.size());

		if (speaking_)
			segment_.push_back(sample);

		window_.push_back(sample);
		if (window_.size() < static_cast<size_t>(kWindow))
			continue;

		float peak = 0.0f;
		for (float value : window_)
			peak = std::max(peak, std::fabs(value));

		if (peak >= kSilentWindowPeak)
			silent_windows_ = 0;
		else if (++silent_windows_ == kResetAfterSilentWindows)
			vad_.reset();

		const float probability = vad_.predict(window_.data());
		window_.clear();
		process_probability(probability);
	}
}

void VadSegmenter::process_probability(float probability)
{
	if (!speaking_) {
		if (probability < kEnterThreshold)
			consecutive_voice_ = 0;
		else if (++consecutive_voice_ >= kVoiceEnterWindows)
			start_segment();
		return;
	}

	if (probability >= kExitThreshold) {
		silence_ms_ = 0;
	} else if ((silence_ms_ += kWindowMs) >= kSilenceCloseMs) {
		close_segment("silence");
		return;
	}

	/* A monologue still reaches the transcriber in pieces, or the first word
	 * of an answer waits for the last word of the question. */
	if (samples_to_ms(segment_.size()) >= kMaxSegmentMs)
		close_segment("force-cut");
}

void VadSegmenter::start_segment()
{
	speaking_ = true;
	consecutive_voice_ = 0;
	silence_ms_ = 0;
	segment_.clear();

	/* The pre-roll, oldest first: the segment starts where it starts, not
	 * where the model finally agreed there was speech. */
	const size_t size = pre_roll_.size();
	const size_t start = (pre_roll_head_ + size - pre_roll_filled_) % size;
	for (size_t i = 0; i < pre_roll_filled_; ++i)
		segment_.push_back(pre_roll_[(start + i) % size]);

	segment_started_at_ = iso8601_utc(-kPreRollMs);
}

void VadSegmenter::close_segment(const char *reason)
{
	speaking_ = false;
	consecutive_voice_ = 0;
	silence_ms_ = 0;

	/* Whatever was said is over; its state only makes the next start harder
	 * to detect. */
	vad_.reset();

	/* A little silence on the end: transcribers eat the last consonant of a
	 * segment that stops dead. */
	segment_.insert(segment_.end(), static_cast<size_t>(kSampleRate) * kPostRollMs / 1000, 0.0f);
	const int duration_ms = samples_to_ms(segment_.size());

	if (duration_ms >= kMinSegmentMs) {
		obs_log(LOG_DEBUG, "vad: segment %d ms (%s)", duration_ms, reason);
		on_segment_(AudioSegment{encode_wav(segment_), duration_ms, segment_started_at_});
	}
	segment_.clear();
}

} // namespace sokaster
