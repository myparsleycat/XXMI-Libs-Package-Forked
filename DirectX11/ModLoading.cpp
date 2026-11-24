#include "ModLoading.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "globals.h"
#include "CommandList.h"
#include "Override.h"
#include "ShaderRegex.h"
#include "log.h"
#include "profiling.h"
#include "dx12/ShimDevice.h"

namespace {

// A condition of an [if], as far as its value is known when a mod is checked.
// Variables are taken at their value then, also those the frame's command
// lists assign (WWMI's $mod_enabled picks one of the mods of an object): when
// one changes, the mod is checked again. The time is not known, and an && or
// || may be known from one side.
struct Cond
{
	enum Kind { CONSTANT, VARIABLE, UNKNOWN, AND, OR, OPERATOR } kind;
	float value;
	const float *variable;
	CommandListOperator *op;
	Cond *lhs, *rhs;
	bool has_variable;
};

// The [Resource] sections a command list reaches. A condition with a variable
// the loader can read chooses a branch when the mod is checked; any other
// condition takes both.
struct LoadNode
{
	struct Branch {
		Cond *condition;
		LoadNode *if_true;
		LoadNode *if_false;
	};
	struct Resource {
		CustomResource *resource;
		// What its uses as a reference are expected to need, on top of
		// the section's own flags
		D3D11_BIND_FLAG bind_flags;
		D3D11_RESOURCE_MISC_FLAG misc_flags;
	};

	std::vector<Resource> resources;
	// Sections that load on use, whose files are read ahead
	std::vector<CustomResource*> warm;
	std::vector<LoadNode*> children;
	std::vector<Branch> branches;

	unsigned index;
};

}

struct PresentLoad
{
	std::vector<LoadNode::Resource> files;
	std::vector<CustomResource*> warm;
};

namespace {

struct ModLoadImpl : ModLoad
{
	std::wstring name;
	LoadNode *root;
	// Of the config the load belongs to, for the marks of a scan
	unsigned node_count;
	// Every section of every branch it waits for
	std::vector<LoadNode::Resource> all;
	// Files of its sections that load on use, see Builder::check_predictions
	std::vector<CustomResource*> warm;
	std::atomic<bool> warm_queued{false};
	std::vector<const PresentLoad*> present_initializers;
	std::vector<const float*> var_list;
	std::unique_ptr<std::atomic<uint32_t>[]> var_seen;
	// epoch << 1 | allowed, for the draws of an epoch to agree
	std::atomic<uint64_t> checked{0};
	std::atomic<bool> waiting{false};
};

struct Job
{
	CustomResource *resource;
	// Held
	ID3D11Device *device = nullptr;
	// Only reads the file into the file cache, for its section to load on
	// use; the section's load state is not the job's
	bool warm_only = false;
	D3D11_BIND_FLAG bind_flags;
	D3D11_RESOURCE_MISC_FLAG misc_flags;
	unsigned generation;
	CustomResourceFileLoad load;
	// Made on the loader thread for D3D12, with its contents still to be
	// copied in under the shim lock
	bool staged = false;
	// A single threaded D3D11 device: only the file is read ahead, the
	// resource is made at present
	bool create_at_commit = false;
	UINT64 file_bytes = 0;
	// The contents of a resource made without them, to be copied in at
	// presents within the commit budget: from the part's staging copy, or
	// else from load.contents
	struct Part {
		UINT subresource;
		D3D11_BOX box;
		const uint8_t *data;
		UINT row_pitch, depth_pitch;
		UINT64 bytes;
		ID3D11Resource *staging;
	};
	std::vector<Part> parts;
	size_t next_part = 0;
	// Counted in upload_bytes
	UINT64 held_bytes = 0;

	~Job();
};

// Changed only by the thread that loads the config: on a reload while it
// holds the ShaderRegex config lock exclusively, the loader thread reading
// them under that lock, and on the first load before that thread exists
unsigned generation = 1;
unsigned built_generation = 0;
std::vector<std::unique_ptr<LoadNode>> nodes;
std::vector<std::unique_ptr<Cond>> conds;
std::vector<std::unique_ptr<ModLoadImpl>> loads;
std::vector<std::unique_ptr<PresentLoad>> present_loads;
// The previous config's, kept for one more config load: a draw on another
// thread may still be looking at them while the config is being replaced
std::vector<std::unique_ptr<LoadNode>> old_nodes;
std::vector<std::unique_ptr<Cond>> old_conds;
std::vector<std::unique_ptr<ModLoadImpl>> old_loads;
std::vector<std::unique_ptr<PresentLoad>> old_present_loads;

// CustomResource::load_reach
enum Reach : unsigned char { REACH_NONE, REACH_MOD, REACH_CONFIG, REACH_FLAGS };

std::atomic<unsigned> current_epoch{1};

std::mutex queue_lock;
std::condition_variable queue_wake;
// The jobs a draw waits for, taken before the files read ahead
std::list<std::unique_ptr<Job>> waited;
std::list<std::unique_ptr<Job>> warming;
std::unordered_set<CustomResource*> warmed;
std::vector<std::unique_ptr<Job>> finished;
// Loaded, waiting for a present with commit budget left, oldest first. Only
// the presenting thread uses it.
std::vector<std::unique_ptr<Job>> to_commit;
bool worker_running;
HMODULE worker_module;

std::atomic<unsigned> stat_queued, stat_loaded, stat_loaded_on_use, stat_held;
std::atomic<LONGLONG> last_game_creation{0};

// Of the files held for their uploads: the loader thread waits while there
// are more
std::atomic<uint64_t> upload_bytes;
const uint64_t UPLOAD_BYTES_MAX = 512ull << 20;
// Of each part, and of the parts copied in at one present by
// UpdateSubresource() and from their staging copies: UpdateSubresource() of
// 16 MB there took about 6 ms (GIMI harness, NVIDIA)
const UINT64 UPLOAD_PART_BYTES = 4 << 20;
const UINT64 UPLOAD_FRAME_BYTES = 8 << 20;
const UINT64 COPY_FRAME_BYTES = 32 << 20;

Job::~Job()
{
	if (held_bytes) {
		upload_bytes -= held_bytes;
		queue_wake.notify_one();
	}
	for (Part &part : parts) {
		if (part.staging)
			part.staging->Release();
	}
	if (load.share && load.resource)
		DropProvisionalShare(load.resource);
	if (device)
		device->Release();
}
std::atomic<uint64_t> stat_load_us;

// The loader thread exits after this long without work
const DWORD WORKER_IDLE_MS = 10000;

// Whether the value of a condition is known now, in *ret
bool evaluate(const Cond *cond, float *ret)
{
	float lhs = std::numeric_limits<float>::quiet_NaN(), rhs;
	bool lhs_known = true, rhs_known;

	switch (cond->kind) {
		case Cond::CONSTANT:
			*ret = cond->value;
			return true;
		case Cond::VARIABLE:
			*ret = *cond->variable;
			return true;
		case Cond::UNKNOWN:
			return false;
		default:
			break;
	}

	rhs_known = evaluate(cond->rhs, &rhs);
	if (cond->lhs)
		lhs_known = evaluate(cond->lhs, &lhs);
	if (cond->kind == Cond::AND && ((lhs_known && !lhs) || (rhs_known && !rhs))) {
		*ret = 0;
		return true;
	}
	if (cond->kind == Cond::OR && ((lhs_known && lhs) || (rhs_known && rhs))) {
		*ret = 1;
		return true;
	}
	if (!lhs_known || !rhs_known)
		return false;
	*ret = cond->op->evaluate(lhs, rhs);
	return true;
}

std::wstring section_namespace(const std::wstring &section);

class Builder
{
public:

