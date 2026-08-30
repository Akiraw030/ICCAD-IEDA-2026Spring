#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

struct ClockNode {
    std::string name;
    std::string type;
    int buffer_type_id = -1;
    bool is_sink = false;
    bool original = false;
    ClockNode* parent = nullptr;
    std::vector<std::unique_ptr<ClockNode>> children;
};

struct ClockTree {
    std::string root_name;
    std::unique_ptr<ClockNode> root;
    mutable int next_buf_id = 0;

    bool rebuild_index();
    bool configure_buffer_types(const std::vector<std::string>& type_names);
    int lookup_buffer_type_id(const std::string& type_name) const;
    ClockNode* find_node(const std::string& name);
    const ClockNode* find_node(const std::string& name) const;
    void add_node_to_index(ClockNode* node) { if (node) node_index[node->name] = node; }
    void remove_node_from_index(const std::string& name) { node_index.erase(name); }
    bool insert_buffer_between(const std::string& parent_name,
                               const std::string& child_name,
                               std::string buffer_name,
                               const std::string& buffer_type);
    bool insert_buffer_above_children(const std::string& parent_name,
                                      const std::vector<std::string>& child_names,
                                      std::string buffer_name,
                                      const std::string& buffer_type);
    bool set_buffer_type(const std::string& node_name, const std::string& new_type);
    bool rename_node(const std::string& node_name, const std::string& new_name);
    bool delete_buffer(const std::string& node_name);
    std::string generate_unique_name(const std::string& prefix = "NEW_BUF");
    ClockTree clone() const;
    size_t node_count() const { return node_index.size(); }

private:
    std::unordered_map<std::string, ClockNode*> node_index;
    std::unordered_map<std::string, int> buffer_type_id_by_name;

    void index_subtree(ClockNode* node);
};
