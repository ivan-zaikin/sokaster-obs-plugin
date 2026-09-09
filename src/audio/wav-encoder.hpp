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

#include <cstdint>
#include <vector>

namespace sokaster {

/*
 * Mono 16-bit PCM in a 44-byte RIFF wrapper — the shape every speech-to-text
 * service accepts without asking questions.
 */
void encode_wav_pcm16(const std::vector<float> &samples, uint32_t sample_rate, std::vector<uint8_t> &out);

} // namespace sokaster