	LoadNode* list(CommandList *command_list);
	void add_child(LoadNode *node, LoadNode *child);
	struct Collected {
		std::vector<LoadNode::Resource> sections;
		std::vector<CustomResource*> warm;
		std::vector<const float*> vars;
		std::unordered_set<CustomResource*> unique;
	};
	void collect(LoadNode *node, std::unordered_set<LoadNode*> *seen, Collected *out);
	void check_predictions();
	const Cond* if_condition(IfCommand *branch) const;
	std::unordered_set<CustomResource*> unpredictable;
	std::unordered_map<CustomResource*, std::pair<UINT, UINT>> predicted;

private:
	struct If {
		CommandListEvaluatable *expression;
		LoadNode::Branch branch;
	};

	std::unordered_map<CommandList*, LoadNode*> lists;
	std::unordered_map<IfCommand*, If> ifs;

	LoadNode* make();
	void variables(const Cond *cond, std::vector<const float*> *vars);
	void commands(CommandList *command_list, LoadNode *node);
	void if_command(IfCommand *branch, LoadNode *node);
	void add_resource(CustomResource *resource, LoadNode *node);
	void copy(ResourceCopyOperation *op, LoadNode *node);
	void target(const ResourceCopyTarget &target, LoadNode *node);
	void expression(CommandListEvaluatable *evaluatable, LoadNode *node);
	Cond* condition(CommandListEvaluatable *evaluatable);
};

void Builder::add_resource(CustomResource *resource, LoadNode *node)
{
	node->resources.push_back({resource, (D3D11_BIND_FLAG)0, (D3D11_RESOURCE_MISC_FLAG)0});
}

LoadNode* Builder::make()
{
	nodes.push_back(std::make_unique<LoadNode>());
	return nodes.back().get();
}

void Builder::add_child(LoadNode *node, LoadNode *child)
{
	if (child && std::find(node->children.begin(), node->children.end(), child) == node->children.end())
		node->children.push_back(child);
}

LoadNode* Builder::list(CommandList *command_list)
{
	if (!command_list)
		return nullptr;
	command_list = command_list->ResolveCommandList();

	auto found = lists.find(command_list);
	if (found != lists.end())
		return found->second;

	// Entered before the commands, which may lead back here
	LoadNode *node = make();
	lists[command_list] = node;
	commands(command_list, node);
	return node;
}

static CustomResource* file_resource(const ResourceCopyTarget &target)
{
	CustomResource *resource;

	if (target.type != ResourceCopyTargetType::CUSTOM_RESOURCE || target.custom_resource_pool)
		return nullptr;
	resource = target.StaticCustomResource();
	if (!resource || resource->filename.empty() || resource->written_by_command_list
			|| resource->pool || resource->pool_index != -2)
		return nullptr;
	return resource;
}

void Builder::target(const ResourceCopyTarget &target, LoadNode *node)
{
	if (CustomResource *resource = file_resource(target))
		add_resource(resource, node);
}

void Builder::expression(CommandListEvaluatable *evaluatable, LoadNode *node)
{
	if (!evaluatable)
		return;
	if (CommandListOperator *op = dynamic_cast<CommandListOperator*>(evaluatable)) {
		expression(op->lhs.get(), node);
		expression(op->rhs.get(), node);
	} else if (CommandListOperand *operand = dynamic_cast<CommandListOperand*>(evaluatable)) {
		target(operand->texture_filter_target, node);
	}
}

void Builder::copy(ResourceCopyOperation *op, LoadNode *node)
{
	CustomResource *resource = file_resource(op->src);
	D3D11_RESOURCE_MISC_FLAG misc_flags = (D3D11_RESOURCE_MISC_FLAG)0;
	D3D11_BIND_FLAG bind_flags;

	if (!resource)
		return;
	add_resource(resource, node);

	// A reference hands the resource the flags of where it goes when it is
	// first loaded. Those of "this" are of the slot that matched the
	// section, taken to be a shader resource slot for a texture file (a use
	// that needs more makes it again, see ResourceCopyTarget::GetResource).
	// A buffer may have been matched in any slot.
	if (!(op->options & ResourceCopyOptions::REFERENCE))
		return;
	if (op->dst.type == ResourceCopyTargetType::THIS_RESOURCE) {
		switch (resource->override_type) {
			case CustomResourceType::BUFFER:
			case CustomResourceType::STRUCTURED_BUFFER:
			case CustomResourceType::RAW_BUFFER:
				unpredictable.insert(resource);
				break;
			default:
				predicted[resource].first |= D3D11_BIND_SHADER_RESOURCE;
				break;
		}
		return;
	}
	if (op->dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !op->dst.StaticCustomResource() && !op->dst.custom_resource_pool) {
		unpredictable.insert(resource);
		return;
	}
	bind_flags = op->dst.BindFlags(nullptr, &misc_flags);
	predicted[resource].first |= bind_flags;
	predicted[resource].second |= misc_flags;
}

// The uses of a section together may ask for flags no single resource can
// have, which only the first use decides between
void Builder::check_predictions()
{
	const UINT target_binds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
	const UINT address_binds = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;

	for (auto &entry : predicted) {
		CustomResource *resource = entry.first;
		UINT bind_flags = resource->bind_flags | entry.second.first;
		UINT misc_flags = resource->misc_flags | entry.second.second;

		switch (resource->override_type) {
			case CustomResourceType::BUFFER:
			case CustomResourceType::RAW_BUFFER:
				if ((misc_flags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && (bind_flags & address_binds))
					unpredictable.insert(resource);
				break;
			case CustomResourceType::STRUCTURED_BUFFER:
				if (bind_flags & address_binds)
					unpredictable.insert(resource);
				break;
			default:
				// A file texture is often block compressed
				if (bind_flags & target_binds)
					unpredictable.insert(resource);
				break;
		}
	}
}

Cond* Builder::condition(CommandListEvaluatable *evaluatable)
{
	CommandListOperator *op = dynamic_cast<CommandListOperator*>(evaluatable);
	CommandListOperand *operand = dynamic_cast<CommandListOperand*>(evaluatable);
	Cond *cond;
	float value;

	conds.push_back(std::make_unique<Cond>());
	cond = conds.back().get();
	*cond = {Cond::UNKNOWN};

	if (op && op->rhs) {
		cond->kind = op->token == L"&&" ? Cond::AND : op->token == L"||" ? Cond::OR : Cond::OPERATOR;
		cond->op = op;
		cond->lhs = op->lhs ? condition(op->lhs.get()) : nullptr;
		cond->rhs = condition(op->rhs.get());
		cond->has_variable = (cond->lhs && cond->lhs->has_variable) || cond->rhs->has_variable;
	} else if (operand) {
		switch (operand->type) {
			case ParamOverrideType::VARIABLE:
				cond->kind = Cond::VARIABLE;
				cond->variable = operand->var_ftarget;
				cond->has_variable = true;
				break;
			case ParamOverrideType::TIME:
			case ParamOverrideType::FRAME_TIME:
			case ParamOverrideType::FRAME_NUMBER:
				break;
			default:
				if (operand->static_evaluate(&value, NULL, false)) {
					cond->kind = Cond::CONSTANT;
					cond->value = value;
				}
				break;
		}
	}
	return cond;
}

const Cond* Builder::if_condition(IfCommand *branch) const
{
	auto found = ifs.find(branch);
	return found == ifs.end() ? nullptr : found->second.branch.condition;
}

void Builder::if_command(IfCommand *branch, LoadNode *node)
{
	auto found = ifs.find(branch);
	float value;

	if (found == ifs.end()) {
		If made;

		made.expression = branch->expression.evaluatable.get();
		made.branch.condition = condition(made.expression);
		made.branch.if_true = make();
		made.branch.if_false = make();
		found = ifs.emplace(branch, made).first;
		add_child(made.branch.if_true, list(branch->true_commands_pre.get()));
		add_child(made.branch.if_true, list(branch->true_commands_post.get()));
		add_child(made.branch.if_false, list(branch->false_commands_pre.get()));
		add_child(made.branch.if_false, list(branch->false_commands_post.get()));
	}
	const LoadNode::Branch &made = found->second.branch;

	expression(found->second.expression, node);
	if (branch->static_evaluated && branch->is_static) {
		node->children.push_back(branch->static_val ? made.if_true : made.if_false);
	} else if (made.condition->has_variable) {
		node->branches.push_back(made);
	} else if (evaluate(made.condition, &value)) {
		node->children.push_back(value ? made.if_true : made.if_false);
	} else {
		node->children.push_back(made.if_true);
		node->children.push_back(made.if_false);
	}
}

void Builder::commands(CommandList *command_list, LoadNode *node)
{
	for (auto &command : command_list->commands) {
		CommandListCommand *cmd = command.get();

		if (IfCommand *branch = dynamic_cast<IfCommand*>(cmd)) {
			if_command(branch, node);
		} else if (ShaderResourceBatch *batch = dynamic_cast<ShaderResourceBatch*>(cmd)) {
			for (auto &op : batch->operations) {
				ConditionalSlotCopyOperation *folded = dynamic_cast<ConditionalSlotCopyOperation*>(op.get());
				IfCommand *source = folded ? dynamic_cast<IfCommand*>(folded->source_if.get()) : nullptr;

				if (source)
					if_command(source, node);
				else if (folded)
					for (auto &taken : folded->branches)
						copy(taken.op.get(), node);
				else
					copy(op.get(), node);
			}
		} else if (ConditionalSlotCopyOperation *folded = dynamic_cast<ConditionalSlotCopyOperation*>(cmd)) {
			if (IfCommand *source = dynamic_cast<IfCommand*>(folded->source_if.get()))
				if_command(source, node);
			else
				for (auto &taken : folded->branches)
					copy(taken.op.get(), node);
		} else if (ResourceCopyOperation *op = dynamic_cast<ResourceCopyOperation*>(cmd)) {
			copy(op, node);
		} else if (SlotRangeCopyOperation *range = dynamic_cast<SlotRangeCopyOperation*>(cmd)) {
			target(range->src, node);
		} else if (RunExplicitCommandList *run = dynamic_cast<RunExplicitCommandList*>(cmd)) {
			if (run->command_list_section) {
				add_child(node, list(&run->command_list_section->command_list));
				add_child(node, list(&run->command_list_section->post_command_list));
			}
		} else if (RunLinkedCommandList *linked = dynamic_cast<RunLinkedCommandList*>(cmd)) {
			add_child(node, list(linked->link));
		} else if (RunCustomShaderCommand *shader = dynamic_cast<RunCustomShaderCommand*>(cmd)) {
			if (shader->custom_shader) {
				add_child(node, list(&shader->custom_shader->command_list));
				add_child(node, list(&shader->custom_shader->post_command_list));
			}
		} else if (CheckTextureOverrideCommand *check = dynamic_cast<CheckTextureOverrideCommand*>(cmd)) {
			// What it matches waits on its own
			target(check->target, node);
		} else if (CheckTextureOverrideBatch *checks = dynamic_cast<CheckTextureOverrideBatch*>(cmd)) {
			for (auto &merged : checks->checks)
				target(merged.command->target, node);
		} else if (DrawCommand *draw = dynamic_cast<DrawCommand*>(cmd)) {
			target(draw->indirect_buffer, node);
			for (auto &arg : draw->args)
				expression(arg.evaluatable.get(), node);
		} else if (StoreCommand *store = dynamic_cast<StoreCommand*>(cmd)) {
			target(store->src, node);
		} else if (AssignmentCommand *assignment = dynamic_cast<AssignmentCommand*>(cmd)) {
			expression(assignment->expression.evaluatable.get(), node);
		}
	}
}

void Builder::variables(const Cond *cond, std::vector<const float*> *vars)
{
	if (!cond || !cond->has_variable)
		return;
	if (cond->kind == Cond::VARIABLE) {
		if (std::find(vars->begin(), vars->end(), cond->variable) == vars->end())
			vars->push_back(cond->variable);
		return;
	}
	variables(cond->lhs, vars);
	variables(cond->rhs, vars);
}

void Builder::collect(LoadNode *node, std::unordered_set<LoadNode*> *seen, Collected *out)
{
	if (!node || !seen->insert(node).second)
		return;
	for (auto &entry : node->resources) {
		if (out->unique.insert(entry.resource).second)
			out->sections.push_back(entry);
	}
	for (CustomResource *resource : node->warm) {
		if (out->unique.insert(resource).second)
			out->warm.push_back(resource);
	}
	for (LoadNode *child : node->children)
		collect(child, seen, out);
	for (auto &branch : node->branches) {
		variables(branch.condition, &out->vars);
		collect(branch.if_true, seen, out);
		collect(branch.if_false, seen, out);
	}
}

// The namespace of a section named "<prefix>\<namespace>\<name>", or the
// whole name of one that has none
std::wstring section_namespace(const std::wstring &section)
{
	size_t first = section.find(L'\\'), last = section.rfind(L'\\');
	std::wstring ret;

	if (first == std::wstring::npos || last <= first)
		ret = section;
	else
		ret = section.substr(first + 1, last - first - 1);
	for (wchar_t &c : ret)
		c = towlower(c);
	return ret;
}

bool is_done(LONG state, unsigned epoch)
{
	return state >= MOD_LOAD_DONE && (unsigned)(state - MOD_LOAD_DONE) <= epoch;
}

void start_worker();

std::unique_ptr<Job> make_job(CustomResource *resource, ID3D11Device *device)
{
	std::unique_ptr<Job> job = std::make_unique<Job>();

	job->resource = resource;
	job->device = device;
	device->AddRef();
	job->generation = generation;
	return job;
}

void enqueue(const LoadNode::Resource &entry, ID3D11Device *device)
{
	CustomResource *resource = entry.resource;
	std::unique_ptr<Job> job;

	if (InterlockedCompareExchange(&resource->load_state, MOD_LOAD_QUEUED, MOD_LOAD_IDLE) != MOD_LOAD_IDLE)
		return;

	job = make_job(resource, device);
	job->bind_flags = (D3D11_BIND_FLAG)(resource->bind_flags | entry.bind_flags);
	job->misc_flags = (D3D11_RESOURCE_MISC_FLAG)(resource->misc_flags | entry.misc_flags);

	std::lock_guard<std::mutex> lock(queue_lock);
	waited.push_back(std::move(job));
	stat_queued++;
	start_worker();
	queue_wake.notify_one();
}

void enqueue_warm(CustomResource *resource, ID3D11Device *device)
{
	std::unique_ptr<Job> job;

	if (resource->substantiated)
		return;

	std::lock_guard<std::mutex> lock(queue_lock);
	if (!warmed.insert(resource).second)
		return;
	job = make_job(resource, device);
	job->warm_only = true;
	warming.push_back(std::move(job));
	start_worker();
	queue_wake.notify_one();
}

// The nodes one scan has entered, by LoadNode::index. A scan runs on the
// drawing thread, and several may run at once.
thread_local std::vector<unsigned> scan_marks;
thread_local unsigned scan_id;

struct Scan
{
	unsigned epoch;
	ID3D11Device *device;
	unsigned id;
};

// Whether the sections the mod needs now are there for a draw of epoch, and
// queues those not yet asked for
bool scan(LoadNode *node, const Scan &s)
{
	bool ready = true;
	float value;

	if (!node || scan_marks[node->index] == s.id)
		return true;
	scan_marks[node->index] = s.id;

	for (auto &entry : node->resources) {
		CustomResource *resource = entry.resource;
		LONG state = resource->load_state;

		if (is_done(state, s.epoch))
			continue;
		ready = false;
		if (state == MOD_LOAD_IDLE && s.device)
			enqueue(entry, s.device);
	}
	for (LoadNode *child : node->children)
		ready = scan(child, s) && ready;
	for (auto &branch : node->branches) {
		if (evaluate(branch.condition, &value)) {
			ready = scan(value ? branch.if_true : branch.if_false, s) && ready;
		} else {
			ready = scan(branch.if_true, s) && ready;
			ready = scan(branch.if_false, s) && ready;
		}
	}
	return ready;
}

bool scan_load(ModLoadImpl *load, unsigned epoch, ID3D11Device *device)
{
	Scan s = {epoch, device};
	bool ready;

	if (scan_marks.size() < load->node_count)
		scan_marks.resize(load->node_count);
	if (!++scan_id) {
		std::fill(scan_marks.begin(), scan_marks.end(), 0);
		scan_id = 1;
	}
	s.id = scan_id;
	ready = scan(load->root, s);

	if (device && !load->warm_queued.exchange(true)) {
		for (CustomResource *resource : load->warm)
			enqueue_warm(resource, device);
	}
	return ready;
}

// seen: the bits of the load's variables taken before the scan that found it
// ready
void finish_check(ModLoadImpl *load, unsigned epoch, bool ready, const uint32_t *seen)
{
	if (ready) {
		for (unsigned i = 0; i < load->var_count; i++)
			load->seen[i].store(seen[i], std::memory_order_relaxed);
		load->complete_from.store(epoch, std::memory_order_release);
	}
	load->waiting.store(!ready, std::memory_order_relaxed);
	load->checked.store((uint64_t)epoch << 1 | (ready ? 1 : 0), std::memory_order_release);
}

// Drops what reaches no [Resource] section to load: most command lists only
// set variables, and a scan need not look at them
void prune()
{
	std::vector<bool> reaches(nodes.size());
	bool changed = true;

	for (size_t i = 0; i < nodes.size(); i++)
		nodes[i]->index = (unsigned)i;

	while (changed) {
		changed = false;
		for (auto &node : nodes) {
			bool any = reaches[node->index] || !node->resources.empty() || !node->warm.empty();

			for (size_t i = 0; !any && i < node->children.size(); i++)
				any = reaches[node->children[i]->index];
			for (size_t i = 0; !any && i < node->branches.size(); i++)
				any = reaches[node->branches[i].if_true->index] || reaches[node->branches[i].if_false->index];
			if (any && !reaches[node->index]) {
				reaches[node->index] = true;
				changed = true;
			}
		}
	}

	for (auto &node : nodes) {
		auto &children = node->children;
		auto &branches = node->branches;

		if (!reaches[node->index]) {
			children.clear();
			branches.clear();
			continue;
		}
		children.erase(std::remove_if(children.begin(), children.end(), [&](LoadNode *child) {
			return !reaches[child->index];
		}), children.end());
		branches.erase(std::remove_if(branches.begin(), branches.end(), [&](LoadNode::Branch &branch) {
			if (!reaches[branch.if_true->index])
				branch.if_true = nullptr;
			if (!reaches[branch.if_false->index])
				branch.if_false = nullptr;
			return !branch.if_true && !branch.if_false;
		}), branches.end());
	}
}

std::array<CommandList*, 6> config_lists()
{
	return {&G->present_command_list, &G->post_present_command_list,
		&G->constants_command_list, &G->post_constants_command_list,
		&G->clear_rtv_command_list, &G->post_clear_rtv_command_list};
}

using ActivationLoads = std::unordered_map<const float*, ModLoadImpl*>;

// If both operands of an OR can activate their own mods, either match may
// start the load. An AND may also contain a local initialization latch.
bool activation_loads(const Cond *cond, const ActivationLoads &activations,
		std::vector<ModLoadImpl*> *owners)
{
	if (cond->kind == Cond::VARIABLE) {
		auto found = activations.find(cond->variable);
		if (found == activations.end())
			return false;
		owners->push_back(found->second);
		return true;
	}
	if (cond->kind != Cond::AND && cond->kind != Cond::OR)
		return false;

	size_t before = owners->size();
	bool lhs = activation_loads(cond->lhs, activations, owners);
	bool rhs = activation_loads(cond->rhs, activations, owners);
	if (cond->kind == Cond::OR && !(lhs && rhs)) {
		owners->resize(before);
		return false;
	}
	return lhs || rhs;
}

bool initialization_latch(const Cond *cond, const CommandListVariable *var)
{
	if (cond->kind == Cond::AND)
		return initialization_latch(cond->lhs, var) || initialization_latch(cond->rhs, var);
	return cond->kind == Cond::OPERATOR && cond->op->token == L"!" && !cond->lhs
		&& cond->rhs->kind == Cond::VARIABLE && cond->rhs->variable == &var->fval;
}

bool present_initializer(IfCommand *branch, const Cond *condition)
{
	if (!branch->pre_finalised)
		return false;
	for (auto &command : branch->true_commands_pre->ResolveCommandList()->commands) {
		if (auto *assignment = dynamic_cast<VariableAssignment*>(command.get())) {
			float value;
			if ((assignment->var->flags & VariableFlags::GLOBAL)
					|| !initialization_latch(condition, assignment->var)
					|| !assignment->expression.static_evaluate(&value) || value != 1)
				return false;
		} else if (auto *batch = dynamic_cast<ConstantAssignmentBatch*>(command.get())) {
			for (auto &assignment : batch->assignments) {
				if (!assignment.var || (assignment.var->flags & VariableFlags::GLOBAL)
						|| !initialization_latch(condition, assignment.var) || assignment.val != 1)
					return false;
			}
		} else if (!dynamic_cast<RunExplicitCommandList*>(command.get())
				&& !dynamic_cast<RunCustomShaderCommand*>(command.get())
				&& !dynamic_cast<RunLinkedCommandList*>(command.get())) {
			return false;
		}
	}
	return true;
}

void collect_present_loads(CommandList *list, Builder &builder, const ActivationLoads &activations)
{
	for (auto &command : list->ResolveCommandList()->commands) {
		auto *branch = dynamic_cast<IfCommand*>(command.get());
		if (!branch)
			continue;

		std::vector<ModLoadImpl*> owners;
		const Cond *condition = builder.if_condition(branch);
		if (condition && activation_loads(condition, activations, &owners) && present_initializer(branch, condition)) {
			std::unordered_set<LoadNode*> seen;
			Builder::Collected collected;
			builder.collect(builder.list(branch->true_commands_pre.get()), &seen, &collected);
			if (!collected.sections.empty()) {
				auto load = std::make_unique<PresentLoad>();
				load->files = std::move(collected.sections);
				load->warm = std::move(collected.warm);
				branch->present_load = load.get();
				for (auto *owner : owners) {
					auto &initializers = owner->present_initializers;
					if (std::find(initializers.begin(), initializers.end(), load.get()) == initializers.end())
						initializers.push_back(load.get());
					owner->present_pending.store(true, std::memory_order_relaxed);
				}
				for (auto &entry : load->files)
					entry.resource->load_reach = REACH_MOD;
				present_loads.push_back(std::move(load));
			}
		} else {
			collect_present_loads(branch->true_commands_pre.get(), builder, activations);
		}
		collect_present_loads(branch->false_commands_pre.get(), builder, activations);
	}
}

// Builds the waits of the current config
void build()
{
	Builder builder;
	std::unordered_map<std::wstring, ModLoadImpl*> by_namespace;
	unsigned overrides = 0, mods = 0, sections = 0;
	DWORD start = GetTickCount();


	auto new_load = [&](const std::wstring &name) {
		loads.push_back(std::make_unique<ModLoadImpl>());
		nodes.push_back(std::make_unique<LoadNode>());
		loads.back()->name = name;
		loads.back()->root = nodes.back().get();
		return loads.back().get();
	};

	auto add_texture_override = [&](TextureOverride *to) {
		std::wstring ns = section_namespace(to->ini_section);
		ModLoadImpl *&load = by_namespace[ns];

		if (!load)
			load = new_load(ns);
		builder.add_child(load->root, builder.list(&to->command_list));
		builder.add_child(load->root, builder.list(&to->post_command_list));
		to->mod_load = load;
		overrides++;
	};

	for (auto &hash : G->mTextureOverrideMap) {
		for (TextureOverride &to : hash.second)
			add_texture_override(&to);
	}
	for (auto &fuzzy : G->mFuzzyTextureOverrides) {
		if (fuzzy->texture_override && !fuzzy->texture_override->mod_load)
			add_texture_override(fuzzy->texture_override);
	}

	// The config's own command lists, which no mod's draws wait for
	nodes.push_back(std::make_unique<LoadNode>());
	LoadNode *config_root = nodes.back().get();
	for (auto &entry : G->mShaderOverrideMap) {
		builder.add_child(config_root, builder.list(&entry.second.command_list));
		builder.add_child(config_root, builder.list(&entry.second.post_command_list));
	}
	for (auto &entry : shader_regex_groups) {
		builder.add_child(config_root, builder.list(&entry.second.command_list));
		builder.add_child(config_root, builder.list(&entry.second.post_command_list));
	}
	for (CommandList *command_list : config_lists())
		builder.add_child(config_root, builder.list(command_list));

	// Sections whose flags cannot be known ahead load on use
	builder.check_predictions();
	for (auto &node : nodes) {
		auto &list = node->resources;
		list.erase(std::remove_if(list.begin(), list.end(), [&](const LoadNode::Resource &entry) {
			if (!builder.unpredictable.count(entry.resource))
				return false;
			if (std::find(node->warm.begin(), node->warm.end(), entry.resource) == node->warm.end())
				node->warm.push_back(entry.resource);
			return true;
		}), list.end());
		for (auto &entry : list) {
			auto found = builder.predicted.find(entry.resource);
			if (found != builder.predicted.end()) {
				entry.bind_flags = (D3D11_BIND_FLAG)found->second.first;
				entry.misc_flags = (D3D11_RESOURCE_MISC_FLAG)found->second.second;
			}
		}
	}

	prune();

	for (auto &load : loads) {
		std::unordered_set<LoadNode*> seen;
		Builder::Collected collected;

		load->node_count = (unsigned)nodes.size();
		builder.collect(load->root, &seen, &collected);
		load->all = std::move(collected.sections);
		load->warm = std::move(collected.warm);
		load->var_list = std::move(collected.vars);
		load->var_seen = std::make_unique<std::atomic<uint32_t>[]>(load->var_list.size());
		load->vars = load->var_list.data();
		load->seen = load->var_seen.get();
		load->var_count = (unsigned)load->var_list.size();
		for (auto &entry : load->all)
			entry.resource->load_reach = REACH_MOD;
		for (CustomResource *resource : load->warm)
			resource->load_reach = REACH_FLAGS;
		if (load->all.empty()) {
			load->complete_from.store(1, std::memory_order_relaxed);
		} else {
			mods++;
			sections += (unsigned)load->all.size();
		}
	}

	ActivationLoads activations;
	// WWMI's registration and detection variables are namespaced globals.
	// Match their resolved addresses so a [Present] in another INI can use them.
	for (auto &entry : by_namespace) {
		for (const wchar_t *name : {L"object_detected", L"mod_enabled"}) {
			auto found = command_list_globals.find(L"$\\" + entry.first + L"\\" + name);
			if (found != command_list_globals.end())
				activations.emplace(&found->second.fval, entry.second);
		}
	}
	if (!activations.empty())
		collect_present_loads(&G->present_command_list, builder, activations);
	LogInfo("Mod loading: %Iu [Present] initialization branches wait for files\n", present_loads.size());

	unsigned config_sections = 0;
	{
		std::unordered_set<LoadNode*> seen;
		Builder::Collected collected;

		builder.collect(config_root, &seen, &collected);
		for (auto &entry : collected.sections) {
			if (!entry.resource->load_reach) {
				entry.resource->load_reach = REACH_CONFIG;
				config_sections++;
			}
		}
		for (CustomResource *resource : collected.warm)
			resource->load_reach = REACH_FLAGS;
	}

	LogInfo("Mod loading: %u of %u mods wait for %u [Resource] sections, from %u TextureOverride sections; "
			"%u more load on use for the config's own command lists; in %ums\n",
			mods, (unsigned)loads.size(), sections, overrides, config_sections, GetTickCount() - start);
}

DWORD WINAPI worker_thread(LPVOID);

// With queue_lock held
void start_worker()
{
	HANDLE thread;

	if (worker_running)
		return;

	// The thread holds a reference on this DLL until it exits, so an
	// unload cannot unmap the code it is running
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&worker_thread, &worker_module))
		return;
	thread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
	if (!thread) {
		FreeLibrary(worker_module);
		return;
	}
	CloseHandle(thread);
	worker_running = true;
}

