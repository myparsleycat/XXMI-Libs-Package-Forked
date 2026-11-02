// decode.exe <GFSDK_Aftermath_Lib.x64.dll> <dump.nv-gpudmp>: writes the dump as
// JSON to <dump>.json.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// The SDK headers stay out of the tree, as in GpuWatch.cpp.
static const uint32_t AFTERMATH_VERSION = 0x21B;
static const uint32_t AFTERMATH_SUCCESS = 1;
static const uint32_t DECODE_ALL_INFO = 0x3FFF;
static const uint32_t FORMAT_UTF8 = 2;

typedef uint32_t (*CreateDecoder)(uint32_t version, const void *dump, uint32_t size, void **decoder);
typedef uint32_t (*GenerateJson)(void *decoder, uint32_t decoder_flags, uint32_t format_flags, void *debug_info, void *shader, void *source_debug_info, void *user, uint32_t *size);
typedef uint32_t (*GetJson)(void *decoder, uint32_t size, char *json);

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: decode <GFSDK_Aftermath_Lib.x64.dll> <dump.nv-gpudmp>\n");
		return 2;
	}

	HMODULE library = LoadLibraryA(argv[1]);
	if (!library) {
		fprintf(stderr, "cannot load %s: error %lu\n", argv[1], GetLastError());
		return 1;
	}
	CreateDecoder create = (CreateDecoder)GetProcAddress(library, "GFSDK_Aftermath_GpuCrashDump_CreateDecoder");
	GenerateJson generate = (GenerateJson)GetProcAddress(library, "GFSDK_Aftermath_GpuCrashDump_GenerateJSON");
	GetJson get = (GetJson)GetProcAddress(library, "GFSDK_Aftermath_GpuCrashDump_GetJSON");
	if (!create || !generate || !get) {
		fprintf(stderr, "the library lacks the decoding functions\n");
		return 1;
	}

	std::ifstream in(argv[2], std::ios::binary);
	std::vector<char> dump((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (dump.empty()) {
		fprintf(stderr, "cannot read %s\n", argv[2]);
		return 1;
	}

	void *decoder = NULL;
	uint32_t result = create(AFTERMATH_VERSION, dump.data(), (uint32_t)dump.size(), &decoder);
	if (result != AFTERMATH_SUCCESS) {
		fprintf(stderr, "CreateDecoder: 0x%x\n", result);
		return 1;
	}
	uint32_t size = 0;
	result = generate(decoder, DECODE_ALL_INFO, FORMAT_UTF8, NULL, NULL, NULL, NULL, &size);
	if (result != AFTERMATH_SUCCESS) {
		fprintf(stderr, "GenerateJSON: 0x%x\n", result);
		return 1;
	}
	std::vector<char> json(size);
	result = get(decoder, size, json.data());
	if (result != AFTERMATH_SUCCESS) {
		fprintf(stderr, "GetJSON: 0x%x\n", result);
		return 1;
	}

	std::string out_path = std::string(argv[2]) + ".json";
	std::ofstream(out_path, std::ios::binary).write(json.data(), strlen(json.data()));
	printf("%s\n", out_path.c_str());
	return 0;
}
