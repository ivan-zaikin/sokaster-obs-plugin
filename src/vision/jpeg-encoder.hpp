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

#include "frame-grabber.hpp"

#include <cstdint>
#include <vector>

namespace sokaster {

/*
 * BGRA to JPEG, using Qt's own encoder.
 *
 * obs-deps carries no libjpeg-turbo, and Qt is already linked for the dock, so
 * this costs nothing to ship and adds no vendored third-party source to review.
 * Never called from the graphics thread: the caller owns a plain copy of the
 * pixels by then, and encoding is pure CPU work.
 */
bool encode_jpeg(const Frame &frame, int quality, std::vector<uint8_t> &out);

} // namespace sokaster
