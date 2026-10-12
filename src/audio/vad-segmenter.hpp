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

#pragma once

#include "silero-vad.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sokaster {

/* One utterance, encoded as 16-bit mono WAV and ready to post. */
struct AudioSegment {
	std::vector<uint8_t> wav;
	int duration_ms = 0;
	/* When the speech started, ISO-8601 UTC: the server orders the
	 * transcript by this, not by arrival. */
	std::string captured_at;
};

/*
 * Turns a stream of 16 kHz samples into utterances: the state machine around
 * Silero's per-window verdicts that decides where a sentence begins and ends.
 * The numbers are tuned against people talking over game audio.
 */
class VadSegmenter {
public:
	static constexpr int kPreRollMs = 200;    /* keeps the attack of the first word */
	static constexpr int kPostRollMs = 100;   /* keeps the transcriber from clipping the last one */
	static constexpr int kMinSegmentMs = 500; /* shorter than this is a cough */
	static constexpr int kMaxSegmentMs = 12000;
	static constexpr int kSilenceCloseMs = 600;
	static constexpr int kVoiceEnterWindows = 3; /* about 96 ms above the threshold */

	/* Hysteresis: harder to start speaking than to keep speaking, so a pause
	 * mid-sentence does not cut the sentence in two. */
	static constexpr float kEnterThreshold = 0.5f;
	static constexpr float kExitThreshold = 0.35f;

	/* A long silence resets the LSTM: left alone, its state can drift into a
	 * corner where it answers ~0.0025 to loud speech and stays there. */
	static constexpr int kResetAfterSilentWindows = 30;
	static constexpr float kSilentWindowPeak = 0.005f;

	using SegmentHandler = std::function<void(AudioSegment &&)>;

	VadSegmenter(SileroVad &vad, SegmentHandler on_segment);

	/* Any number of samples; windowing is internal. */
	void push(const float *samples, size_t count);

private:
	void process_probability(float probability);
	void start_segment();
	void close_segment(const char *reason);

	SileroVad &vad_;
	SegmentHandler on_segment_;

	/* The window under construction. */
	std::vector<float> window_;

	/* Pre-roll ring, always written, read only when speech starts. */
	std::vector<float> pre_roll_;
	size_t pre_roll_head_ = 0;
	size_t pre_roll_filled_ = 0;

	std::vector<float> segment_;
	bool speaking_ = false;
	std::string segment_started_at_;

	int consecutive_voice_ = 0;
	int silence_ms_ = 0;
	int silent_windows_ = 0;
};

} // namespace sokaster
