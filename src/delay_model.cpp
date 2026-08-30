#include "delay_model.h"
#include <algorithm>
#include <limits>
#include <functional>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace {

std::string path_key(const std::string& launch_ff, const std::string& capture_ff) {
	return launch_ff + "->" + capture_ff;
}

} // namespace

DelayModel::DelayModel(const std::vector<BufSpec>& libs) {
	lib_by_id = libs;
	for (const auto& lib : libs) {
		lib_by_name[lib.name] = lib;
	}
}

double DelayModel::buffer_delay_ss(const std::string& cell_type, size_t fanout) const {
	auto it = lib_by_name.find(cell_type);
	if (it == lib_by_name.end() || it->second.ss_delay.empty() || fanout == 0) {
		return 0.0;
	}
	const auto& delays = it->second.ss_delay;
	size_t idx = std::min(fanout - 1, delays.size() - 1);
	return delays[idx];
}

double DelayModel::buffer_delay_ff(const std::string& cell_type, size_t fanout) const {
	auto it = lib_by_name.find(cell_type);
	if (it == lib_by_name.end() || it->second.ff_delay.empty() || fanout == 0) {
		return 0.0;
	}
	const auto& delays = it->second.ff_delay;
	size_t idx = std::min(fanout - 1, delays.size() - 1);
	return delays[idx];
}

double DelayModel::compute_tree_area(const ClockTree& tree) const {
	double area = 0.0;
	if (!tree.root) return area;

	std::function<void(const ClockNode*)> dfs = [&](const ClockNode* node) {
		if (!node) return;
		if (!node->is_sink && !node->type.empty()) {
			if (node->buffer_type_id >= 0 &&
				static_cast<size_t>(node->buffer_type_id) < lib_by_id.size()) {
				area += estimate_buffer_area(lib_by_id[static_cast<size_t>(node->buffer_type_id)]);
			} else {
				auto it = lib_by_name.find(node->type);
				if (it != lib_by_name.end()) area += estimate_buffer_area(it->second);
			}
		}
		for (const auto& child : node->children) {
			dfs(child.get());
		}
	};

	dfs(tree.root.get());
	return area;
}

std::unordered_map<std::string, double> DelayModel::compute_clock_arrivals(const ClockTree& tree, bool ss_corner) const {
	std::unordered_map<std::string, double> arrival_by_ff;
	if (!tree.root) return arrival_by_ff;
	arrival_by_ff.reserve(tree.node_count());

	std::function<void(const ClockNode*, double)> dfs = [&](const ClockNode* node, double arrival) {
		if (!node) return;

		double node_delay = 0.0;
		if (!node->is_sink && !node->type.empty()) {
			size_t fanout = node->children.size();
			if (node->buffer_type_id >= 0 &&
				static_cast<size_t>(node->buffer_type_id) < lib_by_id.size()) {
				const auto& lib = lib_by_id[static_cast<size_t>(node->buffer_type_id)];
				const auto& delays = ss_corner ? lib.ss_delay : lib.ff_delay;
				if (!delays.empty() && fanout > 0) {
					node_delay = delays[std::min(fanout - 1, delays.size() - 1)];
				}
			} else {
				node_delay = ss_corner ? buffer_delay_ss(node->type, fanout)
								       : buffer_delay_ff(node->type, fanout);
			}
		}

		double next_arrival = arrival + node_delay;
		for (const auto& child : node->children) {
			if (child->is_sink) {
				arrival_by_ff[child->name] = next_arrival;
			} else {
				dfs(child.get(), next_arrival);
			}
		}
	};

	dfs(tree.root.get(), 0.0);
	return arrival_by_ff;
}

