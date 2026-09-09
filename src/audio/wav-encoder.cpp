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

#include "wav-encoder.hpp"

#include <cstring>

namespace sokaster {
namespace {

constexpr uint16_t kChannels = 1;
constexpr uint16_t kBitsPerSample = 16;

/* Written byte by byte rather than by casting a struct: RIFF is
 * little-endian everywhere, whatever the machine underneath thinks. */
void put_u32(std::vector<uint8_t> &out, uint32_t value)
{
	out.push_back(static_cast<uint8_t>(value & 0xff));
	out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
	out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
	out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void put_u16(std::vector<uint8_t> &out, uint16_t value)
{
	out.push_back(static_cast<uint8_t>(value & 0xff));
	out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void put_tag(std::vector<uint8_t> &out, const char *tag)
{
	out.insert(out.end(), tag, tag + 4);
}

} // namespace

void encode_wav_pcm16(const std::vector<float> &samples, uint32_t sample_rate, std::vector<uint8_t> &out)
{
	const uint16_t bytes_per_sample = kBitsPerSample / 8;
	const uint16_t block_align = kChannels * bytes_per_sample;
	const uint32_t byte_rate = sample_rate * block_align;
	const uint32_t data_size = static_cast<uint32_t>(samples.size()) * bytes_per_sample;

	out.clear();
	out.reserve(44 + data_size);

	put_tag(out, "RIFF");
	put_u32(out, 36 + data_size);
	put_tag(out, "WAVE");

	put_tag(out, "fmt ");
	put_u32(out, 16);
	put_u16(out, 1); /* PCM */
	put_u16(out, kChannels);
	put_u32(out, sample_rate);
	put_u32(out, byte_rate);
	put_u16(out, block_align);
	put_u16(out, kBitsPerSample);

	put_tag(out, "data");
	put_u32(out, data_size);

	for (float sample : samples) {
		/* Hard clip. Anything past unity is already distorted; wrapping it
		 * round would turn a loud word into a burst of noise. */
		if (sample > 1.0f)
			sample = 1.0f;
		else if (sample < -1.0f)
			sample = -1.0f;

		const int16_t value = static_cast<int16_t>(sample * 32767.0f);
		put_u16(out, static_cast<uint16_t>(value));
	}
}

} // namespace sokaster