// Staging copies of the parts, so that copying them in at present leaves the
// copy in memory to the GPU. Each is made apart: a staging texture of a whole
// large file made with its contents held up the drawing thread and the game's
// resource creation for 10-20 ms (GIMI harness, NVIDIA). Parts of a block
// compressed texture that are not whole blocks get none. Returns whether every
// part has one.
bool make_staging(Job *job)
{
	ID3D11Resource *resource = job->load.resource;
	D3D11_RESOURCE_DIMENSION dimension;
	D3D11_TEXTURE2D_DESC texture = {};
	UINT block = 1;
	bool all = true;

	resource->GetType(&dimension);
	if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		((ID3D11Texture2D*)resource)->GetDesc(&texture);
		block = ModLoadTextureBlockHeight(texture.Format);
	}
	for (Job::Part &part : job->parts) {
		D3D11_SUBRESOURCE_DATA contents = {part.data, part.row_pitch, part.depth_pitch};
		UINT width = part.box.right - part.box.left, height = part.box.bottom - part.box.top;

		if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
			D3D11_BUFFER_DESC desc = {};

			desc.ByteWidth = width;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			job->device->CreateBuffer(&desc, &contents, (ID3D11Buffer**)&part.staging);
		} else if (width % block == 0 && height % block == 0) {
			D3D11_TEXTURE2D_DESC desc = texture;

			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.SampleDesc = {1, 0};
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;
			job->device->CreateTexture2D(&desc, &contents, (ID3D11Texture2D**)&part.staging);
		}
		if (!part.staging)
			all = false;
	}
	return all;
}