TimingAnalysisResult DelayModel::analyze_timing_from_arrivals(const std::unordered_map<std::string, double>& ss_arrival,
															  const std::unordered_map<std::string, double>& ff_arrival,
															  const std::vector<PathInfo>& ss_paths,
															  const std::vector<PathInfo>& ff_paths,
															  double clock_period) const {
	TimingAnalysisResult result;
	result.clock_period = clock_period;
	result.t_setup = 0.08 * clock_period;
	result.t_hold = 0.05 * clock_period;

	std::unordered_map<std::string, TimingPathResult> merged;
	merged.reserve(ss_paths.size() + ff_paths.size());

	for (const auto& path : ss_paths) {
		TimingPathResult item;
		item.path_name = path.name;
		item.launch_ff = path.launch_ff;
		item.capture_ff = path.capture_ff;
		item.data_delay = path.data_delay;
		auto launch_it = ss_arrival.find(path.launch_ff);
		auto capture_it = ss_arrival.find(path.capture_ff);
		if (launch_it != ss_arrival.end()) item.launch_clk_delay = launch_it->second;
		if (capture_it != ss_arrival.end()) item.capture_clk_delay = capture_it->second;
		item.skew = item.capture_clk_delay - item.launch_clk_delay;
		item.setup_slack = clock_period - result.t_setup - item.data_delay + item.skew;
		const std::string key = path_key(path.launch_ff, path.capture_ff);
		auto it = merged.find(key);
		if (it == merged.end()) {
			merged.emplace(key, item);
		} else {
			it->second.setup_slack = std::min(it->second.setup_slack, item.setup_slack);
			if (it->second.path_name.empty()) it->second.path_name = item.path_name;
		}
	}

	for (const auto& path : ff_paths) {
		const std::string key = path_key(path.launch_ff, path.capture_ff);
		auto it = merged.find(key);
		if (it == merged.end()) {
			TimingPathResult item;
			item.path_name = path.name;
			item.launch_ff = path.launch_ff;
			item.capture_ff = path.capture_ff;
			item.data_delay = path.data_delay;
			auto launch_it = ff_arrival.find(path.launch_ff);
			auto capture_it = ff_arrival.find(path.capture_ff);
			if (launch_it != ff_arrival.end()) item.launch_clk_delay = launch_it->second;
			if (capture_it != ff_arrival.end()) item.capture_clk_delay = capture_it->second;
			item.skew = item.capture_clk_delay - item.launch_clk_delay;
			item.hold_slack = item.data_delay - result.t_hold - item.skew;
			merged.emplace(key, item);
		} else {
			auto& item = it->second;
			auto launch_it = ff_arrival.find(path.launch_ff);
			auto capture_it = ff_arrival.find(path.capture_ff);
			if (launch_it != ff_arrival.end()) item.launch_clk_delay = launch_it->second;
			if (capture_it != ff_arrival.end()) item.capture_clk_delay = capture_it->second;
			item.skew = item.capture_clk_delay - item.launch_clk_delay;
			item.hold_slack = std::min(item.hold_slack, path.data_delay - result.t_hold - item.skew);
			if (item.path_name.empty()) item.path_name = path.name;
		}
	}

	result.path_results.reserve(merged.size());
	for (auto& kv : merged) {
		result.path_results.push_back(kv.second);
		const auto& p = kv.second;
		if (p.setup_slack < 0.0) {
			result.ss.tns += p.setup_slack;
			result.ss.violating_paths += 1;
			result.ss.wns = (result.ss.violating_paths == 1) ? p.setup_slack : std::min(result.ss.wns, p.setup_slack);
		}
		if (p.hold_slack < 0.0) {
			result.ff.tns += p.hold_slack;
			result.ff.violating_paths += 1;
			result.ff.wns = (result.ff.violating_paths == 1) ? p.hold_slack : std::min(result.ff.wns, p.hold_slack);
		}
	}

	if (result.ss.violating_paths == 0) result.ss.wns = 0.0;
	if (result.ff.violating_paths == 0) result.ff.wns = 0.0;
	std::sort(result.path_results.begin(), result.path_results.end(), [](const TimingPathResult& a, const TimingPathResult& b) {
		return a.path_name < b.path_name;
	});
	return result;
}

