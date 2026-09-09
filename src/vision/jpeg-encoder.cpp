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

#include "jpeg-encoder.hpp"

#include <plugin-support.h>

#include <QBuffer>
#include <QImage>

namespace sokaster {

bool encode_jpeg(const Frame &frame, int quality, std::vector<uint8_t> &out)
{
	if (frame.width == 0 || frame.height == 0 || frame.data.empty())
		return false;

	/* Format_RGB32 is 0xffRRGGBB in a word, which on a little-endian machine
	 * is exactly the BGRA byte order libobs staged for us. The alpha channel
	 * is dropped, which is what we want: JPEG has none. */
	const QImage image(frame.data.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
			   static_cast<qsizetype>(frame.width) * 4, QImage::Format_RGB32);
	if (image.isNull())
		return false;

	QByteArray encoded;
	QBuffer buffer(&encoded);
	if (!buffer.open(QIODevice::WriteOnly))
		return false;

	if (!image.save(&buffer, "JPEG", quality)) {
		obs_log(LOG_WARNING, "vision: jpeg encode failed");
		return false;
	}
	buffer.close();

	out.assign(encoded.constData(), encoded.constData() + encoded.size());
	return true;
}

} // namespace sokaster
