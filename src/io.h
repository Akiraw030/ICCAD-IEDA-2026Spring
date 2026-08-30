#pragma once
#include <string>
#include "tree.h"
#include "delay_model.h"

bool write_clk_tree(const std::string& path, const ClockTree& tree);
