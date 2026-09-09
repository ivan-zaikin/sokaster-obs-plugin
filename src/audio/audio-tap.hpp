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

#include <obs.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sokaster {

/* The co-host listens at 16 kHz mono, because that is what Silero and the
 * transcriber upstream both want. libobs resamples on its way to us, so this
 * is the only place the rate is stated. */
constexpr uint32_t kListenSampleRate = 16000;

/*
 * A lock-free single-producer, single-consumer ring of float samples.
 *
 * The producer is the OBS audio thread and gets a memcpy and one atomic store,
 * nothing else: an audio callback that blocks is an audio callback that makes
 * the whole stream crackle.
 */
class SampleRing {
public:
	/* Capacity is rounded up to a power of two so the wrap is a mask. */
	explicit SampleRing(size_t capacity);

	void write(const float *src, size_t count);
	size_t read(float *dst, size_t count);

	size_t available() const;
	uint64_t dropped_samples() const { return dropped_; }
	void clear();

private:
	std::vector<float> data_;
	size_t mask_ = 0;

	/* Free-running counters; only their difference matters, so the wrap at
	 * 2^64 is harmless. */
	std::atomic<uint64_t> write_pos_{0};
	std::atomic<uint64_t> read_pos_{0};
	std::atomic<uint64_t> dropped_{0};
};

/*
 * The ears: a raw audio callback on one OBS mix track, feeding the ring.
 *
 * The track is a real OBS track, not a private side channel, and that is the
 * point: the streamer ticks which sources the co-host may hear, OBS mixes
 * exactly those, and we read the result. Nothing is transcribed that was not
 * ticked.
 */
class AudioTap {
public:
	AudioTap();
	~AudioTap();

	AudioTap(const AudioTap &) = delete;
	AudioTap &operator=(const AudioTap &) = delete;

	bool start(size_t mix_index);
	void stop();

	bool running() const { return running_; }
	size_t mix_index() const { return mix_index_; }

	size_t read(float *dst, size_t count) { return ring_.read(dst, count); }
	uint64_t dropped_samples() const { return ring_.dropped_samples(); }

private:
	static void on_audio(void *param, size_t mix_idx, struct audio_data *data);

	SampleRing ring_;
	std::atomic<bool> running_{false};
	size_t mix_index_ = 0;
};

namespace tracks {

/*
 * Picks a track no source is routed to, counting from the last one down.
 *
 * Plenty of streamers record several tracks for VOD editing, so quietly
 * borrowing track 6 could put game audio into someone's finished recording.
 * Returns the mix index, or -1 when every track is already spoken for.
 */
int find_free(uint32_t *used_mask_out = nullptr);

/* Which sources can be heard at all, and which are ticked for our track. */
struct AudioSourceInfo {
	std::string uuid;
	std::string name;
	bool heard = false;
};

std::vector<AudioSourceInfo> list_audio_sources(size_t mix_index);

/* Flips one source's bit for our track. This is the same bitmask the
 * "Advanced Audio Properties" dialog writes, so OBS shows the truth. */
void set_source_heard(const char *uuid, size_t mix_index, bool heard);

/* True when at least one source is routed to the track — otherwise the
 * co-host is listening to silence and should say so rather than pretend. */
bool any_source_heard(size_t mix_index);

} // namespace tracks

} // namespace sokaster
