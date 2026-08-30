#pragma once
#include <string>
#include <vector>
#include "tree.h"

struct PathInfo {
    std::string name;
    std::string launch_ff;
    std::string capture_ff;
    double data_delay = 0.0;
};

struct BufSpec {
    std::string name;
    double width = 0.0;
    double height = 0.0;
    std::vector<double> ss_delay;
    std::vector<double> ff_delay;
};

bool parse_clk_tree(const std::string& path, ClockTree& tree);
bool parse_buf_lib(const std::string& path, std::vector<BufSpec>& libs);
bool parse_rpt(const std::string& path, std::vector<PathInfo>& paths, double& Tclk);
