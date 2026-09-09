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

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sokaster {

/*
 * Silero VAD v5, over ONNX Runtime. Not thread-safe: one instance per worker.
 *
 * The model's contract at 16 kHz:
 *   input  float32 [1, 576]    64 samples of context + a 512-sample chunk
 *   state  float32 [2, 1, 128] LSTM state, zeroed at the start, fed back
 *   sr     int64 scalar        16000
 *   output float32 [1, 1]      probability in [0, 1]
 *   stateN float32 [2, 1, 128] the next state
 *
 * Feeding it a bare 512-sample chunk with no context looks reasonable and is
 * not: the convolutional layers get no warm-up window and the model answers
 * about 0.0006 to everything, speech included. That cost a day once already.
 */
class SileroVad {
public:
	static constexpr int kFrameSize = 512;  /* 32 ms at 16 kHz */
	static constexpr int kContextSize = 64; /* prepended for the conv layers */
	static constexpr int kInputSize = 576;  /* context + frame */
	static constexpr int kSampleRate = 16000;
	static constexpr int kStateSize = 2 * 1 * 128;

	SileroVad();
	~SileroVad();

	SileroVad(const SileroVad &) = delete;
	SileroVad &operator=(const SileroVad &) = delete;

	/* Loads the model from an absolute path. Never throws: a failure here is
	 * a plugin that cannot hear, not a plugin that takes OBS down with it. */
	bool load(const std::string &model_path);

	bool loaded() const { return impl_ != nullptr; }

	/* Clears state and context. Wanted on pauses and at segment boundaries. */
	void reset();

	/* Takes exactly kFrameSize samples, returns the speech probability.
	 * Returns 0 when the model is not loaded or inference failed. */
	float predict(const float *frame);

	/* Inference cost, counted here rather than by the caller: only this
	 * function knows how many windows actually reached the model. */
	uint64_t predictions() const { return predictions_; }
	uint64_t inference_ns() const { return inference_ns_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	std::array<float, kStateSize> state_{};
	std::array<float, kContextSize> context_{};
	std::array<float, kInputSize> input_{};

	uint64_t predictions_ = 0;
	uint64_t inference_ns_ = 0;
};

/* Absolute path to the bundled model, or an empty string when the plugin's
 * data directory did not travel with the binary. */
std::string silero_model_path();

} // namespace sokaster
