#include "io.h"
#include <fstream>
#include <functional>

bool write_clk_tree(const std::string& path, const ClockTree& tree) {
    std::ofstream out(path);
    if (!out) return false;
    out << "Root: " << tree.root_name << "\n";

    if (!tree.root) return true;

    std::function<void(const ClockNode*, int)> dfs = [&](const ClockNode* node, int depth) {
        if (!node) return;
        for (int i = 0; i < depth; ++i) out << '\t';
        out << '[' << depth << "] " << node->name;
        if (!node->type.empty()) {
            out << " (" << node->type << ")";
        }
        if (node->is_sink) {
            out << " (SINK)";
        }
        out << "\n";

        for (const auto& child : node->children) {
            dfs(child.get(), depth + 1);
        }
    };

    for (const auto& child : tree.root->children) {
        dfs(child.get(), 1);
    }
    return true;
}
