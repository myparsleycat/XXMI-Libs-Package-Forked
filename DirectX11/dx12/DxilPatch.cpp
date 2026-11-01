#include "DxilPatch.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "Dxc.h"
#include "HashDump.h"
#include "util.h"
#include "../globals.h"

namespace {

static std::string metadata_line(const std::string &text, unsigned id)
{
	std::string key = "!" + std::to_string(id) + " = ";
	size_t start = text.find(key), end;

	if (start == std::string::npos)
		return "";
	end = text.find('\n', start);
	return text.substr(start, end == std::string::npos ? end : end - start);
}

static std::vector<unsigned> metadata_refs(const std::string &line)
{
	std::vector<unsigned> refs;
	size_t pos;

	for (pos = line.find("!{"); pos != std::string::npos && pos < line.size(); pos = line.find('!', pos + 1)) {
		char *end;
		unsigned long id;
		if (pos + 1 >= line.size() || !isdigit((unsigned char)line[pos + 1]))
			continue;
		id = strtoul(line.c_str() + pos + 1, &end, 10);
		refs.push_back((unsigned)id);
		pos = end - line.c_str() - 1;
	}
	return refs;
}

static std::vector<unsigned> metadata_i32(const std::string &line)
{
	std::vector<unsigned> values;
	size_t pos;

	for (pos = line.find("i32 "); pos != std::string::npos; pos = line.find("i32 ", pos + 4))
		values.push_back((unsigned)strtoul(line.c_str() + pos + 4, NULL, 10));
	return values;
}

static bool add_texture(std::string *text, unsigned reg, unsigned dimension, unsigned *range_id, std::string *why)
{
	size_t root_start, root_end, first_srv, pos, end;
	unsigned resources_id, srv_list_id = 0, highest_id = 0;
	std::string root_line, resources_line, srv_line;
	std::vector<unsigned> root_refs, srv_refs;
	const unsigned kind = dimension == D3D_SRV_DIMENSION_TEXTURE1D ? 1 : 2;
	bool has_srv_table = false;

	root_start = text->find("!dx.resources = !{");
	if (root_start == std::string::npos || (root_end = text->find('\n', root_start)) == std::string::npos) {
		*why = "the DXIL resource metadata was not found";
		return false;
	}
	root_line = text->substr(root_start, root_end - root_start);
	root_refs = metadata_refs(root_line);
	if (root_refs.empty()) {
		*why = "the DXIL resource metadata has no resource list";
		return false;
	}
	resources_id = root_refs[0];
	resources_line = metadata_line(*text, resources_id);
	pos = resources_line.find("!{");
	first_srv = pos == std::string::npos ? pos : resources_line.find_first_not_of(" \t", pos + 2);
	if (first_srv == std::string::npos) {
		*why = "the DXIL resource list is malformed";
		return false;
	}
	if (resources_line[first_srv] == '!' && first_srv + 1 < resources_line.size()
	    && isdigit((unsigned char)resources_line[first_srv + 1])) {
		srv_list_id = (unsigned)strtoul(resources_line.c_str() + first_srv + 1, NULL, 10);
		srv_line = metadata_line(*text, srv_list_id);
		if (srv_line.empty()) {
			*why = "the DXIL SRV table could not be read";
			return false;
		}
		has_srv_table = true;
		srv_refs = metadata_refs(srv_line);
	} else if (resources_line.compare(first_srv, 4, "null") != 0) {
		*why = "the DXIL SRV table reference is malformed";
		return false;
	}

	for (unsigned id : srv_refs) {
		std::string entry = metadata_line(*text, id);
		std::vector<unsigned> fields = metadata_i32(entry);
		if (fields.size() < 5)
			continue;
		highest_id = (std::max)(highest_id, fields[0]);
		if (fields[1] || reg < fields[2] || reg - fields[2] >= fields[3])
			continue;
		if (fields[4] != kind) {
			*why = "t" + std::to_string(reg) + " is already declared with a different resource dimension";
			return false;
		}
		*range_id = fields[0];
		return true;
	}

	unsigned new_id = 0;
	for (pos = 0; (pos = text->find('!', pos)) != std::string::npos; pos++) {
		if (pos + 1 < text->size() && isdigit((unsigned char)(*text)[pos + 1])) {
			unsigned id = (unsigned)strtoul(text->c_str() + pos + 1, NULL, 10);
			new_id = (std::max)(new_id, id + 1);
		}
	}
	*range_id = srv_refs.empty() ? 0 : highest_id + 1;
	unsigned resource_id = new_id++, component_id = new_id++;
	if (!has_srv_table)
		srv_list_id = new_id++;

	std::string type_name = kind == 1 ? "Texture1D" : "Texture2D";
	std::string type = "%\"class." + type_name + "<vector<float, 4> >\"";
	std::string mips_type = "%\"class." + type_name + "<vector<float, 4> >::mips_type\"";
	std::string definitions;
	if (text->find("%dx.types.ResRet.f32 = type") == std::string::npos)
		definitions += "%dx.types.ResRet.f32 = type { float, float, float, float, i32 }\n";
	if (text->find(type + " = type") == std::string::npos)
		definitions += type + " = type { <4 x float>, " + mips_type + " }\n";
	if (text->find(mips_type + " = type") == std::string::npos)
		definitions += mips_type + " = type { i32 }\n";
	if (!definitions.empty()) {
		pos = text->find("define ");
		if (pos == std::string::npos) {
			*why = "the DXIL shader has no function";
			return false;
		}
		text->insert(pos, definitions + "\n");
	}

	if (has_srv_table) {
		pos = srv_line.rfind('}');
		if (pos == std::string::npos) {
			*why = "the DXIL SRV metadata list is malformed";
			return false;
		}
		srv_line.insert(pos, (srv_refs.empty() ? "" : ", ") + std::string("!") + std::to_string(resource_id));
		pos = text->find("!" + std::to_string(srv_list_id) + " = ");
		end = text->find('\n', pos);
		text->replace(pos, end == std::string::npos ? end : end - pos, srv_line);
	} else {
		resources_line.replace(first_srv, 4, "!" + std::to_string(srv_list_id));
		pos = text->find("!" + std::to_string(resources_id) + " = ");
		end = text->find('\n', pos);
		text->replace(pos, end == std::string::npos ? end : end - pos, resources_line);
	}

	*text += "\n!" + std::to_string(resource_id) + " = !{i32 " + std::to_string(*range_id)
		+ ", " + type + "* undef, !\"\", i32 0, i32 " + std::to_string(reg)
		+ ", i32 1, i32 " + std::to_string(kind) + ", i32 0, !" + std::to_string(component_id) + "}\n"
		+ "!" + std::to_string(component_id) + " = !{i32 0, i32 9}\n";
	if (!has_srv_table)
		*text += "!" + std::to_string(srv_list_id) + " = !{!" + std::to_string(resource_id) + "}\n";

	return true;
}

static bool add_shader_flags(std::string *text, UINT64 flags, std::string *why)
{
	size_t at = text->find("!dx.entryPoints = !{"), end, token;
	std::vector<unsigned> refs;
	std::string entry, properties;
	unsigned entry_id, properties_id = 0;
	if (at == std::string::npos || (end = text->find('\n', at)) == std::string::npos)
		goto malformed;
	refs = metadata_refs(text->substr(at, end - at));
	if (refs.empty())
		goto malformed;
	entry_id = refs[0];
	entry = metadata_line(*text, entry_id);
	token = entry.find("!{");
	if (token == std::string::npos)
		goto malformed;
	for (int field = 0; field < 4; field++) {
		token = entry.find(',', token + 1);
		if (token == std::string::npos)
			goto malformed;
	}
	token = entry.find_first_not_of(" \t", token + 1);
	if (token == std::string::npos)
		goto malformed;
	if (entry.compare(token, 4, "null") == 0) {
		for (size_t scan = 0; (scan = text->find('!', scan)) != std::string::npos; scan++)
			if (scan + 1 < text->size() && isdigit((unsigned char)(*text)[scan + 1]))
				properties_id = (std::max)(properties_id, (unsigned)strtoul(text->c_str() + scan + 1, NULL, 10) + 1);
		entry.replace(token, 4, "!" + std::to_string(properties_id));
		at = text->find("!" + std::to_string(entry_id) + " = ");
		end = text->find('\n', at);
		text->replace(at, end == std::string::npos ? end : end - at, entry);
		*text += "\n!" + std::to_string(properties_id) + " = !{i32 0, i64 " + std::to_string(flags) + "}\n";
		return true;
	}
	if (entry[token] != '!' || token + 1 >= entry.size() || !isdigit((unsigned char)entry[token + 1]))
		goto malformed;
	properties_id = (unsigned)strtoul(entry.c_str() + token + 1, NULL, 10);
	properties = metadata_line(*text, properties_id);
	if (properties.empty())
		goto malformed;
	at = properties.find("i32 0, i64 ");
	if (at == std::string::npos) {
		at = properties.rfind('}');
		if (at == std::string::npos)
			goto malformed;
		properties.insert(at, (properties.find("!{}") != std::string::npos ? "" : ", ") + std::string("i32 0, i64 ") + std::to_string(flags));
	} else {
		at += strlen("i32 0, i64 ");
		char *number_end;
		UINT64 value = _strtoui64(properties.c_str() + at, &number_end, 10);
		if (number_end == properties.c_str() + at)
			goto malformed;
		properties.replace(at, number_end - (properties.c_str() + at), std::to_string(value | flags));
	}
	at = text->find("!" + std::to_string(properties_id) + " = ");
	end = text->find('\n', at);
	text->replace(at, end == std::string::npos ? end : end - at, properties);
	return true;
malformed:
	*why = "the DXIL entry point flags could not be read";
	return false;
}

static bool add_aux_resource(std::string *text, unsigned reg, bool sampler, unsigned *range_id, std::string *why)
{
	size_t pos = text->find("!dx.resources = !{"), end;
	unsigned resources_id, table_id = 0, highest = 0, next_id = 0;
	std::string root, resources, table;
	std::vector<unsigned> refs, entries;
	bool has_table = false;
	const char *type = sampler ? "%struct.SamplerState" : "%struct.RWByteAddressBuffer";
	const unsigned kind = sampler ? 0 : 11;
	if (pos == std::string::npos || (end = text->find('\n', pos)) == std::string::npos)
		goto malformed;
	root = text->substr(pos, end - pos);
	refs = metadata_refs(root);
	if (refs.empty())
		goto malformed;
	resources_id = refs[0];
	resources = metadata_line(*text, resources_id);
	pos = resources.find("!{");
	for (unsigned field = 0; field < (sampler ? 3u : 1u) && pos != std::string::npos; field++)
		pos = resources.find(',', pos + 1);
	if (pos == std::string::npos || (pos = resources.find_first_not_of(" \t", pos + 1)) == std::string::npos)
		goto malformed;
	if (resources[pos] == '!' && pos + 1 < resources.size() && isdigit((unsigned char)resources[pos + 1])) {
		table_id = (unsigned)strtoul(resources.c_str() + pos + 1, NULL, 10);
		table = metadata_line(*text, table_id);
		if (table.empty())
			goto malformed;
		has_table = true;
		entries = metadata_refs(table);
	} else if (resources.compare(pos, 4, "null")) {
		goto malformed;
	}
	for (unsigned id : entries) {
		std::vector<unsigned> fields = metadata_i32(metadata_line(*text, id));
		if (fields.size() < 5)
			continue;
		highest = (std::max)(highest, fields[0]);
		if (!fields[1] && reg >= fields[2] && reg - fields[2] < fields[3]) {
			if (fields[4] != kind) {
				*why = std::string(sampler ? "s" : "u") + std::to_string(reg) + " is already declared with a different resource kind";
				return false;
			}
			*range_id = fields[0];
			return true;
		}
	}
	for (size_t scan = 0; (scan = text->find('!', scan)) != std::string::npos; scan++)
		if (scan + 1 < text->size() && isdigit((unsigned char)(*text)[scan + 1]))
			next_id = (std::max)(next_id, (unsigned)strtoul(text->c_str() + scan + 1, NULL, 10) + 1);
	*range_id = entries.empty() ? 0 : highest + 1;
	{
		unsigned resource_id = next_id++;
		if (!has_table)
			table_id = next_id++;
		if (text->find(std::string(type) + " = type") == std::string::npos) {
			size_t function = text->find("define ");
			if (function == std::string::npos)
				goto malformed;
			text->insert(function, std::string(type) + " = type { i32 }\n\n");
		}
		if (has_table) {
			size_t brace = table.rfind('}');
			if (brace == std::string::npos)
				goto malformed;
			table.insert(brace, (entries.empty() ? "" : ", ") + std::string("!") + std::to_string(resource_id));
			size_t at = text->find("!" + std::to_string(table_id) + " = ");
			size_t line_end = text->find('\n', at);
			text->replace(at, line_end == std::string::npos ? line_end : line_end - at, table);
		} else {
			resources.replace(pos, 4, "!" + std::to_string(table_id));
			size_t at = text->find("!" + std::to_string(resources_id) + " = ");
			size_t line_end = text->find('\n', at);
			text->replace(at, line_end == std::string::npos ? line_end : line_end - at, resources);
			*text += "\n!" + std::to_string(table_id) + " = !{!" + std::to_string(resource_id) + "}\n";
		}
		*text += "\n!" + std::to_string(resource_id) + " = !{i32 " + std::to_string(*range_id)
			+ ", " + type + "* undef, !\"\", i32 0, i32 " + std::to_string(reg)
			+ (sampler ? ", i32 1, i32 0, null}\n" : ", i32 1, i32 11, i1 false, i1 false, i1 false, null}\n");
	}
	return sampler || add_shader_flags(text, 16, why);
malformed:
	*why = sampler ? "the DXIL sampler metadata could not be read" : "the DXIL UAV resource metadata could not be read";
	return false;
}

static bool add_handle(std::string *text, unsigned reg, unsigned range_id, bool uav, bool sampler, std::string *why)
{
	std::string name = std::string("%zz_d3dmigoto_") + (sampler ? "s" : uav ? "u" : "t") + std::to_string(reg);
	std::string entry;
	size_t start, end, brace, line_end, at;

	if (text->find(name + " =") != std::string::npos)
		return true;
	start = text->find("!dx.entryPoints = !{");
	if (start != std::string::npos && (end = text->find('\n', start)) != std::string::npos) {
		std::vector<unsigned> refs = metadata_refs(text->substr(start, end - start));
		if (!refs.empty()) {
			std::string metadata = metadata_line(*text, refs[0]);
			at = metadata.find('@');
			if (at != std::string::npos) {
				end = at + 1;
				if (end < metadata.size() && metadata[end] == '"') {
					for (end++; end < metadata.size() && (metadata[end] != '"' || metadata[end - 1] == '\\'); end++);
					if (end < metadata.size())
						end++;
				} else {
					while (end < metadata.size() && (isalnum((unsigned char)metadata[end]) || strchr("-.$_", metadata[end])))
						end++;
				}
				entry = metadata.substr(at, end - at);
			}
		}
	}
	if (entry.empty()) {
		*why = "the DXIL entry point was not found";
		return false;
	}
	for (start = text->find("define "); start != std::string::npos; start = text->find("define ", brace + 1)) {
		brace = text->find('{', start);
		at = text->find(entry, start);
		if (brace == std::string::npos || at == std::string::npos || at >= brace
		    || at + entry.size() >= text->size() || (*text)[at + entry.size()] != '(')
			continue;
		break;
	}
	if (start == std::string::npos || brace == std::string::npos) {
		*why = "the DXIL entry point was not found";
		return false;
	}
	line_end = text->find('\n', brace);
	if (line_end == std::string::npos) {
		*why = "the DXIL entry point body is malformed";
		return false;
	}
	text->insert(line_end + 1, "  " + name + " = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 " + (sampler ? "3" : uav ? "1" : "0") + ", i32 "
		+ std::to_string(range_id) + ", i32 " + std::to_string(reg) + ", i1 false)\n");
	return true;
}

static bool add_declaration(std::string *text, const DxilDeclaration &declaration, std::string *why)
{
	unsigned range_id;
	if (declaration.uav || declaration.sampler ? !add_aux_resource(text, declaration.reg, declaration.sampler, &range_id, why)
	                    : !add_texture(text, declaration.reg, declaration.dimension, &range_id, why))
		return false;
	return add_handle(text, declaration.reg, range_id, declaration.uav, declaration.sampler, why);
}

static bool local_id(const std::string &line, size_t at, size_t *end, std::string *key)
{
	size_t pos = at + 1;
	if (at >= line.size() || line[at] != '%' || pos >= line.size())
		return false;
	if (isdigit((unsigned char)line[pos])) {
		while (pos < line.size() && isdigit((unsigned char)line[pos]))
			pos++;
		*key = "n" + line.substr(at + 1, pos - at - 1);
	} else if (line.compare(pos, 3, "zz_") == 0) {
		pos += 3;
		while (pos < line.size() && (isalnum((unsigned char)line[pos]) || line[pos] == '_' || line[pos] == '.'))
			pos++;
		*key = "z" + line.substr(at + 1, pos - at - 1);
	} else {
		return false;
	}
	*end = pos;
	return true;
}

static void map_local_ids(const std::string &line, std::map<std::string, unsigned> *ids, unsigned *next)
{
	size_t pos = line.find_first_not_of(" \t"), end;
	std::string key;
	if (pos == std::string::npos || !local_id(line, pos, &end, &key))
		return;
	pos = line.find_first_not_of(" \t", end);
	if (pos == std::string::npos || line[pos] != '=')
		return;
	if (!ids->count(key))
		(*ids)[key] = (*next)++;
}

// DXC's textual assembler requires consecutive numeric IDs. `%zz_*` in a
// replacement is a local SSA placeholder; this pass renumbers it together
// with the existing values and basic blocks after insertion.
static bool renumber_ids(std::string *text, std::string *why)
{
	std::vector<std::string> lines;
	for (size_t start = 0, end; start < text->size(); start = end + 1) {
		end = text->find('\n', start);
		if (end == std::string::npos)
			end = text->size();
		lines.push_back(text->substr(start, end - start));
	}

	for (size_t first = 0; first < lines.size(); first++) {
		if (lines[first].compare(0, 7, "define "))
			continue;
		size_t last = first + 1;
		while (last < lines.size() && lines[last] != "}")
			last++;
		if (last == lines.size()) {
			*why = "a DXIL function body is not closed";
			return false;
		}

		std::map<std::string, unsigned> ids;
		unsigned next = 0;
		// Numeric arguments precede the unnamed entry block in DXIL's value numbering.
		size_t header_end = lines[first].find('{');
		for (size_t at = lines[first].find('@'); header_end != std::string::npos && at < header_end;) {
			at = lines[first].find('%', at + 1);
			if (at == std::string::npos || at >= header_end)
				break;
			size_t end = at + 1;
			std::string key;
			if (local_id(lines[first], at, &end, &key) && !ids.count(key))
				ids[key] = next++;
			at = (std::max)(at + 1, end);
		}
		ids["n" + std::to_string(next)] = next++;
		for (size_t line = first + 1; line < last; line++) {
			size_t label = lines[line].find("; <label>:");
			if (label != std::string::npos) {
				const char *begin = lines[line].c_str() + label + 10;
				if (isdigit((unsigned char)*begin)) {
					unsigned old = (unsigned)strtoul(begin, NULL, 10);
					std::string key = "n" + std::to_string(old);
					if (!ids.count(key))
						ids[key] = next++;
				}
			}
			map_local_ids(lines[line], &ids, &next);
		}

		for (size_t line = first; line <= last; line++) {
			std::string result;
			for (size_t at = 0; at < lines[line].size();) {
				if (lines[line][at] == ';' && lines[line].compare(at, 10, "; <label>:") == 0) {
					result.append(lines[line], at, 10);
					at += 10;
					size_t end = at;
					while (end < lines[line].size() && isdigit((unsigned char)lines[line][end]))
						end++;
					std::string key = "n" + lines[line].substr(at, end - at);
					auto found = ids.find(key);
					result += found == ids.end() ? lines[line].substr(at, end - at) : std::to_string(found->second);
					at = end;
					continue;
				}
				size_t end;
				std::string key;
				if (local_id(lines[line], at, &end, &key)) {
					auto found = ids.find(key);
					result += found == ids.end() ? lines[line].substr(at, end - at) : "%" + std::to_string(found->second);
					at = end;
				} else {
					result += lines[line][at++];
				}
			}
			lines[line].swap(result);
		}
		first = last;
	}

	text->clear();
	for (const std::string &line : lines)
		*text += line + "\n";
	return true;
}

static bool group_matches_stage(const ShaderRegexGroup *group, const Dx12ShaderInfo *shader)
{
	const char *stage = dx12_stage_names[shader->stage];
	size_t length = strlen(stage);
	return std::any_of(group->shader_models.begin(), group->shader_models.end(), [&](const std::string &model) {
		return model.compare(0, length, stage) == 0 && model.size() > length && model[length] == '_';
	});
}

struct CacheEntry {
	bool processed = false;
	DxilPatchOutcome outcome = DxilPatchOutcome::NO_MATCH;
	Dx12ShaderInfo shader = {};
	std::vector<BYTE> bytecode;
	std::vector<std::wstring> groups;
	std::string why;
};

static std::map<std::pair<UINT64, uint32_t>, CacheEntry> cache;
static std::mutex cache_mutex;
static uint32_t cache_hash;

} // namespace