// Splits the contents of a resource made without them into parts of about
// UPLOAD_PART_BYTES, false for a resource it cannot
bool make_parts(Job *job)
{
	ID3D11Resource *resource = job->load.resource;
	D3D11_RESOURCE_DIMENSION dimension;

	resource->GetType(&dimension);
	if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
		const D3D11_SUBRESOURCE_DATA &contents = job->load.subresources[0];
		UINT size = contents.SysMemPitch;

		for (UINT start = 0; start < size; start += (UINT)UPLOAD_PART_BYTES) {
			UINT end = (UINT)std::min<UINT64>(size, start + UPLOAD_PART_BYTES);

			job->parts.push_back({0, {start, 0, 0, end, 1, 1}, (const uint8_t*)contents.pSysMem + start, 0, 0, end - start, nullptr});
		}
		return true;
	}
	if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		return false;

	D3D11_TEXTURE2D_DESC desc;
	UINT block;

	((ID3D11Texture2D*)resource)->GetDesc(&desc);
	block = ModLoadTextureBlockHeight(desc.Format);
	for (UINT index = 0; index < (UINT)job->load.subresources.size(); index++) {
		const D3D11_SUBRESOURCE_DATA &contents = job->load.subresources[index];
		UINT mip = index % desc.MipLevels;
		UINT width = std::max<UINT>(1, desc.Width >> mip);
		UINT height = std::max<UINT>(1, desc.Height >> mip);
		UINT rows = (height + block - 1) / block;
		UINT step = (UINT)std::max<UINT64>(1, UPLOAD_PART_BYTES / std::max<UINT>(1, contents.SysMemPitch));

		for (UINT row = 0; row < rows; row += step) {
			UINT end = std::min<UINT>(rows, row + step);

			job->parts.push_back({index, {0, row * block, 0, width, std::min<UINT>(height, end * block), 1},
					(const uint8_t*)contents.pSysMem + (size_t)row * contents.SysMemPitch,
					contents.SysMemPitch, contents.SysMemSlicePitch, (UINT64)(end - row) * contents.SysMemPitch, nullptr});
		}
	}
	return true;
}

