#include "tree.h"
#include <algorithm>
#include <iterator>
#include <functional>

namespace {

ClockNode* find_child_ptr(ClockNode* parent, const std::string& child_name, size_t& child_index) {
	if (!parent) return nullptr;
	for (size_t i = 0; i < parent->children.size(); ++i) {
		if (parent->children[i] && parent->children[i]->name == child_name) {
			child_index = i;
			return parent->children[i].get();
		}
	}
	return nullptr;
}

std::unique_ptr<ClockNode> clone_node(const ClockNode* source, ClockNode* parent) {
	if (!source) return nullptr;

	auto copy = std::make_unique<ClockNode>();
	copy->name = source->name;
	copy->type = source->type;
	copy->buffer_type_id = source->buffer_type_id;
	copy->is_sink = source->is_sink;
	copy->original = source->original;
	copy->parent = parent;
	copy->children.reserve(source->children.size());
	for (const auto& child : source->children) {
		copy->children.push_back(clone_node(child.get(), copy.get()));
	}
	return copy;
}

} // namespace

void ClockTree::index_subtree(ClockNode* node) {
	if (!node) return;
	node_index[node->name] = node;
	for (const auto& child : node->children) {
		index_subtree(child.get());
	}
}

bool ClockTree::rebuild_index() {
	node_index.clear();
	if (!root) return true;
	index_subtree(root.get());
	return true;
}

bool ClockTree::configure_buffer_types(const std::vector<std::string>& type_names) {
	buffer_type_id_by_name.clear();
	buffer_type_id_by_name.reserve(type_names.size());
	for (size_t i = 0; i < type_names.size(); ++i) {
		if (!buffer_type_id_by_name.emplace(type_names[i], static_cast<int>(i)).second) return false;
	}
	bool valid = true;
	std::function<void(ClockNode*)> assign = [&](ClockNode* node) {
		if (!node) return;
		if (!node->is_sink && !node->type.empty()) {
			node->buffer_type_id = lookup_buffer_type_id(node->type);
			valid = valid && node->buffer_type_id >= 0;
		} else {
			node->buffer_type_id = -1;
		}
		for (auto& child : node->children) assign(child.get());
	};
	assign(root.get());
	return valid;
}

int ClockTree::lookup_buffer_type_id(const std::string& type_name) const {
	auto it = buffer_type_id_by_name.find(type_name);
	return it == buffer_type_id_by_name.end() ? -1 : it->second;
}

ClockNode* ClockTree::find_node(const std::string& name) {
	auto it = node_index.find(name);
	if (it == node_index.end()) return nullptr;
	return it->second;
}

const ClockNode* ClockTree::find_node(const std::string& name) const {
	auto it = node_index.find(name);
	if (it == node_index.end()) return nullptr;
	return it->second;
}

bool ClockTree::insert_buffer_between(const std::string& parent_name,
									  const std::string& child_name,
							  std::string buffer_name,
									  const std::string& buffer_type) {
	if (buffer_type.empty()) return false;
	// ensure unique buffer name: if empty or colliding, generate a unique one
	if (buffer_name.empty() || find_node(buffer_name) != nullptr) {
		buffer_name = generate_unique_name(buffer_name.empty() ? "NEW_BUF" : buffer_name);
	}

	ClockNode* parent = find_node(parent_name);
	if (!parent) return false;

	size_t child_index = 0;
	ClockNode* child = find_child_ptr(parent, child_name, child_index);
	if (!child) return false;

	auto buffer = std::make_unique<ClockNode>();
	buffer->name = buffer_name;
	buffer->type = buffer_type;
	buffer->buffer_type_id = lookup_buffer_type_id(buffer_type);
	if (!buffer_type_id_by_name.empty() && buffer->buffer_type_id < 0) return false;
	buffer->is_sink = false;
	buffer->parent = parent;
	buffer->original = false;

	auto moved_child = std::move(parent->children[child_index]);
	moved_child->parent = buffer.get();

	buffer->children.push_back(std::move(moved_child));
	parent->children[child_index] = std::move(buffer);
	node_index[buffer_name] = parent->children[child_index].get();
	return true;
}