TimingAnalysisResult DelayModel::analyze_timing(const ClockTree& tree,
												const std::vector<PathInfo>& ss_paths,
												const std::vector<PathInfo>& ff_paths,
												double clock_period) const {
	const auto ss_arrival = compute_clock_arrivals(tree, true);
	const auto ff_arrival = compute_clock_arrivals(tree, false);
	return analyze_timing_from_arrivals(ss_arrival, ff_arrival, ss_paths, ff_paths, clock_period);
}

ScoreMetrics DelayModel::compute_score_metrics(const TimingAnalysisResult& current,
												   const TimingAnalysisResult& original,
												   double current_area,
												   double original_area,
												   double alpha,
												   double beta,
												   double gamma) const {
	ScoreMetrics metrics;
	metrics.original_ss_tns = original.ss.tns;
	metrics.original_ss_wns = original.ss.wns;
	metrics.original_ff_tns = original.ff.tns;
	metrics.original_ff_wns = original.ff.wns;
	metrics.original_area = original_area;
	metrics.current_ss_tns = current.ss.tns;
	metrics.current_ss_wns = current.ss.wns;
	metrics.current_ff_tns = current.ff.tns;
	metrics.current_ff_wns = current.ff.wns;
	metrics.current_area = current_area;

	auto normalized_delta = [](double current_value, double original_value) {
		if (original_value == 0.0) {
			return (current_value == 0.0) ? 0.0 : -std::abs(current_value);
		}
		return 1.0 - (current_value / original_value);
	};

	metrics.ss_component = normalized_delta(metrics.current_ss_tns, metrics.original_ss_tns)
						 + normalized_delta(metrics.current_ss_wns, metrics.original_ss_wns);
	metrics.ff_component = normalized_delta(metrics.current_ff_tns, metrics.original_ff_tns)
						 + normalized_delta(metrics.current_ff_wns, metrics.original_ff_wns);
	metrics.area_component = normalized_delta(metrics.current_area, metrics.original_area);
	metrics.total_score = alpha * metrics.ss_component + beta * metrics.ff_component + gamma * metrics.area_component;
	return metrics;
}

LegalityReport DelayModel::validate_legality(const ClockTree& tree, const std::vector<BufSpec>& libs) const {
	LegalityReport report;
	auto add_issue = [&](const std::string& issue) {
		report.ok = false;
		report.issues.push_back(issue);
	};

	if (!tree.root) {
		add_issue("Clock tree root is missing");
		return report;
	}

	std::unordered_set<std::string> seen_names;
	seen_names.reserve(tree.node_count());
	std::function<void(const ClockNode*)> dfs = [&](const ClockNode* node) {
		if (!node) return;
		if (!seen_names.insert(node->name).second) {
			add_issue("Duplicate node name: " + node->name);
		}
		if (node->is_sink && !node->children.empty()) {
			add_issue("Sink node has children: " + node->name);
		}
		if (!node->is_sink && node != tree.root.get() && node->children.empty()) {
			add_issue("Floating buffer node with no children: " + node->name);
		}
		for (const auto& child : node->children) {
			if (!child) {
				add_issue("Null child pointer found under: " + node->name);
				continue;
			}
			if (!child->is_sink) {
				const int type_id = child->buffer_type_id;
				if (type_id < 0 || static_cast<size_t>(type_id) >= libs.size()) {
					add_issue("Unknown buffer type: " + child->type);
				} else if (child->children.size() >
						   std::min(libs[static_cast<size_t>(type_id)].ss_delay.size(),
						            libs[static_cast<size_t>(type_id)].ff_delay.size())) {
					add_issue("Fanout limit exceeded at " + child->name);
				}
			}
			if (child->parent != node) {
				add_issue("Broken parent pointer: " + child->name);
			}
			dfs(child.get());
		}
	};

	dfs(tree.root.get());
	if (!tree.root_name.empty() && tree.root->name != tree.root_name) {
		add_issue("Root name mismatch between header and root node");
	}
	return report;
}
