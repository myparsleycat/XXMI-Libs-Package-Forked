#pragma once

// The messages of the desktop channel and their wire encoding, as
// docs/desktop-channel.md defines them. This file does not depend on the rest
// of the DLL, so the vector test in tools/harness builds it on its own.

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace DesktopProtocol {

const uint16_t VERSION = 1;
// A whole frame, length field included
const uint32_t MAX_FRAME_SIZE = 4 << 20;
const uint32_t HEADER_SIZE = 12;

enum class MessageType : uint16_t {
	Hello       = 0x0001,
	HelloReply  = 0x0002,
	Ping        = 0x0003,
	Pong        = 0x0004,
	Error       = 0x00FF,
	GetVars     = 0x0010,
	VarsReply   = 0x0011,
	SetVars     = 0x0012,
	VarsApplied = 0x0013,
	Event       = 0x0020,
	PickArm     = 0x0100,
	PickArmed   = 0x0101,
	PickResult  = 0x0102,
	PickFailed  = 0x0103,
};

enum RenderApi : uint8_t {
	RENDER_D3D11  = 1,
	RENDER_D3D12  = 2,
	RENDER_VULKAN = 3,
};

const uint64_t CAPABILITY_VARS       = 1ull << 0;
const uint64_t CAPABILITY_PICK_D3D11 = 1ull << 1;

enum EventKind : uint16_t {
	EVENT_RELOADED = 1,
	EVENT_SHUTDOWN = 2,
};

const uint8_t TRIGGER_DESKTOP = 1;
const uint8_t TRIGGER_KEY     = 2;

const uint32_t PICK_HITS_OVERFLOW    = 1 << 0;
const uint32_t PICK_DEFERRED_SKIPPED = 1 << 1;

const uint8_t HIT_DEPTH_PASSED = 1 << 0;
const uint8_t HIT_FRONT_FACE = 1 << 1;

const uint8_t ORIGIN_GAME         = 1;
const uint8_t ORIGIN_COMMAND_LIST = 2;

enum DrawCallType : uint8_t {
	CALL_DRAW                             = 1,
	CALL_DRAW_INDEXED                     = 2,
	CALL_DRAW_INSTANCED                   = 3,
	CALL_DRAW_INDEXED_INSTANCED           = 4,
	CALL_DRAW_AUTO                        = 5,
	CALL_DRAW_INSTANCED_INDIRECT          = 6,
	CALL_DRAW_INDEXED_INSTANCED_INDIRECT  = 7,
};

const uint8_t OVERRIDE_TEXTURE = 1;
const uint8_t OVERRIDE_SHADER  = 2;

struct Hello {
	std::string client;
	uint16_t min_version = 0, max_version = 0;
};

struct HelloReply {
	uint16_t version = 0;
	std::string dll_version;
	uint32_t pid = 0;
	std::string exe_path;
	std::string importer_folder;
	uint8_t render_api = 0;
	uint64_t capabilities = 0;
};

struct Ping { uint64_t nonce = 0; };
struct Pong { uint64_t nonce = 0; };

struct Error {
	std::string code;
	std::string detail;
};

struct GetVars { std::vector<std::string> names; };

struct Var {
	std::string name;
	bool exists = false;
	float value = 0;
};

struct VarsReply { std::vector<Var> values; };

struct Assign {
	std::string name;
	float value = 0;
};

struct SetVars { std::vector<Assign> vars; };

struct VarsApplied {
	uint64_t frame = 0;
	std::vector<Var> values;
};

struct Event {
	uint16_t kind = 0;
	uint64_t frame = 0;
};

struct PickArm {
	uint8_t cursor = 0;
	float x = 0, y = 0;
};

struct PickArmed { uint64_t capture_id = 0; };

struct PickFailed {
	uint64_t capture_id = 0;
	std::string code;
	std::string detail;
};

struct Hit {
	uint32_t record = 0;
	uint32_t primitive = 0;
	float depth = 0;
	uint8_t flags = 0;
};

struct Buffer {
	uint32_t hash = 0, orig_hash = 0;
	bool has_region = false;
	uint32_t region_hash = 0;
	uint32_t offset = 0;
	uint32_t format_or_stride = 0;
};

struct VertexBuffer {
	uint8_t slot = 0;
	Buffer buffer;
};

struct Shader {
	uint64_t hash = 0;
	bool replaced = false;
};

struct Texture {
	uint8_t stage = 0, slot = 0;
	uint32_t hash = 0, orig_hash = 0;
	uint8_t dimension = 0;
	uint32_t width = 0, height = 0, format = 0;
};

struct Surface {
	uint32_t hash = 0, width = 0, height = 0, format = 0;
};

struct Target {
	uint8_t slot = 0;
	Surface surface;
};

struct Override {
	uint8_t kind = 0;
	std::string section;
	std::string ini_path;
};

struct CommandOrigin {
	std::string section;
	std::string ini_path;
	std::string line;
};

struct Record {
	uint32_t id = 0;
	uint8_t origin = 0;
	uint32_t draw_number = 0;
	uint8_t call = 0;
	uint32_t count = 0, instances = 0, start = 0;
	int32_t base_vertex = 0;
	uint32_t start_instance = 0;
	uint8_t topology = 0;
	uint8_t depth_func = 0;
	bool has_ib = false;
	Buffer ib;
	std::vector<VertexBuffer> vbs;
	Shader shaders[5];
	std::vector<Texture> srvs;
	std::vector<Target> rts;
	bool has_ds = false;
	Surface ds;
	std::vector<Override> overrides;
	// Only when origin is ORIGIN_COMMAND_LIST
	CommandOrigin command_list;
};

struct PickResult {
	uint64_t capture_id = 0;
	uint64_t frame = 0;
	uint8_t trigger = 0;
	uint32_t window_width = 0, window_height = 0;
	float cursor_x = 0, cursor_y = 0;
	uint32_t flags = 0;
	uint32_t hit_total = 0;
	std::vector<Hit> hits;
	std::vector<Record> records;
};

// Encodes payloads. The first value that cannot be encoded (a string over
// 65535 bytes or not UTF-8, a list over its count's width) makes failed stick.
class Writer {
public:
	std::vector<uint8_t> buf;
	bool failed = false;

	void u8(const char*, uint8_t v) { buf.push_back(v); }
	void u16(const char*, uint16_t v) { put(&v, 2); }
	void u32(const char*, uint32_t v) { put(&v, 4); }
	void u64(const char*, uint64_t v) { put(&v, 8); }
	void i32(const char*, int32_t v) { put(&v, 4); }
	void f32(const char*, float v) { put(&v, 4); }
	void boolean(const char*, bool v) { buf.push_back(v ? 1 : 0); }
	void str(const char*, const std::string &v);
	void check(bool ok) { if (!ok) failed = true; }

	template <class F> void object(const char*, F fn) { fn(); }
	template <class F> void optional(const char*, bool present, F fn)
	{
		boolean(NULL, present);
		if (present)
			fn();
	}
	template <class F> void conditional(const char*, bool present, F fn)
	{
		if (present)
			fn();
	}
	// Elements of `count_bytes` count width, each at least min_size bytes
	// on the wire (min_size only matters to the Reader)
	template <class T, class F> void list(const char*, int count_bytes, size_t, std::vector<T> &v, F fn)
	{
		uint64_t limit = count_bytes == 1 ? 0xff : count_bytes == 2 ? 0xffff : 0xffffffff;
		if (v.size() > limit) {
			failed = true;
			return;
		}
		if (count_bytes == 1)
			u8(NULL, (uint8_t)v.size());
		else if (count_bytes == 2)
			u16(NULL, (uint16_t)v.size());
		else
			u32(NULL, (uint32_t)v.size());
		for (T &e : v)
			fn(e);
	}
	template <class T, class F> void fixed(const char*, T *v, size_t n, F fn)
	{
		for (size_t i = 0; i < n; i++)
			fn(v[i]);
	}

private:
	void put(const void *v, size_t n)
	{
		const uint8_t *p = (const uint8_t*)v;
		buf.insert(buf.end(), p, p + n);
	}
};

// Decodes a payload. The first short read or invalid value makes failed stick
// and every later read gives zero.
class Reader {
public:
	const uint8_t *p;
	size_t left;
	bool failed = false;

	Reader(const uint8_t *data, size_t size) : p(data), left(size) {}

	void u8(const char*, uint8_t &v) { take(&v, 1); }
	void u16(const char*, uint16_t &v) { take(&v, 2); }
	void u32(const char*, uint32_t &v) { take(&v, 4); }
	void u64(const char*, uint64_t &v) { take(&v, 8); }
	void i32(const char*, int32_t &v) { take(&v, 4); }
	void f32(const char*, float &v) { take(&v, 4); }
	void boolean(const char*, bool &v);
	void str(const char*, std::string &v);
	void check(bool ok) { if (!ok) failed = true; }

	template <class F> void object(const char*, F fn) { fn(); }
	template <class F> void optional(const char*, bool &present, F fn)
	{
		boolean(NULL, present);
		if (present)
			fn();
	}
	template <class F> void conditional(const char*, bool present, F fn)
	{
		if (present)
			fn();
	}
	template <class T, class F> void list(const char*, int count_bytes, size_t min_size, std::vector<T> &v, F fn)
	{
		uint32_t n = 0;
		if (count_bytes == 1) {
			uint8_t c = 0;
			u8(NULL, c);
			n = c;
		} else if (count_bytes == 2) {
			uint16_t c = 0;
			u16(NULL, c);
			n = c;
		} else {
			u32(NULL, n);
		}
		// Bound the count by the bytes left, so that a corrupt count
		// cannot make us allocate far more than the frame holds
		if (failed || (uint64_t)n * min_size > left) {
			failed = true;
			v.clear();
			return;
		}
		v.resize(n);
		for (T &e : v) {
			if (failed)
				break;
			fn(e);
		}
	}
	template <class T, class F> void fixed(const char*, T *v, size_t n, F fn)
	{
		for (size_t i = 0; i < n; i++)
			fn(v[i]);
	}

	// True when the payload was read without error and to its end
	bool finish() { return !failed && left == 0; }

private:
	void take(void *v, size_t n)
	{
		if (failed || left < n) {
			failed = true;
			memset(v, 0, n);
			return;
		}
		memcpy(v, p, n);
		p += n;
		left -= n;
	}
};

bool ValidUtf8(const char *s, size_t n);

// The fields of each message in wire order. The names are those of the JSON of
// the test vectors.
template <class S> void Serialize(S &s, Hello &m)
{
	s.str("client", m.client);
	s.u16("minVersion", m.min_version);
	s.u16("maxVersion", m.max_version);
}

template <class S> void Serialize(S &s, HelloReply &m)
{
	s.u16("version", m.version);
	s.str("dllVersion", m.dll_version);
	s.u32("pid", m.pid);
	s.str("exePath", m.exe_path);
	s.str("importerFolder", m.importer_folder);
	s.u8("renderApi", m.render_api);
	s.u64("capabilities", m.capabilities);
}

template <class S> void Serialize(S &s, Ping &m) { s.u64("nonce", m.nonce); }
template <class S> void Serialize(S &s, Pong &m) { s.u64("nonce", m.nonce); }

template <class S> void Serialize(S &s, Error &m)
{
	s.str("code", m.code);
	s.str("detail", m.detail);
}

template <class S> void Serialize(S &s, GetVars &m)
{
	s.list("names", 2, 2, m.names, [&](std::string &name) { s.str(NULL, name); });
}

template <class S> void SerializeVars(S &s, std::vector<Var> &values)
{
	s.list("values", 2, 7, values, [&](Var &v) {
		s.object(NULL, [&] {
			s.str("name", v.name);
			s.boolean("exists", v.exists);
			s.f32("value", v.value);
		});
	});
}

template <class S> void Serialize(S &s, VarsReply &m) { SerializeVars(s, m.values); }

template <class S> void Serialize(S &s, SetVars &m)
{
	s.list("vars", 2, 6, m.vars, [&](Assign &v) {
		s.object(NULL, [&] {
			s.str("name", v.name);
			s.f32("value", v.value);
		});
	});
}

template <class S> void Serialize(S &s, VarsApplied &m)
{
	s.u64("frame", m.frame);
	SerializeVars(s, m.values);
}

template <class S> void Serialize(S &s, Event &m)
{
	s.u16("kind", m.kind);
	s.u64("frame", m.frame);
}

template <class S> void Serialize(S &s, PickArm &m)
{
	s.u8("cursor", m.cursor);
	s.f32("x", m.x);
	s.f32("y", m.y);
}

template <class S> void Serialize(S &s, PickArmed &m) { s.u64("captureId", m.capture_id); }

template <class S> void Serialize(S &s, PickFailed &m)
{
	s.u64("captureId", m.capture_id);
	s.str("code", m.code);
	s.str("detail", m.detail);
}

template <class S> void SerializeBuffer(S &s, Buffer &b)
{
	s.u32("hash", b.hash);
	s.u32("origHash", b.orig_hash);
	s.boolean("hasRegion", b.has_region);
	s.u32("regionHash", b.region_hash);
	s.u32("offset", b.offset);
	s.u32("formatOrStride", b.format_or_stride);
}

template <class S> void SerializeSurface(S &s, Surface &t)
{
	s.u32("hash", t.hash);
	s.u32("width", t.width);
	s.u32("height", t.height);
	s.u32("format", t.format);
}

template <class S> void SerializeRecord(S &s, Record &r)
{
	s.u32("id", r.id);
	s.u8("origin", r.origin);
	s.check(r.origin == ORIGIN_GAME || r.origin == ORIGIN_COMMAND_LIST);
	s.u32("drawNumber", r.draw_number);
	s.u8("call", r.call);
	s.u32("count", r.count);
	s.u32("instances", r.instances);
	s.u32("start", r.start);
	s.i32("baseVertex", r.base_vertex);
	s.u32("startInstance", r.start_instance);
	s.u8("topology", r.topology);
	s.u8("depthFunc", r.depth_func);
	s.optional("ib", r.has_ib, [&] { s.object("ib", [&] { SerializeBuffer(s, r.ib); }); });
	s.list("vbs", 1, 22, r.vbs, [&](VertexBuffer &vb) {
		s.object(NULL, [&] {
			s.u8("slot", vb.slot);
			SerializeBuffer(s, vb.buffer);
		});
	});
	s.fixed("shaders", r.shaders, 5, [&](Shader &shader) {
		s.object(NULL, [&] {
			s.u64("hash", shader.hash);
			s.boolean("replaced", shader.replaced);
		});
	});
	s.list("srvs", 2, 23, r.srvs, [&](Texture &t) {
		s.object(NULL, [&] {
			s.u8("stage", t.stage);
			s.u8("slot", t.slot);
			s.u32("hash", t.hash);
			s.u32("origHash", t.orig_hash);
			s.u8("dimension", t.dimension);
			s.u32("width", t.width);
			s.u32("height", t.height);
			s.u32("format", t.format);
		});
	});
	s.list("rts", 1, 17, r.rts, [&](Target &t) {
		s.object(NULL, [&] {
			s.u8("slot", t.slot);
			SerializeSurface(s, t.surface);
		});
	});
	s.optional("ds", r.has_ds, [&] { s.object("ds", [&] { SerializeSurface(s, r.ds); }); });
	s.list("overrides", 2, 5, r.overrides, [&](Override &o) {
		s.object(NULL, [&] {
			s.u8("kind", o.kind);
			s.str("section", o.section);
			s.str("iniPath", o.ini_path);
		});
	});
	s.conditional("commandList", r.origin == ORIGIN_COMMAND_LIST, [&] {
		s.object("commandList", [&] {
			s.str("section", r.command_list.section);
			s.str("iniPath", r.command_list.ini_path);
			s.str("line", r.command_list.line);
		});
	});
}

template <class S> void Serialize(S &s, PickResult &m)
{
	s.u64("captureId", m.capture_id);
	s.u64("frame", m.frame);
	s.u8("trigger", m.trigger);
	s.u32("windowWidth", m.window_width);
	s.u32("windowHeight", m.window_height);
	s.f32("cursorX", m.cursor_x);
	s.f32("cursorY", m.cursor_y);
	s.u32("flags", m.flags);
	s.u32("hitTotal", m.hit_total);
	s.list("hits", 2, 13, m.hits, [&](Hit &h) {
		s.object(NULL, [&] {
			s.u32("record", h.record);
			s.u32("primitive", h.primitive);
			s.f32("depth", h.depth);
			s.u8("flags", h.flags);
		});
	});
	s.list("records", 2, 85, m.records, [&](Record &r) {
		s.object(NULL, [&] { SerializeRecord(s, r); });
	});
}

// A whole frame of a message. False if the message cannot be encoded or the
// frame would be over MAX_FRAME_SIZE.
template <class M> bool EncodeFrame(MessageType type, uint32_t request_id, M &message, std::vector<uint8_t> *frame)
{
	Writer w;
	w.buf.resize(HEADER_SIZE);
	Serialize(w, message);
	if (w.failed || w.buf.size() > MAX_FRAME_SIZE)
		return false;
	uint32_t length = (uint32_t)w.buf.size() - 4;
	uint16_t t = (uint16_t)type, flags = 0;
	memcpy(&w.buf[0], &length, 4);
	memcpy(&w.buf[4], &t, 2);
	memcpy(&w.buf[6], &flags, 2);
	memcpy(&w.buf[8], &request_id, 4);
	frame->swap(w.buf);
	return true;
}

// Decodes a payload that has to be consumed exactly
template <class M> bool DecodePayload(const uint8_t *payload, size_t size, M *message)
{
	Reader r(payload, size);
	Serialize(r, *message);
	return r.finish();
}

struct FrameHeader {
	uint32_t payload_size;
	uint16_t type;
	uint32_t request_id;
};

// False for a length below 8, a frame over MAX_FRAME_SIZE or nonzero flags,
// which close the connection
bool ParseHeader(const uint8_t header[HEADER_SIZE], FrameHeader *out);

} // namespace DesktopProtocol