DxilPatchOutcome DxilPatchShader(const Dx12ShaderInfo *shader, const std::vector<const ShaderRegexGroup*> &groups,
		DxilPatched *patched, std::string *why)
{
	std::string original, text, errors;
	*patched = DxilPatched();
	why->clear();
	if (!shader || !shader->dxil || !shader->bytecode || !shader->size)
		return DxilPatchOutcome::NO_MATCH;
	if (groups.empty())
		return DxilPatchOutcome::NO_MATCH;
	if (!Dx12DxcDisassemble(shader->bytecode, shader->size, &original, &errors)) {
		*why = "could not be disassembled: " + errors;
		return DxilPatchOutcome::FAILED;
	}
	original.erase(std::remove(original.begin(), original.end(), '\r'), original.end());
	text = original;

	std::string declaration_failure, pattern_failure;
	for (const ShaderRegexGroup *group : groups) {
		std::string saved = text;
		UINT64 saved_srv[2] = { patched->srv[0], patched->srv[1] };
		UINT64 saved_uav = patched->uav;
		UINT saved_samplers = patched->samplers;
		BYTE saved_dimensions[128];
		memcpy(saved_dimensions, patched->srv_dimension, sizeof(saved_dimensions));
		bool match = true;

		for (const DxilDeclaration &declaration : group->dxil_declarations) {
			std::string declaration_error;
			if (!add_declaration(&text, declaration, &declaration_error)) {
				text = saved;
				patched->srv[0] = saved_srv[0];
				patched->srv[1] = saved_srv[1];
				patched->uav = saved_uav;
				patched->samplers = saved_samplers;
				memcpy(patched->srv_dimension, saved_dimensions, sizeof(saved_dimensions));
				declaration_failure = declaration_error;
				match = false;
				break;
			}
		}
		if (!match)
			continue;

		for (const auto &entry : group->dxil_patterns) {
			const ShaderRegexPattern &pattern = entry.second;
			match = pattern.do_replace ? pattern.patch_with(&text, pattern.replace) : pattern.matches(&text);
			if (!match) {
				if (pattern_failure.empty())
					pattern_failure = "unmatched " + std::string(entry.first.begin(), entry.first.end());
				break;
			}
		}
		if (!match) {
			text = saved;
			patched->srv[0] = saved_srv[0];
			patched->srv[1] = saved_srv[1];
			patched->uav = saved_uav;
			patched->samplers = saved_samplers;
			memcpy(patched->srv_dimension, saved_dimensions, sizeof(saved_dimensions));
			continue;
		}
		for (const DxilDeclaration &declaration : group->dxil_declarations) {
			if (declaration.sampler)
				patched->samplers |= 1u << declaration.reg;
			else if (declaration.uav)
				patched->uav |= 1ull << declaration.reg;
			else {
				patched->srv[declaration.reg / 64] |= 1ull << (declaration.reg % 64);
				patched->srv_dimension[declaration.reg] = (BYTE)declaration.dimension;
			}
		}
		patched->groups.push_back(group);
	}

	if (patched->groups.empty())
		if (!declaration_failure.empty()) {
			*why = declaration_failure;
			return DxilPatchOutcome::FAILED;
		} else {
			*why = pattern_failure;
			return DxilPatchOutcome::NO_MATCH;
		}
	if (text == original)
		return DxilPatchOutcome::MATCH;
	// A replacement can write an existing game UAV without declaring a new
	// one, so the release guard must cover the original shader as well.
	if (patched->uav || shader->has_uav || !shader->reflected) {
		*why = "writable DXIL UAVs are not enabled in this build";
		return DxilPatchOutcome::FAILED;
	}
	// ResourceDescriptorHeap/SamplerDescriptorHeap access has no ordinary
	// binding metadata that can prove whether a dynamically selected resource
	// is writable. Refuse to patch such shaders conservatively.
	if (text.find("dx.op.createHandleFromHeap") != std::string::npos
	    || text.find("dx.op.createHandleForLib") != std::string::npos) {
		*why = "directly indexed DXIL descriptor heaps are not enabled in this build";
		return DxilPatchOutcome::FAILED;
	}
	if (!renumber_ids(&text, why))
		return DxilPatchOutcome::FAILED;
	if (!Dx12DxcAssemble(text, &patched->bytecode, &errors)) {
		*why = "could not be assembled or validated: " + errors;
		return DxilPatchOutcome::FAILED;
	}
	std::string assembled;
	if (!Dx12DxcDisassemble(patched->bytecode.data(), patched->bytecode.size(), &assembled, &errors)) {
		patched->bytecode.clear();
		*why = "the patched shader could not be inspected: " + errors;
		return DxilPatchOutcome::FAILED;
	}
	if (assembled.find("dx.op.createHandleFromHeap") != std::string::npos
	    || assembled.find("dx.op.createHandleForLib") != std::string::npos) {
		patched->bytecode.clear();
		*why = "the patched shader directly indexes a descriptor heap";
		return DxilPatchOutcome::FAILED;
	}
	ID3D12ShaderReflection *reflection = NULL;
	if (!Dx12DxcReflect(patched->bytecode.data(), patched->bytecode.size(), &reflection)) {
		patched->bytecode.clear();
		*why = "the patched shader requirements could not be reflected";
		return DxilPatchOutcome::FAILED;
	}
	UINT64 requirements = reflection->GetRequiresFlags();
	reflection->Release();
	if (requirements & (D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING
	    | D3D_SHADER_REQUIRES_SAMPLER_DESCRIPTOR_HEAP_INDEXING)) {
		patched->bytecode.clear();
		*why = "the patched shader directly indexes a descriptor heap";
		return DxilPatchOutcome::FAILED;
	}
	// DxilPattern.Replace is intentionally general and can alter resource
	// metadata without going through InsertDeclarations. Reflect the assembled
	// container, not just the original shader and parsed declarations.
	Dx12ShaderInfo final_info = {};
	if (!Dx12ReflectShaderResources(patched->bytecode.data(), patched->bytecode.size(), &final_info)
	    || final_info.has_uav) {
		patched->bytecode.clear();
		*why = "the patched shader has a writable UAV or could not be reflected";
		return DxilPatchOutcome::FAILED;
	}
	return DxilPatchOutcome::PATCHED;
}

