// Checks the desktop channel codec of the DLL (DirectX11/desktop/Protocol.*)
// against the vectors both repositories keep (testdata/desktop-channel/v1).
// For each .bin it decodes the frame, encodes the result again and compares
// the bytes, then prints the decoded message as JSON in the shape of the .json
// next to it, one line a vector, for vectors.py to compare.
//
//   vectors.exe <vector folder>
//
// Exits with 1 when a vector does not decode or encodes to other bytes.

#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../../../../../DirectX11/desktop/Protocol.h"
#include "json.h"

using namespace DesktopProtocol;

static std::vector<uint8_t> read_file(const std::string &path)
{
	std::vector<uint8_t> data;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return data;
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + n);
	fclose(f);
	return data;
}

template <class M> static bool check(const char *name, MessageType type, const FrameHeader &header,
		const std::vector<uint8_t> &frame, std::string *json)
{
	M message;
	if (!DecodePayload(frame.data() + HEADER_SIZE, header.payload_size, &message)) {
		printf("FAIL %s: does not decode\n", name);
		return false;
	}
	std::vector<uint8_t> again;
	if (!EncodeFrame(type, header.request_id, message, &again)) {
		printf("FAIL %s: does not encode\n", name);
		return false;
	}
	if (again != frame) {
		printf("FAIL %s: encodes to %zu other bytes\n", name, again.size());
		return false;
	}
	*json = ToJson(message);
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("vectors.exe <vector folder>\n");
		return 1;
	}
	std::string dir = argv[1];
	WIN32_FIND_DATAA fd;
	HANDLE find = FindFirstFileA((dir + "\\*.bin").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE) {
		printf("FAIL no vectors in %s\n", argv[1]);
		return 1;
	}

	int failures = 0, vectors = 0;
	do {
		std::string file = fd.cFileName;
		std::vector<uint8_t> frame = read_file(dir + "\\" + file);
		FrameHeader header;
		vectors++;
		if (frame.size() < HEADER_SIZE || !ParseHeader(frame.data(), &header) || header.payload_size != frame.size() - HEADER_SIZE) {
			printf("FAIL %s: bad frame header\n", file.c_str());
			failures++;
			continue;
		}

		std::string json, type_name;
		bool ok = false;
		const char *name = file.c_str();
		switch ((MessageType)header.type) {
#define CASE(T, N) case MessageType::T: type_name = N; ok = check<T>(name, MessageType::T, header, frame, &json); break
			CASE(Hello, "Hello");
			CASE(HelloReply, "HelloReply");
			CASE(Ping, "Ping");
			CASE(Pong, "Pong");
			CASE(Error, "ErrorReply");
			CASE(GetVars, "GetVars");
			CASE(VarsReply, "VarsReply");
			CASE(SetVars, "SetVars");
			CASE(VarsApplied, "VarsApplied");
			CASE(Event, "Event");
			CASE(PickArm, "PickArm");
			CASE(PickArmed, "PickArmed");
			CASE(PickResult, "PickResult");
			CASE(PickFailed, "PickFailed");
#undef CASE
		default:
			printf("FAIL %s: type 0x%04x unknown\n", name, header.type);
		}
		if (!ok) {
			failures++;
			continue;
		}
		printf("{\"file\":\"%s\",\"type\":\"%s\",\"requestId\":%u,\"message\":%s}\n",
			name, type_name.c_str(), header.request_id, json.c_str());
	} while (FindNextFileA(find, &fd));
	FindClose(find);

	// Frames a receiver has to refuse
	uint8_t bad[HEADER_SIZE] = {};
	FrameHeader header;
	uint32_t length = 7;
	memcpy(bad, &length, 4);
	if (ParseHeader(bad, &header)) {
		printf("FAIL a length below 8 is accepted\n");
		failures++;
	}
	length = MAX_FRAME_SIZE - 3;
	memcpy(bad, &length, 4);
	if (ParseHeader(bad, &header)) {
		printf("FAIL a frame over 4 MiB is accepted\n");
		failures++;
	}
	length = 8;
	memcpy(bad, &length, 4);
	bad[6] = 1;
	if (ParseHeader(bad, &header)) {
		printf("FAIL nonzero flags are accepted\n");
		failures++;
	}
	uint8_t trailing[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
	Ping ping;
	if (DecodePayload(trailing, sizeof(trailing), &ping)) {
		printf("FAIL a payload with a trailing byte is accepted\n");
		failures++;
	}
	VarsReply reply;
	uint8_t vars[] = { 1, 0, 1, 0, 'a', 2, 0, 0, 0, 0 };
	if (DecodePayload(vars, sizeof(vars), &reply)) {
		printf("FAIL a bool of 2 is accepted\n");
		failures++;
	}
	uint8_t bad_utf8[] = { 2, 0, 0xc0, 0x80, 1, 0, 1, 0 };
	Hello hello;
	if (DecodePayload(bad_utf8, sizeof(bad_utf8), &hello)) {
		printf("FAIL an overlong UTF-8 form is accepted\n");
		failures++;
	}

	fprintf(stderr, "%d vectors, %d failures\n", vectors, failures);
	return failures ? 1 : 0;
}
