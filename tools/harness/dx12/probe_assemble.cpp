#include "../../../D3D_Shaders/stdafx.h"
#include "../../../log.h"

FILE *LogFile = NULL;
LogVerbosity gLogVerbosity = LogVerbosity::DISABLED;
bool gLogDebug = false;

static vector<char> read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file) return {};
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	vector<char> bytes(size > 0 ? size : 0);
	if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
	fclose(file);
	return bytes;
}

int main()
{
	vector<char> original = read_file("probe-dxbc.bin");
	vector<char> assembly = read_file("probe-dxbc.txt");
	if (original.empty() || assembly.empty()) return 1;
	string source(assembly.begin(), assembly.end());
	size_t pos = source.rfind("\nret");
	if (pos == string::npos) { printf("DXBC return instruction missing\n"); return 1; }
	pos++;
	source.insert(pos, "mov o0.x, l(1.000000)\n");
	assembly.assign(source.begin(), source.end());
	vector<byte> bytecode(original.begin(), original.end());
	vector<AssemblerParseError> errors;
	vector<byte> patched = assembler(&assembly, bytecode, &errors);
	if (patched.empty() || !errors.empty()) {
		for (const auto &error : errors) printf("%s\n", error.what());
		return 1;
	}
	FILE *file = fopen("probe-dxbc-patched.bin", "wb");
	if (!file) return 1;
	bool ok = fwrite(patched.data(), 1, patched.size(), file) == patched.size();
	fclose(file);
	return ok ? 0 : 1;
}