bool ClockTree::insert_buffer_above_children(
	const std::string& parent_name,
	const std::vector<std::string>& child_names,
	std::string buffer_name,
	const std::string& buffer_type) {
	if (buffer_type.empty() || child_names.size() < 2) return false;
	if (buffer_name.empty() || find_node(buffer_name) != nullptr) {
		buffer_name = generate_unique_name(buffer_name.empty() ? "NEW_BUF" : buffer_name);
	}
	ClockNode* parent = find_node(parent_name);
	if (!parent) return false;
	const int type_id = lookup_buffer_type_id(buffer_type);
	if (!buffer_type_id_by_name.empty() && type_id < 0) return false;

	std::unordered_map<std::string, bool> selected_names;
	selected_names.reserve(child_names.size());
	for (const std::string& name : child_names) {
		if (!selected_names.emplace(name, true).second) return false;
	}
	size_t selected_count = 0;
	size_t insertion_index = 0;
	bool found_first = false;
	for (size_t i = 0; i < parent->children.size(); ++i) {
		const auto& child = parent->children[i];
		if (child && selected_names.count(child->name) != 0) {
			if (!found_first) {
				insertion_index = i;
				found_first = true;
			}
			++selected_count;
		}
	}
	if (selected_count != child_names.size()) return false;

	auto buffer = std::make_unique<ClockNode>();
	buffer->name = buffer_name;
	buffer->type = buffer_type;
	buffer->buffer_type_id = type_id;
	buffer->is_sink = false;
	buffer->original = false;
	buffer->parent = parent;
	std::vector<std::unique_ptr<ClockNode>> remaining;
	remaining.reserve(parent->children.size() - selected_count + 1);
	for (auto& child : parent->children) {
		if (child && selected_names.count(child->name) != 0) {
			child->parent = buffer.get();
			buffer->children.push_back(std::move(child));
		} else {
			remaining.push_back(std::move(child));
		}
	}
	// The first selected child has no selected sibling before it.
	const size_t remaining_before = insertion_index;
	ClockNode* buffer_ptr = buffer.get();
	remaining.insert(remaining.begin() + static_cast<std::ptrdiff_t>(remaining_before),
				 std::move(buffer));
	parent->children = std::move(remaining);
	node_index[buffer_name] = buffer_ptr;
	return true;
}

bool ClockTree::set_buffer_type(const std::string& node_name, const std::string& new_type) {
	if (node_name.empty()) return false;
	ClockNode* node = find_node(node_name);
	if (!node) return false;
	const int new_type_id = lookup_buffer_type_id(new_type);
	if (!buffer_type_id_by_name.empty() && new_type_id < 0) return false;
	// Allow resizing of original and new nodes; caller must ensure new_type is valid for libs.
	node->type = new_type;
	node->buffer_type_id = new_type_id;
	return true;
}

bool ClockTree::rename_node(const std::string& node_name, const std::string& new_name) {
	if (node_name.empty() || new_name.empty()) return false;
	ClockNode* node = find_node(node_name);
	if (!node) return false;
	if (node->original) return false; // prohibit renaming original nodes
	if (find_node(new_name) != nullptr) return false; // name collision
	node->name = new_name;
	rebuild_index();
	return true;
}

bool ClockTree::delete_buffer(const std::string& node_name) {
	if (node_name.empty()) return false;
	ClockNode* node = find_node(node_name);
	if (!node) return false;
	if (node == root.get()) return false; // cannot delete root
	if (node->original) return false; // do not delete original nodes

	ClockNode* parent = node->parent;
	if (!parent) return false;

	// find index in parent's children
	size_t idx = 0;
	bool found = false;
	for (size_t i = 0; i < parent->children.size(); ++i) {
		if (parent->children[i].get() == node) { idx = i; found = true; break; }
	}
	if (!found) return false;

	// take ownership of the node to delete
	auto orphan = std::move(parent->children[idx]);

	// prepare children to move into parent
	std::vector<std::unique_ptr<ClockNode>> movers;
	for (auto &ch : orphan->children) {
		ch->parent = parent;
		movers.push_back(std::move(ch));
	}

	// erase the slot for the orphan
	parent->children.erase(parent->children.begin() + idx);

	// insert moved children at the same position
	if (!movers.empty()) {
		parent->children.insert(parent->children.begin() + idx,
								std::make_move_iterator(movers.begin()),
								std::make_move_iterator(movers.end()));
	}

	remove_node_from_index(node_name);
	return true;
}

std::string ClockTree::generate_unique_name(const std::string& prefix) {
	return prefix + "_" + std::to_string(next_buf_id++);
}

ClockTree ClockTree::clone() const {
	ClockTree copy;
	copy.root_name = root_name;
	copy.next_buf_id = next_buf_id;
	copy.buffer_type_id_by_name = buffer_type_id_by_name;
	copy.root = clone_node(root.get(), nullptr);
	copy.rebuild_index();
	return copy;
}
