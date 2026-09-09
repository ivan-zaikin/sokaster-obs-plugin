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

#include "wav-encoder.hpp"

#include <obs.h>
#include <plugin-support.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace sokaster {
namespace {

/* ISO-8601 UTC, milliseconds, trailing Z — what the backend's DateTime parser
 * has been fed since the desktop client. */
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

	/* The calendar part goes through strftime rather than one snprintf of
	 * seven ints: -Werror=format-truncation reasons about the whole int range
	 * and does not know gmtime already bounded these fields. */
	char stamp[24];
	if (std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
		return {};

	char buffer[40];
	std::snprintf(buffer, sizeof(buffer), "%s.%03dZ", stamp, static_cast<int>(millis));
	return buffer;
}

int samples_to_ms(size_t samples)
{
	return static_cast<int>((samples * 1000) / VadSegmenter::kSampleRate);
}

} // namespace

VadSegmenter::VadSegmenter(SileroVad &vad, SegmentHandler on_segment) : vad_(vad), on_segment_(std::move(on_segment))
{
	window_.reserve(kVadFrameSamples);
	pre_roll_.assign(static_cast<size_t>(kSampleRate) * kPreRollMs / 1000, 0.0f);
	segment_.reserve(static_cast<size_t>(kSampleRate) * 4);
}

void VadSegmenter::push(const float *samples, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		const float sample = samples[i];

		/* The pre-roll ring is written unconditionally, so that when speech
		 * does start we already have the 200 ms before it. */
		pre_roll_[pre_roll_head_] = sample;
		pre_roll_head_ = (pre_roll_head_ + 1) % pre_roll_.size();
		if (pre_roll_filled_ < pre_roll_.size())
			++pre_roll_filled_;

		if (speaking_)
			segment_.push_back(sample);

		window_.push_back(sample);
		if (window_.size() < static_cast<size_t>(kVadFrameSamples))
			continue;

		float peak = 0.0f;
		for (float value : window_) {
			const float magnitude = std::fabs(value);
			if (magnitude > peak)
				peak = magnitude;
		}

		if (peak < kSilentWindowPeak) {
			if (++silent_windows_ == kResetAfterSilentWindows)
				vad_.reset();
		} else {
			silent_windows_ = 0;
		}

		const float probability = vad_.predict(window_.data());
		window_.clear();

		process_probability(probability);
	}
}

void VadSegmenter::process_probability(float probability)
{
	if (!speaking_) {
		if (probability >= kEnterThreshold) {
			if (++consecutive_voice_ >= kVoiceEnterFrames)
				start_segment();
		} else {
			consecutive_voice_ = 0;
		}
		return;
	}

	if (probability < kExitThreshold) {
		silence_ms_ += kWindowMs;
		if (silence_ms_ >= kSilenceCloseMs) {
			close_segment("silence");
			return;
		}
	} else {
		silence_ms_ = 0;
	}

	/* A monologue still has to reach the transcriber in pieces, or the first
	 * word of an answer waits for the last word of the question. */
	if (samples_to_ms(segment_.size()) >= kMaxSegmentMs)
		close_segment("force-cut");
}

void VadSegmenter::start_segment()
{
	speaking_ = true;
	consecutive_voice_ = 0;
	silence_ms_ = 0;
	segment_.clear();

	/* Pour the ring out oldest first. */
	if (pre_roll_filled_ > 0) {
		const size_t start = (pre_roll_head_ + pre_roll_.size() - pre_roll_filled_) % pre_roll_.size();
		for (size_t i = 0; i < pre_roll_filled_; ++i)
			segment_.push_back(pre_roll_[(start + i) % pre_roll_.size()]);
	}

	/* The segment starts where the pre-roll starts, not where the model
	 * finally agreed there was speech. */
	segment_started_at_ = iso8601_utc(-kPreRollMs);
}

void VadSegmenter::close_segment(const char *reason)
{
	speaking_ = false;
	consecutive_voice_ = 0;
	silence_ms_ = 0;

	if (segment_.empty())
		return;

	/* A little silence on the end: transcribers routinely eat the last
	 * consonant of a segment that stops dead. */
	segment_.insert(segment_.end(), static_cast<size_t>(kSampleRate) * kPostRollMs / 1000, 0.0f);

	const int duration_ms = samples_to_ms(segment_.size());

	/* Whatever was said before is over; carrying the state into the next
	 * silence only makes the next start harder to detect. */
	vad_.reset();

	if (duration_ms < kMinSegmentMs) {
		obs_log(LOG_DEBUG, "vad: dropped a %d ms blip (%s)", duration_ms, reason);
		segment_.clear();
		return;
	}

	AudioSegment out;
	out.duration_ms = duration_ms;
	out.captured_at = segment_started_at_;
	encode_wav_pcm16(segment_, kSampleRate, out.wav);
	segment_.clear();

	obs_log(LOG_DEBUG, "vad: segment %d ms, %zu kB (%s)", duration_ms, out.wav.size() / 1024, reason);

	if (on_segment_)
		on_segment_(std::move(out));
}

void VadSegmenter::flush()
{
	if (speaking_)
		close_segment("flush");
}

void VadSegmenter::discard()
{
	speaking_ = false;
	consecutive_voice_ = 0;
	silence_ms_ = 0;
	silent_windows_ = 0;
	segment_.clear();
	window_.clear();
	pre_roll_head_ = 0;
	pre_roll_filled_ = 0;
	vad_.reset();
}

} // namespace sokaster
