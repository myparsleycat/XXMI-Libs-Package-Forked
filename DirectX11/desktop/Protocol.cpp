#include "Protocol.h"

namespace DesktopProtocol {

// Rejects overlong forms, surrogates and code points past U+10FFFF, as Go's
// utf8.Valid does on the desktop side
bool ValidUtf8(const char *str, size_t n)
{
	const uint8_t *s = (const uint8_t*)str;
	size_t i = 0;

	while (i < n) {
		uint8_t c = s[i];
		uint32_t cp;
		size_t len;

		if (c < 0x80) {
			i++;
			continue;
		}
		if (c >= 0xc2 && c <= 0xdf) {
			len = 2;
			cp = c & 0x1f;
		} else if (c >= 0xe0 && c <= 0xef) {
			len = 3;
			cp = c & 0x0f;
		} else if (c >= 0xf0 && c <= 0xf4) {
			len = 4;
			cp = c & 0x07;
		} else {
			return false;
		}
		if (n - i < len)
			return false;
		for (size_t k = 1; k < len; k++) {
			if ((s[i + k] & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (s[i + k] & 0x3f);
		}
		if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return false;
		i += len;
	}
	return true;
}

void Writer::str(const char*, const std::string &v)
{
	if (v.size() > 0xffff || !ValidUtf8(v.data(), v.size())) {
		failed = true;
		return;
	}
	uint16_t n = (uint16_t)v.size();
	put(&n, 2);
	buf.insert(buf.end(), v.begin(), v.end());
}

void Reader::boolean(const char*, bool &v)
{
	uint8_t b = 0;
	u8(NULL, b);
	if (b > 1)
		failed = true;
	v = b == 1;
}

void Reader::str(const char*, std::string &v)
{
	uint16_t n = 0;
	u16(NULL, n);
	v.clear();
	if (failed || left < n) {
		failed = true;
		return;
	}
	if (!ValidUtf8((const char*)p, n)) {
		failed = true;
		return;
	}
	v.assign((const char*)p, n);
	p += n;
	left -= n;
}

bool ParseHeader(const uint8_t header[HEADER_SIZE], FrameHeader *out)
{
	uint32_t length;
	uint16_t flags;

	memcpy(&length, header, 4);
	memcpy(&out->type, header + 4, 2);
	memcpy(&flags, header + 6, 2);
	memcpy(&out->request_id, header + 8, 4);
	if (length < HEADER_SIZE - 4 || length > MAX_FRAME_SIZE - 4 || flags != 0)
		return false;
	out->payload_size = length - (HEADER_SIZE - 4);
	return true;
}

} // namespace DesktopProtocol