void run_job(Job *job)
{
	ShaderRegexConfigRead config;
	CustomResource *resource = job->resource;
	auto start = std::chrono::steady_clock::now();

	// The section is gone with the config it came from
	if (job->generation != generation || resource->substantiated)
		return;

	if (job->warm_only) {
		FileContents file(resource->filename.c_str());
		return;
	}

	if (!Dx12IsShimDevice(job->device) && (job->device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)) {
		// The device may only be used on the game's thread: read the
		// file into the file cache for the load at present
		FileContents file(resource->filename.c_str());
		job->create_at_commit = true;
		job->file_bytes = file.identity.size;
	} else {
		// A file identical to one loaded earlier but not given to its
		// section yet shares that resource
		SharedProvisionalScope provisional;

		// Without LockResourceCreationMode(): this DLL sets no stereo
		// surface creation mode for it to guard, and holding it through a
		// large creation would stall the draws that create resources
		Profiling::Spike::Scope spike(Profiling::Spike::RESOURCE_LOAD, resource->filename.c_str());

		if (Dx12IsShimDevice(job->device)) {
			Dx12StagingScope staging;
			resource->LoadFile(job->device, job->bind_flags, job->misc_flags, &job->load);
			job->staged = true;
			if (job->load.resource && !job->load.reused) {
				job->held_bytes = Dx12ShimStagedUploadBytes(job->load.resource);
				upload_bytes += job->held_bytes;
			}
		} else {
			// Made with their contents in one go, a large resource
			// holds up the drawing thread in the driver, and the GPU
			// copies it all in the next frame. Recording its contents
			// on a deferred context here held up the drawing thread
			// the same way.
			job->load.may_defer_contents = true;
			resource->LoadFile(job->device, job->bind_flags, job->misc_flags, &job->load);
			if (!job->load.subresources.empty() && !make_parts(job)) {
				CustomResourceFileLoad retry;

				job->parts.clear();
				resource->LoadFile(job->device, job->bind_flags, job->misc_flags, &retry);
				std::swap(job->load.resource, retry.resource);
				job->load.device = retry.device;
				job->load.contents.reset();
			}
			bool staged_parts = !job->parts.empty() && make_staging(job);
			job->load.subresources.clear();
			if (job->load.contents) {
				job->held_bytes = job->load.contents->size;
				upload_bytes += job->held_bytes;
				if (staged_parts)
					job->load.contents.reset();
			}
		}
		// Not shared before its contents are there
		if (job->load.share && job->load.resource && job->parts.empty())
			ShareFileResource(job->load.share_key, resource->filename, job->load.identity, job->load.resource, true);
	}
	if (!job->create_at_commit)
		InterlockedIncrement((volatile LONG*)&Profiling::resources_created);
	stat_load_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void wait_for_game_creation()
{
	if (!last_game_creation.load(std::memory_order_relaxed))
		return;
	static const LONGLONG frequency = [] {
		LARGE_INTEGER value;
		QueryPerformanceFrequency(&value);
		return value.QuadPart;
	}();
	const DWORD sleep_ms = 2;
	const LONGLONG recent_ticks = frequency * 8 / 1000;
	const LONGLONG last_sleep_ticks = frequency * (100 - sleep_ms) / 1000;
	LARGE_INTEGER start, now;
	QueryPerformanceCounter(&start);
	for (;;) {
		LONGLONG last = last_game_creation.load(std::memory_order_relaxed);
		QueryPerformanceCounter(&now);
		if (now.QuadPart - last >= recent_ticks || now.QuadPart - start.QuadPart >= last_sleep_ticks)
			return;
		Sleep(sleep_ms);
	}
}

void worker_loop()
{
	for (;;) {
		std::unique_ptr<Job> job;

		{
			std::unique_lock<std::mutex> lock(queue_lock);
			if (!queue_wake.wait_for(lock, std::chrono::milliseconds(WORKER_IDLE_MS), [] {
					return (!waited.empty() || !warming.empty()) && upload_bytes.load() <= UPLOAD_BYTES_MAX; })) {
				worker_running = false;
				return;
			}
			std::list<std::unique_ptr<Job>> *queue = !waited.empty() ? &waited : &warming;

			job = std::move(queue->front());
			queue->pop_front();
		}

		if (!job->warm_only && G->async_resource_loading)
			wait_for_game_creation();
		run_job(job.get());
		if (job->warm_only)
			continue;

		std::lock_guard<std::mutex> lock(queue_lock);
		finished.push_back(std::move(job));
	}
}

DWORD WINAPI worker_thread(LPVOID)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	worker_loop();
	// Without unwinding: nothing here may still own memory
	FreeLibraryAndExitThread(worker_module, 0);
}

}

