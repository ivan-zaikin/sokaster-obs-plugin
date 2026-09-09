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

#include "audio-tap.hpp"

#include <plugin-support.h>

#include <media-io/audio-io.h>

#include <cstring>

namespace sokaster {
namespace {

/* Eight seconds of headroom. The reader empties this every few milliseconds;
 * the size is there so a stalled worker loses audio instead of the audio
 * thread losing time. */
constexpr size_t kRingSeconds = 8;

size_t round_up_pow2(size_t value)
{
	size_t result = 1;
	while (result < value)
		result <<= 1;
	return result;
}

} // namespace

SampleRing::SampleRing(size_t capacity) : data_(round_up_pow2(capacity), 0.0f)
{
	mask_ = data_.size() - 1;
}

void SampleRing::write(const float *src, size_t count)
{
	const uint64_t write_pos = write_pos_.load(std::memory_order_relaxed);
	const uint64_t read_pos = read_pos_.load(std::memory_order_acquire);
	const size_t free_space = data_.size() - static_cast<size_t>(write_pos - read_pos);

	if (count > free_space) {
		/* The reader is gone or wedged. Dropping the newest samples keeps
		 * this function to a bounded memcpy; the counter is what tells the
		 * log that anything was lost at all. */
		dropped_.fetch_add(count - free_space, std::memory_order_relaxed);
		count = free_space;
		if (count == 0)
			return;
	}

	const size_t offset = static_cast<size_t>(write_pos) & mask_;
	const size_t first = data_.size() - offset < count ? data_.size() - offset : count;

	std::memcpy(data_.data() + offset, src, first * sizeof(float));
	if (count > first)
		std::memcpy(data_.data(), src + first, (count - first) * sizeof(float));

	write_pos_.store(write_pos + count, std::memory_order_release);
}

size_t SampleRing::read(float *dst, size_t count)
{
	const uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
	const uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
	const size_t filled = static_cast<size_t>(write_pos - read_pos);

	if (count > filled)
		count = filled;
	if (count == 0)
		return 0;

	const size_t offset = static_cast<size_t>(read_pos) & mask_;
	const size_t first = data_.size() - offset < count ? data_.size() - offset : count;

	std::memcpy(dst, data_.data() + offset, first * sizeof(float));
	if (count > first)
		std::memcpy(dst + first, data_.data(), (count - first) * sizeof(float));

	read_pos_.store(read_pos + count, std::memory_order_release);
	return count;
}

size_t SampleRing::available() const
{
	const uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
	const uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
	return static_cast<size_t>(write_pos - read_pos);
}

void SampleRing::clear()
{
	read_pos_.store(write_pos_.load(std::memory_order_acquire), std::memory_order_release);
	dropped_.store(0, std::memory_order_relaxed);
}

AudioTap::AudioTap() : ring_(kListenSampleRate * kRingSeconds) {}

AudioTap::~AudioTap()
{
	stop();
}

void AudioTap::on_audio(void *param, size_t, struct audio_data *data)
{
	/* Runs on the OBS audio thread. Everything here is a memcpy and two
	 * atomics on purpose — see the ring's comment. */
	auto *tap = static_cast<AudioTap *>(param);
	if (!data || !data->data[0] || data->frames == 0)
		return;

	tap->ring_.write(reinterpret_cast<const float *>(data->data[0]), data->frames);
}

bool AudioTap::start(size_t mix_index)
{
	if (running_)
		return true;

	if (mix_index >= MAX_AUDIO_MIXES) {
		obs_log(LOG_ERROR, "audio: track index %zu out of range", mix_index);
		return false;
	}

	ring_.clear();

	/* libobs resamples and downmixes on the way here, which is what removes
	 * the whole hand-written capture-and-resample path this replaces. */
	struct audio_convert_info conversion = {};
	conversion.samples_per_sec = kListenSampleRate;
	conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
	conversion.speakers = SPEAKERS_MONO;

	mix_index_ = mix_index;
	obs_add_raw_audio_callback(mix_index, &conversion, on_audio, this);
	running_ = true;

	obs_log(LOG_INFO, "audio: listening on track %zu at %u Hz mono", mix_index + 1, kListenSampleRate);
	return true;
}

void AudioTap::stop()
{
	if (!running_)
		return;

	/* Returns only once the callback is off the list, so no sample can arrive
	 * after this line. */
	obs_remove_raw_audio_callback(mix_index_, on_audio, this);
	running_ = false;

	const uint64_t dropped = ring_.dropped_samples();
	if (dropped > 0)
		obs_log(LOG_WARNING, "audio: dropped %llu samples, the worker could not keep up",
			static_cast<unsigned long long>(dropped));

	obs_log(LOG_INFO, "audio: stopped listening on track %zu", mix_index_ + 1);
}

namespace tracks {
namespace {

struct MixerScan {
	uint32_t used = 0;
};

bool has_audio(obs_source_t *source)
{
	return (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
}

bool collect_used_mixers(void *param, obs_source_t *source)
{
	if (has_audio(source))
		static_cast<MixerScan *>(param)->used |= obs_source_get_audio_mixers(source);
	return true;
}

} // namespace

int find_free(uint32_t *used_mask_out)
{
	MixerScan scan;
	obs_enum_sources(collect_used_mixers, &scan);

	if (used_mask_out)
		*used_mask_out = scan.used;

	/* From the back, because the low tracks are the ones people actually use:
	 * track 1 is the stream, and 2-4 are the usual VOD splits. */
	for (int index = MAX_AUDIO_MIXES - 1; index >= 0; --index) {
		if ((scan.used & (1u << index)) == 0)
			return index;
	}

	return -1;
}

namespace {

struct SourceList {
	std::vector<AudioSourceInfo> items;
	uint32_t bit = 0;
};

bool collect_audio_sources(void *param, obs_source_t *source)
{
	auto *list = static_cast<SourceList *>(param);
	if (!has_audio(source))
		return true;

	AudioSourceInfo info;
	const char *uuid = obs_source_get_uuid(source);
	const char *name = obs_source_get_name(source);
	info.uuid = uuid ? uuid : "";
	info.name = name ? name : "";
	info.heard = (obs_source_get_audio_mixers(source) & list->bit) != 0;

	list->items.push_back(std::move(info));
	return true;
}

} // namespace

std::vector<AudioSourceInfo> list_audio_sources(size_t mix_index)
{
	SourceList list;
	list.bit = 1u << mix_index;
	obs_enum_sources(collect_audio_sources, &list);
	return std::move(list.items);
}

void set_source_heard(const char *uuid, size_t mix_index, bool heard)
{
	obs_source_t *source = obs_get_source_by_uuid(uuid);
	if (!source)
		return;

	const uint32_t bit = 1u << mix_index;
	const uint32_t mixers = obs_source_get_audio_mixers(source);
	const uint32_t next = heard ? (mixers | bit) : (mixers & ~bit);

	if (next != mixers) {
		obs_source_set_audio_mixers(source, next);
		obs_log(LOG_INFO, "audio: %s is %s", obs_source_get_name(source), heard ? "heard" : "muted");
	}

	obs_source_release(source);
}

bool any_source_heard(size_t mix_index)
{
	MixerScan scan;
	obs_enum_sources(collect_used_mixers, &scan);
	return (scan.used & (1u << mix_index)) != 0;
}

} // namespace tracks
} // namespace sokaster
