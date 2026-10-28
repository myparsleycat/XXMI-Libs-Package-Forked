// Checks picking over the desktop channel (docs/desktop-channel.md) without a
// game. Loads the d3d11.dll of a run folder, writes Mods\pick\mod.ini there
// with the hashes of its own buffers and shaders, and draws one scene every
// frame into a 640x360 target with depth:
//
//   cloth  x -0.6..0.0, z 0.3, drawn first, its vertex shader writes
//          SV_Position to o3 after three other outputs
//   body   x -0.8..0.2, z 0.6, drawn second, SV_Position in o0, so hidden
//          behind the cloth where they overlap
//   mod    two quads, x 0.3..0.55 and 0.6..0.9, drawn as one DrawIndexed of
//          12 indices that mod.ini skips and draws again in two drawindexed
//          parts of 6
//   post   x -1.0..-0.05, z 1.0, a pass after the scene with depth func
//          ALWAYS, as a game's full screen pass: where nothing was drawn its
//          depth equals the cleared depth, and it must still not come out as
//          the visible draw
//   and a cached deferred command list into a target of its own
//
// A client thread of this program connects to the pipe the DLL serves, checks
// the handshake, INI variables and the refusals, then picks at the overlap,
// at the second mod part and at empty space and checks the PickResults.
// After every draw the program checks that the output merger, pixel shader,
// its b13 and the depth stencil state are what it set, and that every frame's
// picture is the same, pick or no pick. Hidden draws inside overlapping
// occlusion and pipeline statistics queries must preserve the query results.
//
//   pick.exe <run folder> [uav | region | devices | cbzero | indirect | shader-uav | limits | coplanar | stencil | reconnect | multiswap | predication | backpressure | idle <seconds>]
//
// With idle it draws the scene for that long without its client, for
// client.exe to connect to.
// The uav, region and devices cases use WARP to check upper UAV slots,
// hashes before buffer updates and picking across two devices, respectively.
//
// Exits with 2 when a check fails, 1 when the DLL or D3D11 fails.

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../../../common/crc.h"
#include "../../../common/window.h"
#include "../../../../../DirectX11/desktop/Protocol.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DesktopProtocol;