void ModLoadNoteGameCreation()
{
	if (!G->async_resource_loading)
		return;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	last_game_creation.store(now.QuadPart, std::memory_order_relaxed);
}

unsigned ModLoadEpoch()
{
	return current_epoch.load(std::memory_order_acquire);
}

bool ModLoadCheck(ModLoad *base, unsigned epoch, ID3D11Device *device)
{
	ModLoadImpl *load = static_cast<ModLoadImpl*>(base);
	uint64_t checked;
	bool ready;

	// Loads on use as before while frames are being dumped
	if (G->analyse_frame)
		return true;
	if (!epoch)
		epoch = ModLoadEpoch();

	checked = load->checked.load(std::memory_order_acquire);
	if ((checked >> 1) == epoch && (!(checked & 1) || ModLoadVariablesSeen(load))) {
		ready = checked & 1;
	} else {
		thread_local std::vector<uint32_t> seen;

		seen.resize(load->var_count);
		for (unsigned i = 0; i < load->var_count; i++)
			memcpy(&seen[i], load->vars[i], sizeof(uint32_t));
		ready = scan_load(load, epoch, device);
		finish_check(load, epoch, ready, seen.data());
	}
	if (!ready)
		stat_held++;
	return ready;
}

void ModLoadQueuePresent(ModLoad *base, ID3D11Device *device)
{
	if (!device || G->analyse_frame || !base->present_pending.exchange(false, std::memory_order_relaxed))
		return;
	auto *load = static_cast<ModLoadImpl*>(base);
	for (auto *present : load->present_initializers) {
		for (auto &entry : present->files)
			enqueue(entry, device);
		for (auto *resource : present->warm)
			enqueue_warm(resource, device);
	}
}