const Dx12ShaderInfo* DxilGameShader(const Dx12ShaderInfo *shader, bool link_groups, const char **why)
{
	std::pair<UINT64, uint32_t> key;
	CacheEntry *entry;
	std::vector<const ShaderRegexGroup*> candidates;

	if (why)
		*why = NULL;
	if (!shader_regex_dxil_groups || !shader || !shader->dxil)
		return NULL;
	std::lock_guard<std::mutex> guard(cache_mutex);
	if (cache_hash != shader_regex_hash) {
		cache.clear();
		cache_hash = shader_regex_hash;
	}
	key = std::make_pair(shader->hash, shader_regex_hash);
	entry = &cache[key];
	if (!entry->processed) {
		for (const auto &item : shader_regex_groups) {
			const ShaderRegexGroup *group = &item.second;
			if (!group->dxil() || !group_matches_stage(group, shader))
				continue;
			if (group->dx12_match ? !group->dx12_matches(shader) : !group->patterns.empty())
				continue;
			candidates.push_back(group);
		}
		DxilPatched patched;
		entry->outcome = DxilPatchShader(shader, candidates, &patched, &entry->why);
		entry->processed = true;
		if (entry->outcome == DxilPatchOutcome::FAILED && G->dx12_dump_hashes)
			Dx12DumpNote("DXIL ShaderRegex patch failed for %016llx: %s\n", shader->hash, entry->why.c_str());
		if (entry->outcome == DxilPatchOutcome::PATCHED && G->dx12_dump_hashes)
			Dx12DumpNote("DXIL ShaderRegex patch succeeded for %016llx\n", shader->hash);
		if (entry->outcome != DxilPatchOutcome::FAILED)
			for (const ShaderRegexGroup *group : patched.groups)
				entry->groups.push_back(group->ini_section);
		if (entry->outcome == DxilPatchOutcome::PATCHED) {
			entry->bytecode.swap(patched.bytecode);
			entry->shader = *shader;
			entry->shader.hash = fnv_64_buf(entry->bytecode.data(), entry->bytecode.size());
			entry->shader.size = (UINT32)entry->bytecode.size();
			entry->shader.bytecode = entry->bytecode.data();
			entry->shader.uav |= patched.uav;
			entry->shader.samplers |= patched.samplers;
			for (UINT reg = 0; reg < 64; reg++)
				if (patched.uav & 1ull << reg)
					entry->shader.uav_dimension[reg] = D3D12_UAV_DIMENSION_BUFFER;
			for (UINT reg = 0; reg < 128; reg++) {
				if (patched.srv[reg / 64] & 1ull << (reg % 64)) {
					entry->shader.srv[reg / 64] |= 1ull << (reg % 64);
					entry->shader.srv_dimension[reg] = patched.srv_dimension[reg];
				}
			}
		}
	}

	if (link_groups) {
		for (const std::wstring &name : entry->groups) {
			auto group = shader_regex_groups.find(name);
			if (group != shader_regex_groups.end())
				group->second.link_command_lists_and_filter_index(shader->hash);
		}
	}
	if (entry->outcome == DxilPatchOutcome::FAILED && why)
		*why = entry->why.c_str();
	return entry->outcome == DxilPatchOutcome::PATCHED ? &entry->shader : NULL;
}