typedef HRESULT (WINAPI *CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { printf("FAILED %s = 0x%08x\n", #call, (unsigned)hr_); exit(1); } } while (0)

static const UINT WIDTH = 640, HEIGHT = 360;

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11DeviceContext1 *ctx1;
static volatile LONG failures;
static volatile bool client_done;
enum class TestMode { BASIC, UAV, REGION, DEVICES, CBZERO, INDIRECT, SHADER_UAV, LIMITS, COPLANAR, STENCIL, RECONNECT, MULTISWAP, PREDICATION, BACKPRESSURE };
static TestMode test_mode;
static unsigned scene_number;
static const float cloth_vertices[] = {
	-0.6f, -0.5f, 0.3f, -0.6f, 0.5f, 0.3f, 0, 0.5f, 0.3f, 0, -0.5f, 0.3f,
};
static const uint16_t cloth_indices[] = { 0, 1, 2, 0, 2, 3 };

static void expect(const char *what, bool ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		InterlockedIncrement(&failures);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static uint64_t fnv(const void *data, size_t size)
{
	const uint8_t *p = (const uint8_t*)data;
	uint64_t h = 0;
	for (size_t i = 0; i < size; i++) {
		h *= 0x100000001b3ull;
		h ^= p[i];
	}
	return h;
}

// ---------------------------------------------------------------- the scene

static const char *shader_source = R"(
cbuffer Tint : register(b13) { float4 tint; };
Texture2D<float4> tex : register(t0);
struct VIn { float3 pos : POSITION; };
struct VOut0 { float4 pos : SV_Position; float4 color : COLOR0; };
struct VOut3 { float4 a : TEXCOORD0; float4 b : TEXCOORD1; float4 c : TEXCOORD2; float4 pos : SV_Position; };
#ifdef SHADER_UAV
RWStructuredBuffer<uint> vertex_data : register(u12);
#endif
float3 vertex_position(float3 pos) {
#ifdef SHADER_UAV
    uint previous;
    InterlockedAdd(vertex_data[1], 1, previous);
    pos.x += vertex_data[0] ? 0 : 0.4;
#endif
    return pos;
}
VOut0 vs_o0(VIn v) { VOut0 o; o.pos = float4(vertex_position(v.pos), 1); o.color = float4(0.8, 0.5, 0.4, 1); return o; }
VOut0 vs_mod(VIn v) { VOut0 o; o.pos = float4(vertex_position(v.pos), 1); o.color = float4(0.2, 0.3, 0.9, 1); return o; }
VOut0 vs_post(VIn v) { VOut0 o; o.pos = float4(vertex_position(v.pos), 1); o.color = float4(0.5, 0.5, 0.5, 1); return o; }
VOut3 vs_o3(VIn v) { VOut3 o; o.a = 1; o.b = 2; o.c = float4(0.1, 0.7, 0.2, 1); o.pos = float4(vertex_position(v.pos), 1); return o; }
float4 ps0(VOut0 i) : SV_Target { return i.color * tint * tex.Load(int3(0, 0, 0)); }
float4 ps3(VOut3 i) : SV_Target { return i.c * tint; }
)";

static ID3DBlob *compile(const char *entry, const char *target)
{
	ID3DBlob *blob = NULL, *errors = NULL;
	D3D_SHADER_MACRO defines[] = { { "SHADER_UAV", "1" }, { NULL, NULL } };
	if (FAILED(D3DCompile(shader_source, strlen(shader_source), "pick",
			test_mode == TestMode::SHADER_UAV ? defines : NULL, NULL, entry, target, 0, 0, &blob, &errors))) {
		printf("%s does not compile: %s\n", entry, errors ? (char*)errors->GetBufferPointer() : "");
		exit(1);
	}
	if (test_mode == TestMode::SHADER_UAV && !strncmp(target, "vs_", 3)) {
		ID3DBlob *stripped;
		CHECK(D3DStripShader(blob->GetBufferPointer(), blob->GetBufferSize(), D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped));
		blob->Release();
		blob = stripped;
	}
	return blob;
}

struct Mesh {
	ID3D11Buffer *vb, *ib;
	UINT indices;
	uint32_t ib_hash;
};

static uint32_t buffer_hash(const void *data, UINT bytes, const D3D11_BUFFER_DESC &desc)
{
	return crc32c(crc32c(0, data, bytes), &desc, sizeof(desc));
}

static Mesh make_mesh(const std::vector<float> &quads, float z)
{
	Mesh m = {};
	std::vector<float> vertices;
	std::vector<uint16_t> indices;
	for (size_t q = 0; q < quads.size(); q += 4) {
		float x0 = quads[q], x1 = quads[q + 1], y0 = quads[q + 2], y1 = quads[q + 3];
		uint16_t base = (uint16_t)(vertices.size() / 3);
		float v[] = { x0, y0, z, x0, y1, z, x1, y1, z, x1, y0, z };
		vertices.insert(vertices.end(), v, v + 12);
		uint16_t i[] = { 0, 1, 2, 0, 2, 3 };
		for (uint16_t k : i)
			indices.push_back(base + k);
	}
	D3D11_BUFFER_DESC vd = { (UINT)(vertices.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA vi = { vertices.data() };
	CHECK(dev->CreateBuffer(&vd, &vi, &m.vb));
	D3D11_BUFFER_DESC id = { (UINT)(indices.size() * 2), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA ii = { indices.data() };
	CHECK(dev->CreateBuffer(&id, &ii, &m.ib));
	m.indices = (UINT)indices.size();
	m.ib_hash = buffer_hash(indices.data(), id.ByteWidth, id);
	return m;
}

// --------------------------------------------------------------- the client

static uint64_t vs_o0_hash, vs_o3_hash, vs_mod_hash, vs_post_hash, ps0_hash;
static uint32_t mod_ib_hash;
static uint32_t mod_ib_region_hash;
static std::string run_folder;

static HANDLE open_pipe(DWORD *error)
{
	wchar_t name[64];
	_snwprintf_s(name, _TRUNCATE, L"\\\\.\\pipe\\nahida-migoto-%lu", GetCurrentProcessId());
	HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	*error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	return h;
}

static bool write_all(HANDLE h, const std::vector<uint8_t> &data)
{
	DWORD n;
	return WriteFile(h, data.data(), (DWORD)data.size(), &n, NULL) && n == data.size();
}

template <class M> static bool send(HANDLE h, MessageType type, uint32_t id, M m)
{
	std::vector<uint8_t> frame;
	return EncodeFrame(type, id, m, &frame) && write_all(h, frame);
}

static bool read_exact(HANDLE h, void *out, DWORD size)
{
	uint8_t *p = (uint8_t*)out;
	while (size) {
		DWORD n = 0;
		if (!ReadFile(h, p, size, &n, NULL) || !n)
			return false;
		p += n;
		size -= n;
	}
	return true;
}

struct Received {
	FrameHeader header;
	std::vector<uint8_t> payload;
};

static bool receive(HANDLE h, Received *r)
{
	uint8_t header[HEADER_SIZE];
	if (!read_exact(h, header, HEADER_SIZE) || !ParseHeader(header, &r->header))
		return false;
	r->payload.resize(r->header.payload_size);
	return !r->header.payload_size || read_exact(h, r->payload.data(), r->header.payload_size);
}

template <class M> static bool receive_as(HANDLE h, MessageType type, uint32_t id, M *m)
{
	Received r;
	if (!receive(h, &r))
		return false;
	return r.header.type == (uint16_t)type && r.header.request_id == id && DecodePayload(r.payload.data(), r.payload.size(), m);
}

static bool error_reply(HANDLE h, uint32_t id, const char *code, std::string *detail = NULL)
{
	Error e;
	if (!receive_as(h, MessageType::Error, id, &e))
		return false;
	if (detail)
		*detail = e.detail;
	return e.code == code;
}

static uint32_t next_id = 1;

template <class M> static void check_large_rejection(HANDLE h, MessageType type, uint32_t id, M &message)
{
	Ping ping;
	ping.nonce = id;
	bool sent = send(h, type, id, message) && send(h, MessageType::Ping, id + 1, ping);
	expect("the oversized-response request fits in a frame", sent);
	if (!sent)
		return;
	Received r;
	if (!receive(h, &r)) {
		expect("the oversized-response request is answered", false);
		return;
	}
	Error error;
	bool rejected = r.header.type == (uint16_t)MessageType::Error && r.header.request_id == id
		&& DecodePayload(r.payload.data(), r.payload.size(), &error) && error.code == "RESPONSE_TOO_LARGE";
	expect("the oversized response is rejected with the request ID", rejected);
	if (r.header.type != (uint16_t)MessageType::Pong && !receive(h, &r)) {
		expect("Ping after rejection is answered", false);
		return;
	}
	Pong pong;
	expect("the connection remains usable after rejection", r.header.type == (uint16_t)MessageType::Pong
		&& r.header.request_id == id + 1 && DecodePayload(r.payload.data(), r.payload.size(), &pong) && pong.nonce == id);
}

static void check_variable_limits(HANDLE h)
{
	GetVars get;
	get.names.assign(64, std::string(65535, 'a'));
	size_t excess = HEADER_SIZE + 2 + get.names.size() * (65535 + 7) - MAX_FRAME_SIZE;
	get.names.back().resize(65535 - excess);
	VarsReply reply;
	bool ok = send(h, MessageType::GetVars, 100, get) && receive_as(h, MessageType::VarsReply, 100, &reply);
	expect("GetVars accepts a response of exactly 4 MiB", ok && reply.values.size() == get.names.size()
		&& reply.values.back().name == get.names.back() && !reply.values.back().exists);
	get.names.back().push_back('a');
	check_large_rejection(h, MessageType::GetVars, 101, get);

	const char *name = "$\\Mods\\pick\\mod.ini\\test";
	SetVars set;
	set.vars.push_back({ name, 9 });
	for (const std::string &filler : get.names)
		set.vars.push_back({ filler, 0 });
	size_t size = HEADER_SIZE + 8 + 2;
	for (const Assign &a : set.vars)
		size += 7 + a.name.size();
	set.vars.back().name.resize(set.vars.back().name.size() - (size - MAX_FRAME_SIZE) + 1);
	check_large_rejection(h, MessageType::SetVars, 103, set);
	get.names = { name };
	ok = send(h, MessageType::GetVars, 105, get) && receive_as(h, MessageType::VarsReply, 105, &reply);
	expect("rejected SetVars does not change any variable", ok && reply.values.size() == 1
		&& reply.values[0].exists && reply.values[0].value == 5);

	set.vars.back().name.pop_back();
	VarsApplied applied;
	ok = send(h, MessageType::SetVars, 106, set) && receive_as(h, MessageType::VarsApplied, 106, &applied);
	expect("SetVars accepts a response of exactly 4 MiB", ok && applied.values.size() == set.vars.size()
		&& applied.values[0].exists && applied.values[0].value == 9 && applied.values.back().name == set.vars.back().name);
}

static void check_reconnect(HANDLE *connection)
{
	Hello hello;
	hello.client = "reconnect harness";
	hello.min_version = hello.max_version = 1;
	GetVars large, barrier;
	large.names.assign(65535, "$\\missing-variable-xxxxxxxxxxxxxxxxxxxxxxxxx");
	barrier.names = { "$\\Mods\\pick\\mod.ini\\test" };
	bool isolated = true;
	for (unsigned trial = 0; trial < 12; trial++) {
		if (!send(*connection, MessageType::GetVars, 1000 + trial, large)) {
			expect("GetVars before reconnect is sent", false);
			return;
		}
		Sleep(10 + trial * 2);
		CloseHandle(*connection);
		DWORD error;
		for (unsigned retry = 0; retry < 500; retry++) {
			*connection = open_pipe(&error);
			if (*connection != INVALID_HANDLE_VALUE)
				break;
			Sleep(1);
		}
		HelloReply info;
		if (*connection == INVALID_HANDLE_VALUE || !send(*connection, MessageType::Hello, 1, hello)
				|| !receive_as(*connection, MessageType::HelloReply, 1, &info)
				|| !send(*connection, MessageType::GetVars, 2, barrier)) {
			expect("the new connection accepts requests", false);
			return;
		}
		Received received;
		bool answered = false;
		while (receive(*connection, &received)) {
			if (received.header.type == (uint16_t)MessageType::Event)
				continue;
			if (received.header.request_id >= 1000) {
				isolated = false;
				continue;
			}
			VarsReply reply;
			answered = received.header.type == (uint16_t)MessageType::VarsReply
				&& received.header.request_id == 2 && DecodePayload(received.payload.data(), received.payload.size(), &reply)
				&& reply.values.size() == 1 && reply.values[0].exists && reply.values[0].value == 5;
			break;
		}
		expect("the new connection receives its own variable response", answered);
	}
	expect("responses from disconnected clients never reach a new connection", isolated);
}

static std::string pressure_variable(unsigned index)
{
	return "$\\Mods\\pick\\mod.ini\\pressure" + std::to_string(index);
}

static void check_reply_backpressure(HANDLE h)
{
	const unsigned requests = 512;
	const uint32_t first_id = 2000;
	std::vector<uint8_t> bytes, frame;
	bytes.reserve(128 * 1024 * 1024 + 65536);
	for (unsigned i = 0; i < requests; i++) {
		bool encoded;
		if (i % 2) {
			SetVars set;
			set.vars.push_back({ pressure_variable(i / 2), (float)(i + 1) });
			encoded = EncodeFrame(MessageType::SetVars, first_id + i, set, &frame);
		} else {
			GetVars get;
			encoded = EncodeFrame(MessageType::GetVars, first_id + i, get, &frame);
		}
		if (!encoded) {
			expect("backpressure requests encode", false);
			return;
		}
		bytes.insert(bytes.end(), frame.begin(), frame.end());
		if (i % 4 == 3) {
			// Unknown frames keep reads ready while the render thread queues replies.
			size_t offset = bytes.size();
			bytes.resize(offset + 1024 * 1024);
			uint32_t length = 1024 * 1024 - 4;
			uint16_t type = 0x7ffe;
			memcpy(bytes.data() + offset, &length, 4);
			memcpy(bytes.data() + offset + 4, &type, 2);
		}
	}
	if (!write_all(h, bytes)) {
		expect("backpressure requests are sent", false);
		return;
	}
	std::vector<bool> seen(requests), applied(requests / 2);
	unsigned responses = 0, busy = 0;
	bool valid = true;
	DWORD until = GetTickCount() + 5000;
	while (responses < requests && (int)(until - GetTickCount()) > 0) {
		DWORD available = 0;
		if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL))
			break;
		if (available < HEADER_SIZE) {
			Sleep(1);
			continue;
		}
		Received r;
		if (!receive(h, &r))
			break;
		if (r.header.type == (uint16_t)MessageType::Event)
			continue;
		if (r.header.request_id < first_id || r.header.request_id >= first_id + requests) {
			valid = false;
			continue;
		}
		unsigned i = r.header.request_id - first_id;
		if (seen[i]) {
			valid = false;
			continue;
		}
		seen[i] = true;
		responses++;
		if (r.header.type == (uint16_t)MessageType::Error) {
			Error error;
			valid &= DecodePayload(r.payload.data(), r.payload.size(), &error) && error.code == "BUSY";
			busy++;
		} else if (i % 2) {
			VarsApplied reply;
			applied[i / 2] = true;
			valid &= r.header.type == (uint16_t)MessageType::VarsApplied
				&& DecodePayload(r.payload.data(), r.payload.size(), &reply) && reply.values.size() == 1
				&& reply.values[0].name == pressure_variable(i / 2) && reply.values[0].exists
				&& reply.values[0].value == (float)(i + 1);
		} else {
			VarsReply reply;
			valid &= r.header.type == (uint16_t)MessageType::VarsReply
				&& DecodePayload(r.payload.data(), r.payload.size(), &reply) && reply.values.empty();
		}
	}
	printf("  backpressure: %u responses, %u BUSY, %u missing\n", responses, busy, requests - responses);
	expect("every backpressure request receives one matching response", responses == requests && valid);

	GetVars get;
	for (unsigned i = 0; i < requests / 2; i++)
		get.names.push_back(pressure_variable(i));
	VarsReply reply;
	bool ok = send(h, MessageType::GetVars, first_id + requests, get)
		&& receive_as(h, MessageType::VarsReply, first_id + requests, &reply)
		&& reply.values.size() == get.names.size();
	expect("requests succeed after the reply queue drains", ok);
	for (unsigned i = 0; ok && i < reply.values.size(); i++)
		ok = reply.values[i].name == get.names[i] && reply.values[i].exists
			&& reply.values[i].value == (applied[i] ? (float)(i * 2 + 2) : 0);
	expect("only acknowledged SetVars requests change variables under backpressure", ok);
}

// Picks at normalized (x, y) and waits for the result of that capture
static bool pick(HANDLE h, float x, float y, PickResult *result)
{
	uint32_t id = next_id++;
	PickArm arm;
	arm.cursor = 1;
	arm.x = x;
	arm.y = y;
	PickArmed armed;
	if (!send(h, MessageType::PickArm, id, arm) || !receive_as(h, MessageType::PickArmed, id, &armed))
		return false;
	for (;;) {
		Received r;
		if (!receive(h, &r))
			return false;
		if (r.header.type == (uint16_t)MessageType::PickFailed) {
			PickFailed f;
			DecodePayload(r.payload.data(), r.payload.size(), &f);
			printf("  pick failed: %s %s\n", f.code.c_str(), f.detail.c_str());
			return false;
		}
		if (r.header.type == (uint16_t)MessageType::PickResult) {
			if (!DecodePayload(r.payload.data(), r.payload.size(), result))
				return false;
			if (result->capture_id == armed.capture_id)
				return true;
		}
	}
}

static const Record* record_by_id(const PickResult &r, uint32_t id)
{
	for (const Record &rec : r.records) {
		if (rec.id == id)
			return &rec;
	}
	return NULL;
}

static void dump(const PickResult &r)
{
	printf("  %zu hits of %u, %zu records, flags %u\n", r.hits.size(), r.hit_total, r.records.size(), r.flags);
	for (const Hit &h : r.hits)
		printf("  hit record %u prim %u depth %.3f flags %u\n", h.record, h.primitive, h.depth, h.flags);
	for (const Record &rec : r.records) {
		printf("  record %u origin %u draw %u call %u count %u start %u vs %016llx ib %08x %s\n", rec.id, rec.origin,
			rec.draw_number, rec.call, rec.count, rec.start, rec.shaders[0].hash, rec.has_ib ? rec.ib.hash : 0,
			rec.command_list.line.c_str());
		for (const Override &o : rec.overrides)
			printf("    override %u %s %s\n", o.kind, o.section.c_str(), o.ini_path.c_str());
	}
}

static bool ends_with(const std::string &s, const std::string &end)
{
	return s.size() >= end.size() && !_stricmp(s.c_str() + s.size() - end.size(), end.c_str());
}

static bool has_override(const Record &rec, uint8_t kind, const char *section)
{
	for (const Override &o : rec.overrides) {
		if (o.kind == kind && o.section == section && ends_with(o.ini_path, "\\Mods\\pick\\mod.ini"))
			return true;
	}
	return false;
}

static void check_overlap(const PickResult &r)
{
	const Hit *cloth_tested = NULL, *body_cover = NULL;
	bool body_tested = false, post_cover = false, post_tested = false;
	uint32_t cloth = UINT32_MAX, body = UINT32_MAX, post = UINT32_MAX, visible = UINT32_MAX;
	if (test_mode == TestMode::MULTISWAP)
		expect("the capture uses the primary swap chain's window", r.window_width == WIDTH && r.window_height == HEIGHT);

	for (const Record &rec : r.records) {
		if (rec.shaders[0].hash == vs_o3_hash)
			cloth = rec.id;
		if (rec.shaders[0].hash == vs_o0_hash)
			body = rec.id;
		if (rec.shaders[0].hash == vs_post_hash)
			post = rec.id;
	}
	for (const Hit &h : r.hits) {
		if (h.record == cloth && (h.flags & HIT_DEPTH_PASSED))
			cloth_tested = &h;
		if (h.record == body && (h.flags & HIT_DEPTH_PASSED))
			body_tested = true;
		if (h.record == body && !(h.flags & HIT_DEPTH_PASSED))
			body_cover = &h;
		if (h.record == post)
			(h.flags & HIT_DEPTH_PASSED ? post_tested : post_cover) = true;
		if ((h.flags & HIT_DEPTH_PASSED) && (visible == UINT32_MAX || h.record > visible))
			visible = h.record;
	}
	expect("overlap: the ALWAYS pass after the scene has a hit with depth testing off", post_cover);
	expect("overlap: the ALWAYS pass has no depth-tested hit", !post_tested);
	expect("overlap: the visible draw (highest id with a depth-tested hit) is the cloth", visible == cloth);
	expect("overlap: the cloth (SV_Position in o3) has a depth-tested hit", cloth_tested != NULL);
	expect("overlap: the hidden body has a hit with depth testing off", body_cover != NULL);
	expect("overlap: the hidden body has no depth-tested hit", !body_tested);
	expect("overlap: the cloth was drawn before the body", cloth < body);
	expect("overlap: four hits counted", r.hit_total == 4 && r.hits.size() == 4);
	if (cloth_tested && body_cover)
		expect("overlap: the expected mesh depths are reported", test_mode == TestMode::COPLANAR || test_mode == TestMode::STENCIL
			? cloth_tested->depth == body_cover->depth : cloth_tested->depth < body_cover->depth);
	const Record *c = record_by_id(r, cloth);
	if (c) {
		expect("overlap: the cloth is a game DrawIndexed of 6", c->origin == ORIGIN_GAME && c->call == CALL_DRAW_INDEXED && c->count == 6);
		expect("overlap: the cloth's draw is the first of the frame", c->draw_number == 1);
		expect("overlap: depth func LESS, triangle list", c->depth_func == D3D11_COMPARISON_LESS && c->topology == 4);
		expect("overlap: the depth target is 640x360", c->has_ds && c->ds.width == WIDTH && c->ds.height == HEIGHT);
		expect("overlap: one render target of 640x360", c->rts.size() == 1 && c->rts[0].slot == 0 && c->rts[0].surface.width == WIDTH);
		expect("overlap: the pixel shader is the game's, not replaced", c->shaders[4].hash != 0 && !c->shaders[4].replaced);
		expect("overlap: no overrides for the cloth", c->overrides.empty());
		if (test_mode == TestMode::REGION) {
			expect("overlap: the vertex buffer region hash is from the draw before its update",
				c->vbs.size() == 1 && c->vbs[0].buffer.has_region
				&& c->vbs[0].buffer.region_hash == crc32c(0, cloth_vertices, sizeof(cloth_vertices)));
			expect("overlap: the index buffer region hash is from the draw before its update",
				c->has_ib && c->ib.has_region && c->ib.region_hash == crc32c(0, cloth_indices, sizeof(cloth_indices)));
		}
	}
	const Record *b = record_by_id(r, body);
	if (b) {
		bool texture = false;
		for (const Texture &t : b->srvs)
			texture = texture || (t.stage == 4 && t.slot == 0 && t.width == 4 && t.height == 4);
		expect("overlap: the body's ps-t0 4x4 texture is reported", texture);
		expect("overlap: the body's pixel shader hash", b->shaders[4].hash == ps0_hash);
		expect("overlap: the body's vertex buffer at slot 0 with stride 12", b->vbs.size() == 1 && b->vbs[0].slot == 0 && b->vbs[0].buffer.format_or_stride == 12);
	}
	expect("overlap: a draw on a deferred context is reported as skipped", (r.flags & PICK_DEFERRED_SKIPPED) != 0);
}

static void check_mod(const PickResult &r, bool first_part = false)
{
	const Record *part = NULL;
	bool tested = false, game = false;
	for (const Record &rec : r.records) {
		if (rec.origin == ORIGIN_COMMAND_LIST)
			part = &rec;
		if (rec.origin == ORIGIN_GAME)
			game = true;
	}
	expect("mod: one command list record and no game record", part && !game && r.records.size() == 1);
	if (!part)
		return;
	for (const Hit &h : r.hits)
		tested = tested || (h.record == part->id && (h.flags & HIT_DEPTH_PASSED));
	expect("mod: the part has a depth-tested hit", tested);
	if (test_mode == TestMode::INDIRECT) {
		expect("mod: the indirect draw command", part->command_list.line == (first_part
			? "drawinstancedindirect = resourceargs, 0" : "drawindexedinstancedindirect = resourceargs, 16"));
		expect("mod: the indirect draw type", part->call == (first_part
			? CALL_DRAW_INSTANCED_INDIRECT : CALL_DRAW_INDEXED_INSTANCED_INDIRECT));
	} else {
		expect("mod: the line of the second part", part->command_list.line == "drawindexed = 6, 6, 0");
		expect("mod: DrawIndexed of 6 from index 6", part->call == CALL_DRAW_INDEXED && part->count == 6 && part->start == 6);
	}
	expect("mod: the section that drew it", part->command_list.section == "TextureOverridePickIB"
		&& ends_with(part->command_list.ini_path, "\\Mods\\pick\\mod.ini"));
	expect("mod: it belongs to the third game draw", part->draw_number == 3);
	expect("mod: the game's index buffer hash", part->has_ib && part->ib.hash == mod_ib_hash && part->ib.orig_hash == mod_ib_hash
		&& part->ib.has_region == (test_mode == TestMode::REGION) && part->ib.format_or_stride == DXGI_FORMAT_R16_UINT);
	if (test_mode == TestMode::REGION)
		expect("mod: the game's index buffer region hash", part->ib.region_hash == mod_ib_region_hash);
	expect("mod: the game's vertex shader", part->shaders[0].hash == vs_mod_hash);
	expect("mod: the TextureOverride and ShaderOverride that matched", has_override(*part, OVERRIDE_TEXTURE, "TextureOverridePickIB")
		&& has_override(*part, OVERRIDE_SHADER, "ShaderOverridePickVS"));
}

static DWORD WINAPI client(LPVOID)
{
	HANDLE h = INVALID_HANDLE_VALUE;
	DWORD error = 0;

	for (int i = 0; i < 500 && h == INVALID_HANDLE_VALUE; i++) {
		h = open_pipe(&error);
		if (h == INVALID_HANDLE_VALUE)
			Sleep(10);
	}
	expect("the pipe is there after the first Present", h != INVALID_HANDLE_VALUE);
	if (h == INVALID_HANDLE_VALUE) {
		client_done = true;
		return 0;
	}
	ULONG server = 0;
	expect("the pipe's server is this process", GetNamedPipeServerProcessId(h, &server) && server == GetCurrentProcessId());

	Hello hello;
	hello.client = "pick harness";
	hello.min_version = 1;
	hello.max_version = 1;
	HelloReply info;
	bool ok = send(h, MessageType::Hello, 1, hello) && receive_as(h, MessageType::HelloReply, 1, &info);
	expect("HelloReply", ok);
	expect("HelloReply: version 1, this process, D3D11, VARS and PICK_D3D11", info.version == 1 && info.pid == GetCurrentProcessId()
		&& info.render_api == RENDER_D3D11 && info.capabilities == (CAPABILITY_VARS | CAPABILITY_PICK_D3D11));
	expect("HelloReply: the importer folder is the run folder", !_stricmp(info.importer_folder.c_str(), run_folder.c_str()));
	printf("  DLL version %s, exe %s\n", info.dll_version.c_str(), info.exe_path.c_str());

	DWORD second_error;
	HANDLE second = open_pipe(&second_error);
	expect("a second client is refused", second == INVALID_HANDLE_VALUE && second_error == ERROR_PIPE_BUSY);
	if (second != INVALID_HANDLE_VALUE)
		CloseHandle(second);

	Ping ping;
	ping.nonce = 0x1122334455667788ull;
	Pong pong;
	expect("Ping is answered with its nonce", send(h, MessageType::Ping, 2, ping) && receive_as(h, MessageType::Pong, 2, &pong)
		&& pong.nonce == ping.nonce);

	// Let the mods load at the first configuration reload
	Sleep(1000);

	GetVars get;
	get.names = { "$\\Mods\\pick\\mod.ini\\test", "$\\global\\missing" };
	VarsReply vars;
	ok = send(h, MessageType::GetVars, 3, get) && receive_as(h, MessageType::VarsReply, 3, &vars);
	expect("GetVars: the mod's variable and a missing one", ok && vars.values.size() == 2 && vars.values[0].exists
		&& vars.values[0].value == (scene_number ? 5 : 2) && !vars.values[1].exists);
	SetVars set;
	set.vars = { { "$\\Mods\\pick\\mod.ini\\test", 5 }, { "$\\global\\missing", 1 } };
	VarsApplied applied;
	ok = send(h, MessageType::SetVars, 4, set) && receive_as(h, MessageType::VarsApplied, 4, &applied);
	expect("SetVars: applied, the missing one not created", ok && applied.values.size() == 2 && applied.values[0].exists
		&& applied.values[0].value == 5 && !applied.values[1].exists && applied.frame > 0);
	ok = send(h, MessageType::GetVars, 5, get) && receive_as(h, MessageType::VarsReply, 5, &vars);
	expect("GetVars after SetVars", ok && vars.values.size() == 2 && vars.values[0].value == 5);
	if (test_mode == TestMode::LIMITS)
		check_variable_limits(h);
	if (test_mode == TestMode::RECONNECT)
		check_reconnect(&h);
	if (test_mode == TestMode::BACKPRESSURE)
		check_reply_backpressure(h);

	if (test_mode == TestMode::SHADER_UAV) {
		PickArm arm;
		arm.cursor = 1;
		arm.x = 0.35f;
		arm.y = 0.5f;
		PickArmed armed;
		PickFailed failed;
		ok = send(h, MessageType::PickArm, 6, arm) && receive_as(h, MessageType::PickArmed, 6, &armed)
			&& receive_as(h, MessageType::PickFailed, 0, &failed);
		expect("draws with vertex UAV reads and writes are excluded from picking", ok
			&& failed.capture_id == armed.capture_id && failed.code == "NO_ELIGIBLE_DRAW"
			&& failed.detail.find("shader UAV") != std::string::npos);
	} else {
		PickArm arm;
		arm.cursor = 1;
		arm.x = 0.5f;
		arm.y = 0.5f;
		PickArmed armed;
		ok = send(h, MessageType::PickArm, 6, arm) && send(h, MessageType::PickArm, 7, arm)
			&& receive_as(h, MessageType::PickArmed, 6, &armed) && error_reply(h, 7, "BUSY");
		expect("a PickArm during a pick is answered BUSY", ok);
		for (int i = 0; ok && i < 50; i++) {
			Received r;
			ok = receive(h, &r);
			if (r.header.type == (uint16_t)MessageType::PickResult || r.header.type == (uint16_t)MessageType::PickFailed)
				break;
		}
		next_id = 10;

		PickResult result;
		// x -0.3, y 0 in clip space
		ok = pick(h, 0.35f, 0.5f, &result);
		expect("pick at the overlap", ok);
		if (ok) {
			dump(result);
			check_overlap(result);
		}
		if (test_mode == TestMode::INDIRECT) {
			ok = pick(h, 0.7125f, 0.5f, &result);
			expect("pick at the first indirect mod part", ok);
			if (ok) {
				dump(result);
				check_mod(result, true);
			}
		}
		// x 0.75: the second part of the mod
		ok = pick(h, 0.875f, 0.5f, &result);
		expect("pick at the second mod part", ok);
		if (ok) {
			dump(result);
			check_mod(result);
		}
		// y 0.9: nothing drawn there
		ok = pick(h, 0.5f, 0.05f, &result);
		expect("pick at empty space: a result without hits", ok && result.hits.empty() && result.records.empty() && result.hit_total == 0);
		// x -0.5, y 0.9: only the ALWAYS pass, at the cleared depth
		ok = pick(h, 0.25f, 0.05f, &result);
		bool tested = false;
		for (const Hit &hit : result.hits)
			tested = tested || (hit.flags & HIT_DEPTH_PASSED);
		expect("pick at empty space under the ALWAYS pass: a hit with depth testing off and no depth-tested hit",
			ok && result.records.size() == 1 && result.records[0].shaders[0].hash == vs_post_hash && result.hit_total == 1 && !tested);
	}
	CloseHandle(h);

	// The server listens again, refuses another version, answers nothing
	// before Hello and drops a malformed frame
	for (int i = 0; i < 100 && (h = open_pipe(&error)) == INVALID_HANDLE_VALUE; i++)
		Sleep(10);
	expect("reconnects after a disconnect", h != INVALID_HANDLE_VALUE);
	if (h != INVALID_HANDLE_VALUE) {
		std::string detail;
		expect("NOT_READY before Hello", send(h, MessageType::Ping, 20, ping) && error_reply(h, 20, "NOT_READY"));
		hello.min_version = 2;
		hello.max_version = 3;
		expect("UNSUPPORTED_VERSION with the DLL's range", send(h, MessageType::Hello, 21, hello)
			&& error_reply(h, 21, "UNSUPPORTED_VERSION", &detail) && detail == "1-1");
		Received r;
		expect("the connection is closed after UNSUPPORTED_VERSION", !receive(h, &r));
		CloseHandle(h);
	}
	for (int i = 0; i < 100 && (h = open_pipe(&error)) == INVALID_HANDLE_VALUE; i++)
		Sleep(10);
	if (h != INVALID_HANDLE_VALUE) {
		hello.min_version = 1;
		hello.max_version = 2;
		ok = send(h, MessageType::Hello, 30, hello) && receive_as(h, MessageType::HelloReply, 30, &info);
		expect("a version range around 1 gets version 1", ok && info.version == 1);
		uint8_t bad[HEADER_SIZE] = { 8, 0, 0, 0, 3, 0, 1, 0, 31, 0, 0, 0 };
		DWORD n;
		WriteFile(h, bad, sizeof(bad), &n, NULL);
		expect("a frame with flags set is answered MALFORMED", error_reply(h, 0, "MALFORMED"));
		Received r;
		expect("and the connection is closed", !receive(h, &r));
		CloseHandle(h);
	}

	client_done = true;
	return 0;
}

// ---------------------------------------------------------------- the game

static void write_mod_ini(const std::string &folder)
{
	std::string dir = folder + "\\Mods\\pick";
	CreateDirectoryA((folder + "\\Mods").c_str(), NULL);
	CreateDirectoryA(dir.c_str(), NULL);
	FILE *f = fopen((dir + "\\mod.ini").c_str(), "w");
	if (!f) {
		printf("cannot write %s\\mod.ini\n", dir.c_str());
		exit(1);
	}
	fprintf(f, "[Constants]\nglobal $test = 2\n\n");
	if (test_mode == TestMode::BACKPRESSURE) {
		for (unsigned i = 0; i < 256; i++)
			fprintf(f, "global $pressure%u = 0\n", i);
	}
	fprintf(f, "[ShaderOverridePickVS]\nhash = %016llx\ncheckTextureOverride = ib\n\n", vs_mod_hash);
	fprintf(f, "[TextureOverridePickIB]\nhash = %08x\n", test_mode == TestMode::REGION ? mod_ib_region_hash : mod_ib_hash);
	if (test_mode == TestMode::REGION)
		fprintf(f, "match_index_count = 12\n");
	if (test_mode == TestMode::INDIRECT) {
		fprintf(f, "handling = skip\nResourceSavedVB = ref vb0\nvb0 = ResourceFirstPartVB\n"
			"drawinstancedindirect = ResourceArgs, 0\nvb0 = ref ResourceSavedVB\n"
			"drawindexedinstancedindirect = ResourceArgs, 16\n\n"
			"[ResourceSavedVB]\n\n[ResourceFirstPartVB]\ntype = Buffer\nstride = 12\nformat = R32_FLOAT\n"
			"data = 0.3 -0.5 0.5 0.3 0.5 0.5 0.55 0.5 0.5 0.3 -0.5 0.5 0.55 0.5 0.5 0.55 -0.5 0.5\n\n"
			"[ResourceArgs]\ntype = Buffer\nformat = R32_UINT\ndata = 6 1 0 0 6 1 6 0 0\n");
	} else {
		fprintf(f, "handling = skip\ndrawindexed = 6, 0, 0\ndrawindexed = 6, 6, 0\n");
	}
	fclose(f);
}

struct State {
	ID3D11RenderTargetView *rtv;
	ID3D11DepthStencilView *dsv;
	ID3D11DepthStencilState *dss;
	ID3D11PixelShader *ps;
	ID3D11Buffer *cb;
	ID3D11UnorderedAccessView *uav;
	UINT cb_first, cb_count;
};

// What the pick may have touched has to be as the game left it
static unsigned check_state(const State &want)
{
	ID3D11RenderTargetView *rtvs[2] = {};
	ID3D11DepthStencilView *dsv = NULL;
	ID3D11UnorderedAccessView *uavs[D3D11_1_UAV_SLOT_COUNT - 1] = {};
	UINT uav_count = test_mode == TestMode::UAV || test_mode == TestMode::SHADER_UAV ? D3D11_1_UAV_SLOT_COUNT - 1 : 7;
	ID3D11DepthStencilState *dss = NULL;
	UINT ref = 0;
	ID3D11PixelShader *ps = NULL;
	ID3D11Buffer *cb = NULL;
	UINT cb_first = 0, cb_count = 0;
	unsigned bad = 0;

	ctx->OMGetRenderTargets(2, rtvs, &dsv);
	ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, NULL, NULL, 1, uav_count, uavs);
	ctx->OMGetDepthStencilState(&dss, &ref);
	ctx->PSGetShader(&ps, NULL, NULL);
	ctx1->PSGetConstantBuffers1(13, 1, &cb, &cb_first, &cb_count);
	bad += rtvs[0] != want.rtv || rtvs[1] || dsv != want.dsv;
	for (UINT i = 0; i < uav_count; i++) {
		bad += uavs[i] != (i + 1 == 12 ? want.uav : NULL);
		if (uavs[i])
			uavs[i]->Release();
	}
	bad += dss != want.dss || ref != 5 || ps != want.ps || cb != want.cb;
	bad += cb_first != want.cb_first || cb_count != want.cb_count;
	for (IUnknown *u : { (IUnknown*)rtvs[0], (IUnknown*)rtvs[1], (IUnknown*)dsv, (IUnknown*)dss, (IUnknown*)ps, (IUnknown*)cb }) {
		if (u)
			u->Release();
	}
	return bad;
}

static int run_scene(int argc, char **argv)
{
	if (argc < 2) {
		printf("pick.exe <run folder>\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	crc_init();
	char full[MAX_PATH];
	GetFullPathNameA(argv[1], MAX_PATH, full, NULL);
	run_folder = full;
	while (!run_folder.empty() && run_folder.back() == '\\')
		run_folder.pop_back();

	ID3DBlob *vs_o0_blob = compile("vs_o0", "vs_5_0"), *vs_o3_blob = compile("vs_o3", "vs_5_0"), *vs_mod_blob = compile("vs_mod", "vs_5_0");
	ID3DBlob *vs_post_blob = compile("vs_post", "vs_5_0");
	ID3DBlob *ps0_blob = compile("ps0", "ps_5_0"), *ps3_blob = compile("ps3", "ps_5_0");
	vs_o0_hash = fnv(vs_o0_blob->GetBufferPointer(), vs_o0_blob->GetBufferSize());
	vs_o3_hash = fnv(vs_o3_blob->GetBufferPointer(), vs_o3_blob->GetBufferSize());
	vs_mod_hash = fnv(vs_mod_blob->GetBufferPointer(), vs_mod_blob->GetBufferSize());
	vs_post_hash = fnv(vs_post_blob->GetBufferPointer(), vs_post_blob->GetBufferSize());
	ps0_hash = fnv(ps0_blob->GetBufferPointer(), ps0_blob->GetBufferSize());

	// The mod's index buffer, as make_mesh() will make it
	{
		uint16_t indices[] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
		D3D11_BUFFER_DESC desc = { sizeof(indices), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
		mod_ib_hash = buffer_hash(indices, sizeof(indices), desc);
		mod_ib_region_hash = crc32c(0, indices, sizeof(indices));
	}
	write_mod_ini(run_folder);

	HMODULE lib = LoadLibraryA((run_folder + "\\d3d11.dll").c_str());
	if (!lib) {
		printf("cannot load the DLL: %lu\n", GetLastError());
		return 1;
	}
	CreateFn create = (CreateFn)GetProcAddress(lib, "D3D11CreateDeviceAndSwapChain");

	WNDCLASSW wc = {};
	wc.lpfnWndProc = window_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"pick-harness";
	RegisterClassW(&wc);
	RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	HWND window = CreateWindowW(wc.lpszClassName, L"pick-harness", WS_OVERLAPPEDWINDOW, 40, 40,
			rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);
	show_harness_window(window);

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferDesc.Width = WIDTH;
	scd.BufferDesc.Height = HEIGHT;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.OutputWindow = window;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	D3D_FEATURE_LEVEL level = test_mode == TestMode::UAV || test_mode == TestMode::SHADER_UAV
		? D3D_FEATURE_LEVEL_11_1 : D3D_FEATURE_LEVEL_11_0;
	IDXGISwapChain *swap;
	D3D_DRIVER_TYPE driver = test_mode == TestMode::BASIC ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
	CHECK(create(NULL, driver, NULL, 0, &level, 1, D3D11_SDK_VERSION, &scd, &swap, &dev, NULL, &ctx));
	CHECK(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1));
	IDXGISwapChain *second_swap = NULL;
	ID3D11Device *second_device = NULL;
	ID3D11DeviceContext *second_context = NULL;
	HWND second_window = NULL;
	if (test_mode == TestMode::MULTISWAP) {
		RECT second_rect = { 0, 0, (LONG)WIDTH / 2, (LONG)HEIGHT / 2 };
		AdjustWindowRect(&second_rect, WS_OVERLAPPEDWINDOW, FALSE);
		second_window = CreateWindowW(wc.lpszClassName, L"pick-secondary", WS_OVERLAPPEDWINDOW, 40, 40,
			second_rect.right - second_rect.left, second_rect.bottom - second_rect.top, NULL, NULL, wc.hInstance, NULL);
		DXGI_SWAP_CHAIN_DESC second_desc = scd;
		second_desc.BufferDesc.Width /= 2;
		second_desc.BufferDesc.Height /= 2;
		second_desc.OutputWindow = second_window;
		if (scene_number) {
			CHECK(create(NULL, driver, NULL, 0, &level, 1, D3D11_SDK_VERSION, &second_desc,
				&second_swap, &second_device, NULL, &second_context));
		} else {
			IDXGIDevice *dxgi_device;
			IDXGIAdapter *adapter;
			IDXGIFactory *factory;
			CHECK(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device));
			CHECK(dxgi_device->GetAdapter(&adapter));
			CHECK(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory));
			CHECK(factory->CreateSwapChain(dev, &second_desc, &second_swap));
			factory->Release();
			adapter->Release();
			dxgi_device->Release();
		}
	}

	ID3D11VertexShader *vs_o0, *vs_o3, *vs_mod, *vs_post;
	ID3D11PixelShader *ps0, *ps3;
	CHECK(dev->CreateVertexShader(vs_o0_blob->GetBufferPointer(), vs_o0_blob->GetBufferSize(), NULL, &vs_o0));
	CHECK(dev->CreateVertexShader(vs_o3_blob->GetBufferPointer(), vs_o3_blob->GetBufferSize(), NULL, &vs_o3));
	CHECK(dev->CreateVertexShader(vs_mod_blob->GetBufferPointer(), vs_mod_blob->GetBufferSize(), NULL, &vs_mod));
	CHECK(dev->CreateVertexShader(vs_post_blob->GetBufferPointer(), vs_post_blob->GetBufferSize(), NULL, &vs_post));
	CHECK(dev->CreatePixelShader(ps0_blob->GetBufferPointer(), ps0_blob->GetBufferSize(), NULL, &ps0));
	CHECK(dev->CreatePixelShader(ps3_blob->GetBufferPointer(), ps3_blob->GetBufferSize(), NULL, &ps3));
	D3D11_INPUT_ELEMENT_DESC element = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
	ID3D11InputLayout *layout;
	CHECK(dev->CreateInputLayout(&element, 1, vs_o0_blob->GetBufferPointer(), vs_o0_blob->GetBufferSize(), &layout));

	Mesh cloth = make_mesh({ -0.6f, 0.0f, -0.5f, 0.5f }, 0.3f);
	Mesh body = make_mesh({ -0.8f, 0.2f, -0.7f, 0.7f },
		test_mode == TestMode::COPLANAR || test_mode == TestMode::STENCIL ? 0.3f : 0.6f);
	Mesh mod = make_mesh({ 0.3f, 0.55f, -0.5f, 0.5f, 0.6f, 0.9f, -0.5f, 0.5f }, 0.5f);
	Mesh post = make_mesh({ -1.0f, -0.05f, -1.0f, 1.0f }, 1.0f);
	Mesh queried = make_mesh({ -0.55f, -0.05f, -0.45f, 0.45f }, 0.6f);
	if (mod.ib_hash != mod_ib_hash) {
		printf("the mod's index buffer hash differs from mod.ini's\n");
		return 1;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = WIDTH;
	td.Height = HEIGHT;
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	ID3D11Texture2D *scene, *scene_staging, *depth, *other;
	CHECK(dev->CreateTexture2D(&td, NULL, &scene));
	CHECK(dev->CreateTexture2D(&td, NULL, &other));
	td.BindFlags = 0;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	CHECK(dev->CreateTexture2D(&td, NULL, &scene_staging));
	td.Usage = D3D11_USAGE_DEFAULT;
	td.CPUAccessFlags = 0;
	td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	CHECK(dev->CreateTexture2D(&td, NULL, &depth));
	ID3D11RenderTargetView *scene_rtv, *other_rtv;
	ID3D11DepthStencilView *dsv;
	CHECK(dev->CreateRenderTargetView(scene, NULL, &scene_rtv));
	CHECK(dev->CreateRenderTargetView(other, NULL, &other_rtv));
	CHECK(dev->CreateDepthStencilView(depth, NULL, &dsv));

	uint32_t texels[16];
	for (uint32_t &t : texels)
		t = 0xffc0c0c0;
	td = {};
	td.Width = td.Height = 4;
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA tinit = { texels, 16, 0 };
	ID3D11Texture2D *tex;
	ID3D11ShaderResourceView *srv;
	CHECK(dev->CreateTexture2D(&td, &tinit, &tex));
	CHECK(dev->CreateShaderResourceView(tex, NULL, &srv));

	float tint[4] = { 1, 1, 1, 1 };
	D3D11_BUFFER_DESC cbd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA cbi = { tint };
	ID3D11Buffer *tint_cb;
	CHECK(dev->CreateBuffer(&cbd, &cbi, &tint_cb));

	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = TRUE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dd.DepthFunc = D3D11_COMPARISON_LESS;
	if (test_mode == TestMode::STENCIL) {
		dd.StencilEnable = TRUE;
		dd.StencilReadMask = dd.StencilWriteMask = 0xff;
		dd.FrontFace.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
		dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR_SAT;
		dd.BackFace = dd.FrontFace;
	}
	ID3D11DepthStencilState *depth_state, *always_state;
	CHECK(dev->CreateDepthStencilState(&dd, &depth_state));
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
	CHECK(dev->CreateDepthStencilState(&dd, &always_state));
	D3D11_BLEND_DESC bd = {};
	ID3D11BlendState *no_color;
	CHECK(dev->CreateBlendState(&bd, &no_color));
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	ID3D11RasterizerState *raster;
	CHECK(dev->CreateRasterizerState(&rd, &raster));

	ID3D11DeviceContext *deferred;
	CHECK(dev->CreateDeferredContext(0, &deferred));
	ID3D11Query *occlusion, *statistics, *disjoint;
	D3D11_QUERY_DESC query_desc = { D3D11_QUERY_OCCLUSION, 0 };
	CHECK(dev->CreateQuery(&query_desc, &occlusion));
	query_desc.Query = D3D11_QUERY_PIPELINE_STATISTICS;
	CHECK(dev->CreateQuery(&query_desc, &statistics));
	query_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
	CHECK(dev->CreateQuery(&query_desc, &disjoint));
	ID3D11Predicate *predicate = NULL;
	BOOL predicate_value = TRUE;
	if (test_mode == TestMode::PREDICATION) {
		D3D11_QUERY_DESC predicate_desc = { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
		CHECK(dev->CreatePredicate(&predicate_desc, &predicate));
		ctx->Begin(predicate);
		ctx->End(predicate);
		HRESULT hr;
		while ((hr = ctx->GetData(predicate, &predicate_value, sizeof(predicate_value), 0)) == S_FALSE)
			Sleep(0);
		CHECK(hr);
		expect("an empty occlusion predicate is false", predicate_value == FALSE);
	}
	ID3D11UnorderedAccessView *high_uav = NULL;
	ID3D11Buffer *uav_buffer = NULL, *uav_staging = NULL;
	if (test_mode == TestMode::UAV || test_mode == TestMode::SHADER_UAV) {
		D3D11_BUFFER_DESC uav_desc = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS,
			0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4 };
		CHECK(dev->CreateBuffer(&uav_desc, NULL, &uav_buffer));
		CHECK(dev->CreateUnorderedAccessView(uav_buffer, NULL, &high_uav));
		if (test_mode == TestMode::SHADER_UAV) {
			uav_desc.Usage = D3D11_USAGE_STAGING;
			uav_desc.BindFlags = uav_desc.MiscFlags = 0;
			uav_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			CHECK(dev->CreateBuffer(&uav_desc, NULL, &uav_staging));
		}
	}

	DWORD idle = argc > 3 && !strcmp(argv[2], "idle") ? strtoul(argv[3], NULL, 10) * 1000 : 0;
	HANDLE thread = idle ? NULL : CreateThread(NULL, 0, client, NULL, 0, NULL);
	std::vector<uint32_t> sums;
	unsigned state_failures = 0;
	unsigned query_failures = 0;
	unsigned uav_failures = 0;
	unsigned predicate_failures = 0;
	std::vector<UINT> vertex_invocations;
	DWORD start = GetTickCount();
	D3D11_VIEWPORT vp = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 };
	UINT stride = 12, offset = 0;
	State want = { scene_rtv, dsv, depth_state, NULL, tint_cb, high_uav,
		0, test_mode == TestMode::CBZERO ? 0u : D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT };

	deferred->OMSetRenderTargets(1, &other_rtv, NULL);
	deferred->RSSetViewports(1, &vp);
	deferred->IASetInputLayout(layout);
	deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	deferred->IASetVertexBuffers(0, 1, &cloth.vb, &stride, &offset);
	deferred->IASetIndexBuffer(cloth.ib, DXGI_FORMAT_R16_UINT, 0);
	deferred->VSSetShader(vs_o3, NULL, 0);
	deferred->PSSetShader(ps3, NULL, 0);
	deferred->DrawIndexed(6, 0, 0);
	ID3D11CommandList *list;
	CHECK(deferred->FinishCommandList(FALSE, &list));

	while (!client_done) {
		if (idle && GetTickCount() - start > idle)
			break;
		if (!idle && GetTickCount() - start > 30000) {
			printf("FAIL the client did not finish in 30 s\n");
			return 2;
		}

		ctx->ExecuteCommandList(list, FALSE);
		if (uav_staging) {
			const UINT values[4] = { 1, 0, 0, 0 };
			ctx->ClearUnorderedAccessViewUint(high_uav, values);
		}
		if (test_mode == TestMode::REGION) {
			ctx->UpdateSubresource(cloth.vb, 0, NULL, cloth_vertices, 0, 0);
			ctx->UpdateSubresource(cloth.ib, 0, NULL, cloth_indices, 0, 0);
		}
		ctx->Begin(disjoint);

		float sky[4] = { 0.4f, 0.6f, 0.9f, 1 };
		ctx->ClearRenderTargetView(scene_rtv, sky);
		ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
		ctx->OMSetRenderTargets(1, &scene_rtv, dsv);
		if (high_uav)
			ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
				NULL, NULL, 12, 1, &high_uav, NULL);
		ctx->OMSetDepthStencilState(depth_state, 5);
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(raster);
		ctx->IASetInputLayout(layout);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		if (test_mode == TestMode::CBZERO)
			ctx1->PSSetConstantBuffers1(13, 1, &tint_cb, &want.cb_first, &want.cb_count);
		else
			ctx->PSSetConstantBuffers(13, 1, &tint_cb);
		ctx->PSSetShaderResources(0, 1, &srv);

		struct { Mesh *mesh; ID3D11VertexShader *vs; ID3D11PixelShader *ps; } draws[] = {
			{ &cloth, vs_o3, ps3 }, { &body, vs_o0, ps0 }, { &mod, vs_mod, ps0 },
		};
		for (auto &d : draws) {
			ctx->IASetVertexBuffers(0, 1, &d.mesh->vb, &stride, &offset);
			ctx->IASetIndexBuffer(d.mesh->ib, DXGI_FORMAT_R16_UINT, 0);
			ctx->VSSetShader(d.vs, NULL, 0);
			ctx->PSSetShader(d.ps, NULL, 0);
			ctx->DrawIndexed(d.mesh->indices, 0, 0);
			if (test_mode == TestMode::REGION && d.mesh == &cloth) {
				float empty_vertices[12] = {};
				uint16_t empty_indices[6] = {};
				ctx->UpdateSubresource(cloth.vb, 0, NULL, empty_vertices, 0, 0);
				ctx->UpdateSubresource(cloth.ib, 0, NULL, empty_indices, 0, 0);
			}
			want.ps = d.ps;
			state_failures += check_state(want);
		}
		if (predicate) {
			ctx->SetPredication(predicate, predicate_value);
			ctx->IASetVertexBuffers(0, 1, &cloth.vb, &stride, &offset);
			ctx->IASetIndexBuffer(cloth.ib, DXGI_FORMAT_R16_UINT, 0);
			ctx->VSSetShader(vs_o3, NULL, 0);
			ctx->PSSetShader(ps3, NULL, 0);
			ctx->DrawIndexed(cloth.indices, 0, 0);
			ID3D11Predicate *bound = NULL;
			BOOL value;
			ctx->GetPredication(&bound, &value);
			predicate_failures += bound != predicate || value != predicate_value;
			if (bound)
				bound->Release();
			ctx->SetPredication(NULL, FALSE);
			ctx->PSSetShader(want.ps, NULL, 0);
		}
		ctx->IASetVertexBuffers(0, 1, &post.vb, &stride, &offset);
		ctx->IASetIndexBuffer(post.ib, DXGI_FORMAT_R16_UINT, 0);
		ctx->VSSetShader(vs_post, NULL, 0);
		ctx->OMSetDepthStencilState(always_state, 5);
		ctx->OMSetBlendState(no_color, NULL, 0xffffffff);
		ctx->DrawIndexed(post.indices, 0, 0);
		want.dss = always_state;
		state_failures += check_state(want);
		want.dss = depth_state;
		ctx->OMSetBlendState(NULL, NULL, 0xffffffff);

		ctx->OMSetDepthStencilState(depth_state, 5);
		ctx->IASetVertexBuffers(0, 1, &queried.vb, &stride, &offset);
		ctx->IASetIndexBuffer(queried.ib, DXGI_FORMAT_R16_UINT, 0);
		ctx->VSSetShader(vs_o0, NULL, 0);
		ctx->PSSetShader(ps0, NULL, 0);
		ctx->Begin(occlusion);
		ctx->Begin(statistics);
		ctx->DrawIndexed(queried.indices, 0, 0);
		ctx->End(occlusion);
		ctx->DrawIndexed(queried.indices, 0, 0);
		ctx->End(statistics);
		ctx->End(disjoint);
		if (uav_staging) {
			ctx->CopyResource(uav_staging, uav_buffer);
			D3D11_MAPPED_SUBRESOURCE uav_map;
			CHECK(ctx->Map(uav_staging, 0, D3D11_MAP_READ, 0, &uav_map));
			const UINT *values = (const UINT*)uav_map.pData;
			uav_failures += values[0] != 1;
			vertex_invocations.push_back(values[1]);
			ctx->Unmap(uav_staging, 0);
		}

		ctx->CopyResource(scene_staging, scene);
		D3D11_MAPPED_SUBRESOURCE map;
		CHECK(ctx->Map(scene_staging, 0, D3D11_MAP_READ, 0, &map));
		uint32_t sum = 0;
		for (UINT y = 0; y < HEIGHT; y++)
			sum = crc32c(sum, (uint8_t*)map.pData + y * map.RowPitch, WIDTH * 4);
		ctx->Unmap(scene_staging, 0);
		sums.push_back(sum);
		UINT64 samples = 0;
		D3D11_QUERY_DATA_PIPELINE_STATISTICS counts = {};
		HRESULT query_hr;
		while ((query_hr = ctx->GetData(occlusion, &samples, sizeof(samples), 0)) == S_FALSE)
			Sleep(0);
		CHECK(query_hr);
		while ((query_hr = ctx->GetData(statistics, &counts, sizeof(counts), 0)) == S_FALSE)
			Sleep(0);
		CHECK(query_hr);
		query_failures += samples != 0 || counts.IAVertices != 12 || counts.IAPrimitives != 4;

		if (predicate)
			ctx->SetPredication(predicate, predicate_value);
		CHECK(swap->Present(0, 0));
		if (predicate) {
			ID3D11Predicate *bound = NULL;
			BOOL value;
			ctx->GetPredication(&bound, &value);
			predicate_failures += bound != predicate || value != predicate_value;
			if (bound)
				bound->Release();
			ctx->SetPredication(NULL, FALSE);
		}
		if (second_swap)
			CHECK(second_swap->Present(0, 0));
	}
	list->Release();
	if (thread)
		WaitForSingleObject(thread, INFINITE);

	expect("the output merger, pixel shader, b13 buffer and range, and depth state are preserved after every draw", state_failures == 0);
	bool same = true;
	for (uint32_t s : sums)
		same = same && s == sums[0];
	printf("  %zu frames\n", sums.size());
	expect("every frame has the same picture, pick or no pick", same);
	expect("picking preserves occlusion and overlapping pipeline statistics query results", query_failures == 0);
	if (predicate) {
		expect("picking preserves predication after draws and Present", predicate_failures == 0);
		predicate->Release();
	}
	if (uav_staging) {
		for (UINT count : vertex_invocations)
			uav_failures += count == 0 || count != vertex_invocations[0];
		expect("picking preserves vertex UAV data and invocation counts", uav_failures == 0);
		uav_staging->Release();
	}
	if (uav_buffer)
		uav_buffer->Release();
	ctx1->Release();
	if (test_mode == TestMode::DEVICES || test_mode == TestMode::MULTISWAP)
		expect("the primary swap chain is released before the next scene", swap->Release() == 0);
	if (second_swap)
		second_swap->Release();
	if (second_context)
		second_context->Release();
	if (second_device)
		second_device->Release();
	if (second_window)
		DestroyWindow(second_window);

	printf("%ld failures\n", failures);
	return failures ? 2 : 0;
}

int main(int argc, char **argv)
{
	if (argc > 2) {
		if (!strcmp(argv[2], "uav"))
			test_mode = TestMode::UAV;
		else if (!strcmp(argv[2], "region"))
			test_mode = TestMode::REGION;
		else if (!strcmp(argv[2], "devices"))
			test_mode = TestMode::DEVICES;
		else if (!strcmp(argv[2], "cbzero"))
			test_mode = TestMode::CBZERO;
		else if (!strcmp(argv[2], "indirect"))
			test_mode = TestMode::INDIRECT;
		else if (!strcmp(argv[2], "shader-uav"))
			test_mode = TestMode::SHADER_UAV;
		else if (!strcmp(argv[2], "limits"))
			test_mode = TestMode::LIMITS;
		else if (!strcmp(argv[2], "coplanar"))
			test_mode = TestMode::COPLANAR;
		else if (!strcmp(argv[2], "stencil"))
			test_mode = TestMode::STENCIL;
		else if (!strcmp(argv[2], "reconnect"))
			test_mode = TestMode::RECONNECT;
		else if (!strcmp(argv[2], "multiswap"))
			test_mode = TestMode::MULTISWAP;
		else if (!strcmp(argv[2], "predication"))
			test_mode = TestMode::PREDICATION;
		else if (!strcmp(argv[2], "backpressure"))
			test_mode = TestMode::BACKPRESSURE;
	}
	unsigned scenes = test_mode == TestMode::DEVICES || test_mode == TestMode::MULTISWAP ? 2 : 1;
	for (scene_number = 0; scene_number < scenes; scene_number++) {
		client_done = false;
		next_id = 1;
		printf("Scene %u\n", scene_number + 1);
		int result = run_scene(argc, argv);
		if (result)
			return result;
	}
	return 0;
}