bool ModLoadPresentReady(const PresentLoad *load, ID3D11Device *device)
{
	if (!device || G->analyse_frame)
		return true;

	bool ready = true;
	unsigned epoch = ModLoadEpoch();
	for (auto &entry : load->files) {
		LONG state = entry.resource->load_state;
		if (is_done(state, epoch))
			continue;
		ready = false;
		if (state == MOD_LOAD_IDLE)
			enqueue(entry, device);
	}
	if (!ready) {
		for (auto *resource : load->warm)
			enqueue_warm(resource, device);
	}
	return ready;
}

bool ModLoadClaim(CustomResource *resource)
{
	for (;;) {
		LONG state = resource->load_state;

		if (state == MOD_LOAD_APPLYING || state >= MOD_LOAD_DONE)
			return false;
		if (InterlockedCompareExchange(&resource->load_state, MOD_LOAD_APPLYING, state) == state)
			return true;
	}
}

void ModLoadResourceDone(CustomResource *resource)
{
	InterlockedExchange(&resource->load_state, MOD_LOAD_DONE);
}

void ModLoadNoteLoadOnUse(CustomResource *resource)
{
	static const char *const reasons[] = {
		"reached by no command list the loader follows",
		"used before its background load finished",
		"used only by the config's own command lists",
		"its uses need flags no single resource can have",
	};

	if (built_generation != generation || !G->async_resource_loading)
		return;
	stat_loaded_on_use++;
	LogInfo("Mod loading: %S loaded on use: %s\n", resource->name.c_str(),
			reasons[resource->load_reach < ARRAYSIZE(reasons) ? resource->load_reach : 0]);
}

void ModLoadsConfigCleared()
{
	generation++;
	old_nodes = std::move(nodes);
	old_conds = std::move(conds);
	old_loads = std::move(loads);
	old_present_loads = std::move(present_loads);
	nodes.clear();
	conds.clear();
	loads.clear();
	present_loads.clear();

	std::list<std::unique_ptr<Job>> dropped;
	{
		std::lock_guard<std::mutex> lock(queue_lock);
		dropped.swap(warming);
		warmed.clear();
	}
}

void ModLoadsConfigLoaded()
{
	if (built_generation == generation)
		return;
	built_generation = generation;
	if (G->async_resource_loading)
		build();
}

// The first use of a resource made on the loader thread cost the draw that
// made it several milliseconds (NVIDIA). A section given its resource at a
// present has a copy of a few bytes out of it queued there instead, and is
// drawn from once an event query says the GPU is past that copy. Until then
// its state is MOD_LOAD_TOUCHING.
const LONG MOD_LOAD_TOUCHING = MOD_LOAD_DONE + 0x3fffffff;

struct Touch
{
	ID3D11Query *query;
	// Not held, only compared with the presenting device
	ID3D11Device *device;
	std::vector<CustomResource*> resources;
	unsigned generation;
};
std::deque<Touch> touches;
ID3D11Device *touch_device;
std::unordered_map<UINT, ID3D11Resource*> touch_targets;

ID3D11Resource* touch_target(ID3D11Device *device, D3D11_RESOURCE_DIMENSION dimension, DXGI_FORMAT format, UINT block)
{
	UINT key = (UINT)dimension << 16 | (UINT)format;
	ID3D11Resource *target = nullptr;

	if (touch_device != device) {
		for (auto &entry : touch_targets)
			entry.second->Release();
		touch_targets.clear();
		touch_device = device;
	}
	auto found = touch_targets.find(key);
	if (found != touch_targets.end())
		return found->second;

	if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		device->CreateBuffer(&desc, NULL, (ID3D11Buffer**)&target);
	} else {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = desc.Height = block;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		device->CreateTexture2D(&desc, NULL, (ID3D11Texture2D**)&target);
	}
	touch_targets[key] = target;
	return target;
}

bool touch(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *resource)
{
	D3D11_RESOURCE_DIMENSION dimension;
	ID3D11Resource *target;
	D3D11_BOX box = {0, 0, 0, 1, 1, 1};

	resource->GetType(&dimension);
	if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
		D3D11_BUFFER_DESC desc;
		((ID3D11Buffer*)resource)->GetDesc(&desc);
		box.right = std::min<UINT>(16, desc.ByteWidth);
		target = touch_target(device, dimension, DXGI_FORMAT_UNKNOWN, 0);
	} else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC desc;
		UINT block;
		((ID3D11Texture2D*)resource)->GetDesc(&desc);
		if (desc.SampleDesc.Count > 1 || (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL))
			return false;
		block = ModLoadTextureBlockHeight(desc.Format);
		if (block == 1 && (desc.Format == DXGI_FORMAT_R8G8_B8G8_UNORM || desc.Format == DXGI_FORMAT_G8R8_G8B8_UNORM
				|| desc.Format >= DXGI_FORMAT_AYUV || desc.Format == DXGI_FORMAT_UNKNOWN))
			return false;
		if (desc.Width < block || desc.Height < block)
			return false;
		box.right = box.bottom = block;
		target = touch_target(device, dimension, desc.Format, block);
	} else {
		return false;
	}
	if (!target)
		return false;
	context->CopySubresourceRegion(target, 0, 0, 0, 0, resource, 0, &box);
	return true;
}

