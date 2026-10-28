// The JSON form of a desktop channel message, in the shape of the vectors'
// .json files, by the same Serialize functions as the wire
#pragma once
#include <stdio.h>
#include <string>
#include <vector>
#include "../../../../../DirectX11/desktop/Protocol.h"

class Json {
public:
	std::string out;

	void u8(const char *name, uint8_t v) { number(name, std::to_string(v)); }
	void u16(const char *name, uint16_t v) { number(name, std::to_string(v)); }
	void u32(const char *name, uint32_t v) { number(name, std::to_string(v)); }
	void u64(const char *name, uint64_t v) { number(name, std::to_string(v)); }
	void i32(const char *name, int32_t v) { number(name, std::to_string(v)); }
	void f32(const char *name, float v)
	{
		char buf[32];
		_snprintf(buf, sizeof(buf), "%.9g", v);
		number(name, buf);
	}
	void boolean(const char *name, bool v) { number(name, v ? "true" : "false"); }
	void str(const char *name, const std::string &v)
	{
		key(name);
		out += '"';
		for (unsigned char c : v) {
			if (c == '"' || c == '\\') {
				out += '\\';
				out += (char)c;
			} else if (c < 0x20) {
				char buf[8];
				_snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += (char)c;
			}
		}
		out += '"';
	}
	void check(bool) {}

	template <class F> void object(const char *name, F fn)
	{
		key(name);
		out += '{';
		first = true;
		fn();
		out += '}';
		first = false;
	}
	template <class F> void optional(const char *name, bool present, F fn)
	{
		if (present)
			fn();
		else
			number(name, "null");
	}
	template <class F> void conditional(const char *name, bool present, F fn) { optional(name, present, fn); }
	template <class T, class F> void list(const char *name, int, size_t, std::vector<T> &v, F fn)
	{
		key(name);
		out += '[';
		first = true;
		for (T &e : v)
			fn(e);
		out += ']';
		first = false;
	}
	template <class T, class F> void fixed(const char *name, T *v, size_t n, F fn)
	{
		key(name);
		out += '[';
		first = true;
		for (size_t i = 0; i < n; i++)
			fn(v[i]);
		out += ']';
		first = false;
	}

private:
	bool first = true;

	void key(const char *name)
	{
		if (!first)
			out += ',';
		first = false;
		if (name) {
			out += '"';
			out += name;
			out += "\":";
		}
	}
	void number(const char *name, const std::string &v)
	{
		key(name);
		out += v;
	}
};

template <class M> std::string ToJson(M &message)
{
	Json j;
	j.object(NULL, [&] { DesktopProtocol::Serialize(j, message); });
	return j.out;
}
