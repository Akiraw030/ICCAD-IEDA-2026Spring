#include "parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

// Minimal/robust stubs for parsing; to be implemented
bool parse_clk_tree(const std::string& path, ClockTree& tree) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    // helpers
    auto ltrim = [](std::string s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
        return s;
    };
    auto rtrim = [](std::string s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
        return s;
    };
    auto trim = [&](std::string s){ return rtrim(ltrim(s)); };

    // create root container
    tree.root.reset(new ClockNode());
    tree.root->name = "";
    tree.root->type = "";
    tree.root->original = true;

    // stack of node pointers by level (index = level)
    std::vector<ClockNode*> level_stack;
    level_stack.resize(1);
    level_stack[0] = tree.root.get();

    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        // Root line
        if (t.find("Root:") == 0) {
            std::string rn = trim(t.substr(5));
            tree.root_name = rn;
            tree.root->name = rn;
            continue;
        }

        // Expect lines like: [<level>] <Name> (<Type>) [ (SINK) ]
        size_t p1 = t.find('[');
        size_t p2 = t.find(']');
        if (p1 == std::string::npos || p2 == std::string::npos || p2 <= p1) continue;
        std::string level_str = t.substr(p1+1, p2-p1-1);
        int lvl = 0;
        try { lvl = std::stoi(level_str); } catch(...) { continue; }

        // remainder after ]
        std::string rest = trim(t.substr(p2+1));
        // extract name (up to first '('\n)
        std::string name;
        std::string type;
        bool is_sink = false;
        size_t par = rest.find('(');
        if (par != std::string::npos) {
            name = trim(rest.substr(0, par));
            size_t par2 = rest.find(')', par);
            if (par2 != std::string::npos) {
                type = trim(rest.substr(par+1, par2-par-1));
                // check for (SINK) after
                std::string after = trim(rest.substr(par2+1));
                if (!after.empty() && after.find("SINK") != std::string::npos) is_sink = true;
            } else {
                name = trim(rest);
            }
        } else {
            name = trim(rest);
        }

        // ensure stack large enough
        if ((int)level_stack.size() <= lvl) level_stack.resize(lvl+1, nullptr);

        // parent is level-1 (level 1 children of root)
        ClockNode* parent = (lvl > 0) ? level_stack[lvl-1] : tree.root.get();
        if (!parent) parent = tree.root.get();

        // create node and attach
        auto node = std::make_unique<ClockNode>();
        node->name = name;
        node->type = type;
        node->is_sink = is_sink;
        node->parent = parent;
        node->original = true;
        ClockNode* node_ptr = node.get();
        parent->children.emplace_back(std::move(node));

        // set/replace stack at this level
        level_stack[lvl] = node_ptr;
        // clear deeper levels
        for (size_t i = lvl+1; i < level_stack.size(); ++i) level_stack[i] = nullptr;
    }
    tree.rebuild_index();
    return true;
}

bool parse_buf_lib(const std::string& path, std::vector<BufSpec>& libs) {
    std::ifstream in(path);
    if (!in) return false;
    // stub: not implemented
    std::string line;
    BufSpec cur;
    bool in_cell = false;
    while (std::getline(in, line)) {
        // trim
        auto l = line;
        l.erase(l.begin(), std::find_if(l.begin(), l.end(), [](unsigned char ch){ return !std::isspace(ch); }));
        l.erase(std::find_if(l.rbegin(), l.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), l.end());
        if (l.empty()) continue;
        if (!in_cell) {
            // look for cell (NAME)
            if (l.rfind("cell", 0) == 0) {
                size_t p = l.find('(');
                size_t q = l.find(')');
                if (p != std::string::npos && q != std::string::npos && q > p) {
                    cur = BufSpec();
                    cur.name = l.substr(p+1, q-p-1);
                    in_cell = true;
                }
            }
        } else {
            if (l.find("}") != std::string::npos) {
                // finish cell
                libs.push_back(cur);
                in_cell = false;
                continue;
            }
            // parse SIZE, SS_DELAY, FF_DELAY
            std::istringstream ss(l);
            std::string tok;
            ss >> tok;
            if (tok == "SIZE") {
                double w = 0, by = 0; std::string bytok;
                ss >> w >> bytok; // bytok should be "BY"
                if (bytok == "BY") ss >> by; else by = 0;
                cur.width = w; cur.height = by;
            } else if (tok == "SS_DELAY") {
                cur.ss_delay.clear();
                double v;
                while (ss >> v) cur.ss_delay.push_back(v);
            } else if (tok == "FF_DELAY") {
                cur.ff_delay.clear();
                double v;
                while (ss >> v) cur.ff_delay.push_back(v);
            }
        }
    }
    return true;
}

bool parse_rpt(const std::string& path, std::vector<PathInfo>& paths, double& Tclk) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        // Clock Period line
        if (line.find("Clock Period") != std::string::npos) {
            // find first number in the line
            std::stringstream ss(line);
            std::string tok;
            while (ss >> tok) {
                try {
                    double v = std::stod(tok);
                    Tclk = v;
                    break;
                } catch (...) {}
            }
            continue;
        }

        // Path lines like: Path1 :     FF_644  ->  FF_1000    0.2432
        // find ':' then parse remainder
        size_t col = line.find(':');
        if (col == std::string::npos) continue;
        std::string rest = line.substr(col+1);
        // replace tabs with spaces and compress
        for (char &c : rest) if (c == '\t') c = ' ';
        std::istringstream ss(rest);
        std::string launch, arrow, capture;
        double delay = 0.0;
        if (!(ss >> launch)) continue;
        // next token might be '->' or stray; consume until '->'
        if (!(ss >> arrow)) continue;
        if (arrow != "->") {
            // maybe launch token was empty due to spacing; try read until find ->
            // shift: arrow actually might be the DFF if formatted: FF_644 ->
            // handle when arrow is something like FF_644
            std::string maybe;
            if (arrow == "") continue;
            // arrow currently holds what we expected as '->', so treat as launch is correct and arrow is '->' absent
        }
        // if arrow is not '->', try to find '->' in stream
        if (arrow != "->") {
            // attempt to read until we find ->
            std::string token;
            bool found = false;
            while (ss >> token) {
                if (token == "->") { found = true; break; }
                capture = token; // capture might be shifted
            }
            if (!found) continue;
            // next token is capture
            if (!(ss >> capture)) continue;
        } else {
            // arrow == "->" so next token is capture
            if (!(ss >> capture)) continue;
        }
        // final token(s) include delay
        if (!(ss >> delay)) {
            // maybe there are extra spaces, try to read last token from line
            std::stringstream s2(rest);
            std::string tok;
            std::vector<std::string> toks;
            while (s2 >> tok) toks.push_back(tok);
            if (!toks.empty()) {
                try { delay = std::stod(toks.back()); } catch(...) { continue; }
            } else continue;
        }

        PathInfo p;
        p.launch_ff = launch;
        p.capture_ff = capture;
        p.data_delay = delay;
        // try to give a name to the path if possible
        p.name = "";
        paths.push_back(p);
    }

    return true;
}