void ModLoadsFrame(ID3D11Device *device)
{
	ID3D11DeviceContext *context = nullptr;
	Touch touching = {};
	std::vector<std::unique_ptr<Job>> done;
	UINT64 budget = (UINT64)G->async_resource_commit_mb << 20, used = 0;
	UINT64 upload_budget = std::min<UINT64>(budget, UPLOAD_FRAME_BYTES), uploaded = 0;
	UINT64 copy_budget = std::min<UINT64>(budget, COPY_FRAME_BYTES), copied = 0;
	UINT64 staged_copied = 0;
	unsigned visible = ModLoadEpoch() + 1;
	size_t kept = 0;

	{
		std::lock_guard<std::mutex> lock(queue_lock);
		done.swap(finished);
		// The thread could not be started when the jobs were queued
		if (!waited.empty() || !warming.empty())
			start_worker();
	}
	for (auto &job : done)
		to_commit.push_back(std::move(job));

	while (!touches.empty()) {
		Touch &oldest = touches.front();
		// A query of a device that has been replaced is not asked: the
		// resources are given out, and expire() deals with their device
		bool same_device = !device || oldest.device == device;

		if (!context && device)
			device->GetImmediateContext(&context);
		if (oldest.generation == generation && same_device && context
		    && context->GetData(oldest.query, NULL, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE)
			break;
		if (oldest.generation == generation) {
			for (CustomResource *resource : oldest.resources)
				InterlockedCompareExchange(&resource->load_state, MOD_LOAD_DONE + visible, MOD_LOAD_TOUCHING);
		}
		oldest.query->Release();
		touches.pop_front();
	}

	for (size_t i = 0; i < to_commit.size(); i++) {
		std::unique_ptr<Job> &job = to_commit[i];
		CustomResource *resource = job->resource;
		bool keep = false;

		if (job->generation != generation) {
			job.reset();
			continue;
		}
		// Made on a device that has been replaced: loaded again on the
		// presenting one when its mod is next drawn
		if (device && job->device != device) {
			InterlockedCompareExchange(&resource->load_state, MOD_LOAD_IDLE, MOD_LOAD_QUEUED);
			job.reset();
			continue;
		}

		// Not queued any more: loaded on use meanwhile, or reset
		if (InterlockedCompareExchange(&resource->load_state, MOD_LOAD_APPLYING, MOD_LOAD_QUEUED) != MOD_LOAD_QUEUED) {
			job.reset();
			continue;
		}
		if (resource->substantiated) {
			// Given a resource some other way that left it queued
			ModLoadResourceDone(resource);
			job.reset();
			continue;
		}
		if (resource->resource || resource->view) {
			// Set some other way, which Substantiate() would not replace
			resource->substantiated = true;
		} else if (job->create_at_commit) {
			if (used && used + job->file_bytes > budget) {
				keep = true;
			} else {
				resource->substantiated = true;
				InterlockedIncrement((volatile LONG*)&Profiling::resources_created);
				Profiling::Spike::Scope spike(Profiling::Spike::MOD_RESOURCE_AT_PRESENT, job->resource->filename.c_str());
				spike.add_bytes(job->file_bytes);
				LockResourceCreationMode();
				resource->LoadFile(job->device, job->bind_flags, job->misc_flags, &job->load);
				resource->ApplyFileLoad(&job->load);
				resource->async_loaded = true;
				UnlockResourceCreationMode();
				used += job->file_bytes;
			}
		} else {
			if (job->staged && job->load.resource) {
				UINT64 remaining = copy_budget > staged_copied ? copy_budget - staged_copied : 0;
				UINT64 recorded;

				Profiling::Spike::Scope spike(Profiling::Spike::MOD_UPLOAD_AT_PRESENT, resource->name.c_str());
				keep = !Dx12ShimCommitStaged(job->load.resource, remaining, !staged_copied, &recorded);
				spike.add_bytes(recorded);
				staged_copied += recorded;
			}
			if (job->next_part < job->parts.size() && !context && device)
				device->GetImmediateContext(&context);
			while (job->next_part < job->parts.size() && context) {
				Job::Part &part = job->parts[job->next_part];
				UINT64 &spent = part.staging ? copied : uploaded;

				if (spent && spent + part.bytes > (part.staging ? copy_budget : upload_budget))
					break;
				job->next_part++;
				spent += part.bytes;
				Profiling::Spike::Scope spike(Profiling::Spike::MOD_UPLOAD_AT_PRESENT, resource->name.c_str());
				spike.add_bytes(part.bytes);
				if (part.staging) {
					context->CopySubresourceRegion(job->load.resource, part.subresource, part.box.left, part.box.top,
							part.box.front, part.staging, 0, NULL);
					part.staging->Release();
					part.staging = nullptr;
				} else {
					context->UpdateSubresource(job->load.resource, part.subresource, &part.box,
							part.data, part.row_pitch, part.depth_pitch);
				}
			}
			if (job->next_part < job->parts.size())
				keep = true;
			if (!keep && job->held_bytes) {
				job->load.contents.reset();
				upload_bytes -= job->held_bytes;
				job->held_bytes = 0;
				queue_wake.notify_one();
			}
			if (!keep) {
				resource->substantiated = true;
				resource->ApplyFileLoad(&job->load);
				resource->async_loaded = true;
				if (!job->staged && device && resource->resource && !Dx12IsShimDevice(device)) {
					if (!context)
						device->GetImmediateContext(&context);
					if (context && touch(device, context, resource->resource)) {
						touching.resources.push_back(resource);
						InterlockedExchange(&resource->load_state, MOD_LOAD_TOUCHING);
						stat_loaded++;
						job.reset();
						continue;
					}
				}
			}
		}

		if (keep) {
			InterlockedExchange(&resource->load_state, MOD_LOAD_QUEUED);
			if (kept != i)
				to_commit[kept] = std::move(job);
			kept++;
			continue;
		}
		InterlockedExchange(&resource->load_state, MOD_LOAD_DONE + visible);
		stat_loaded++;
		job.reset();
	}
	to_commit.resize(kept);

	if (!touching.resources.empty()) {
		D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};

		touching.generation = generation;
		touching.device = device;
		if (SUCCEEDED(device->CreateQuery(&desc, &touching.query))) {
			context->End(touching.query);
			touches.push_back(std::move(touching));
		} else {
			for (CustomResource *resource : touching.resources)
				InterlockedCompareExchange(&resource->load_state, MOD_LOAD_DONE + visible, MOD_LOAD_TOUCHING);
		}
	}
	if (context)
		context->Release();

	current_epoch.store(visible, std::memory_order_release);
}

void ModLoadGetStats(ModLoadStats *stats)
{
	*stats = {};
	for (auto &load : loads) {
		if (load->all.empty())
			continue;
		stats->mods++;
		if (load->waiting.load(std::memory_order_relaxed))
			stats->waiting_mods++;
	}
	stats->queued = stat_queued;
	stats->loaded = stat_loaded;
	stats->loaded_on_use = stat_loaded_on_use;

	stats->held_draws = stat_held;
	stats->load_ms = stat_load_us / 1000.0;
}
