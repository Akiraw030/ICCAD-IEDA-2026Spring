#include "optimizer.h"
#include "fast_timing_engine.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef ENABLE_OUTPUT
#define DEBUG_PRINT(x) do { std::cout << x << std::endl; } while(0)
#else
#define DEBUG_PRINT(x) do {} while(0)
#endif

namespace {

TimingAnalysisResult compact_timing_snapshot(const TimingAnalysisResult& source) {
    TimingAnalysisResult snapshot;
    snapshot.clock_period = source.clock_period;
    snapshot.t_setup = source.t_setup;
    snapshot.t_hold = source.t_hold;
    snapshot.ss = source.ss;
    snapshot.ff = source.ff;
    return snapshot;
}

void compact_cycle_record(RepairReclaimCycleRecord& cycle) {
    cycle.before_pulse_timing = compact_timing_snapshot(cycle.before_pulse_timing);
    cycle.after_pulse_timing = compact_timing_snapshot(cycle.after_pulse_timing);
    cycle.after_reclaim_timing = compact_timing_snapshot(cycle.after_reclaim_timing);
}

void compact_iteration_record(FinalAltIterationRecord& iteration) {
    iteration.before_timing = compact_timing_snapshot(iteration.before_timing);
    iteration.after_repair_timing = compact_timing_snapshot(iteration.after_repair_timing);
    iteration.after_timing = compact_timing_snapshot(iteration.after_timing);
}

void compact_perturb_record(PerturbRecoverCycleRecord& cycle) {
    cycle.timing_before = compact_timing_snapshot(cycle.timing_before);
    cycle.timing_after = compact_timing_snapshot(cycle.timing_after);
}

struct RemovedBufferState {
    std::string name;
    std::string type;
    std::string parent_name;
    std::vector<std::string> child_names;
    size_t parent_index = 0;
    bool original = false;
};

struct WorstPathChoice {
    bool valid = false;
    std::string target_node;
    std::string path_name;
    double violation_slack = 0.0;
    bool is_setup = true;
};

struct ArrivalRollback {
    std::vector<std::pair<std::string, double>> old_values;
};

struct Phase1bPathGroup {
    std::vector<const PathInfo*> ss_paths;
    std::vector<const PathInfo*> ff_paths;
};

struct Phase1bTimingCache {
    TimingAnalysisResult timing;
    std::vector<Phase1bPathGroup> groups;
    std::unordered_map<std::string, std::vector<size_t>> groups_by_ff;
    std::multiset<double> ss_violations;
    std::multiset<double> ff_violations;
};

struct PathTimingRollback {
    std::vector<std::pair<size_t, TimingPathResult>> old_results;
};

enum class Pattern {
    ParallelMerge,
    CascadedCollapse,
    SizeSwap,
    Rebalance,
    GreedyFallback,
};

struct ParallelMergeRollbackEntry {
    std::unique_ptr<ClockNode> removed_node;
    size_t insert_index = 0;
    size_t moved_child_count = 0;
};

struct ParallelMergeRollback {
    std::string parent_name;
    std::string survivor_name;
    std::vector<ParallelMergeRollbackEntry> removed_nodes;
};

struct Phase0NodeCandidate {
    std::string node_name;
    double pressure = 0.0;
    double estimated_value = 0.0;
};

struct Phase0TypeCandidate {
    const BufSpec* lib = nullptr;
    double ranking_score = 0.0;
    double average_delay_delta = 0.0;
    double area_delta = 0.0;
    int type_distance = 0;
};

struct Phase0BatchCandidate {
    std::string node_name;
    std::string old_type;
    std::string new_type;
    double pressure = 0.0;
    double area_delta = 0.0;
};

enum class ReclaimCandidateKind {
    ResizeOriginal,
    ResizeNewBuffer,
    DeleteNewBuffer,
};

struct ReclaimCandidate {
    ReclaimCandidateKind kind = ReclaimCandidateKind::ResizeNewBuffer;
    std::string node_name;
    std::string old_type;
    std::string new_type;
    double area_save = 0.0;
    double pressure = 0.0;
    double delay_delta = 0.0;
    double pressure_effect = 0.0;
    double score = 0.0;
};

struct LibraryCache {
    std::unordered_map<std::string, int> type_id_by_name;
    std::vector<const BufSpec*> type_by_id;
    std::vector<std::string> type_name;
    std::vector<double> area;
    std::vector<std::vector<double>> ss_delay;
    std::vector<std::vector<double>> ff_delay;
    std::vector<int> types_by_area_ascending;
    std::vector<int> types_by_area_descending;
    std::vector<size_t> max_fanout;
    std::vector<std::vector<std::vector<int>>> smaller_types_by_type_and_fanout;
    long long* lookups = nullptr;
    long long* misses = nullptr;

    int get_type_id(const std::string& name) const {
        if (lookups) ++(*lookups);
        auto it = type_id_by_name.find(name);
        if (it == type_id_by_name.end()) {
            if (misses) ++(*misses);
            return -1;
        }
        return it->second;
    }

    const BufSpec* get(const std::string& name) const {
        const int id = get_type_id(name);
        return id < 0 ? nullptr : type_by_id[static_cast<size_t>(id)];
    }

    bool supports_fanout(int type_id, size_t fanout) const {
        if (type_id < 0 || static_cast<size_t>(type_id) >= type_by_id.size()) return false;
        return fanout <= ss_delay[static_cast<size_t>(type_id)].size() &&
               fanout <= ff_delay[static_cast<size_t>(type_id)].size();
    }

    double delay(int type_id, size_t fanout, bool ss_corner) const {
        if (type_id < 0 || fanout == 0) return 0.0;
        const auto& table = ss_corner
            ? ss_delay[static_cast<size_t>(type_id)]
            : ff_delay[static_cast<size_t>(type_id)];
        if (table.empty()) return 0.0;
        return table[std::min(fanout - 1, table.size() - 1)];
    }
};

struct TreeIndexCache {
    std::vector<ClockNode*> nodes;
    std::vector<int> parent_id;
    std::vector<std::vector<int>> children_ids;
    std::vector<int> subtree_end;
    std::unordered_map<std::string, int> node_id_by_name;
    std::vector<int> preorder;
    std::vector<int> postorder;
    std::vector<int> ff_node_ids;
    std::unordered_map<std::string, int> ff_id_by_name;
    std::vector<int> ff_node_id_by_ff_id;
};

struct IndexedPathInfo {
    const PathInfo* original = nullptr;
    int launch_ff_id = -1;
    int capture_ff_id = -1;
    double data_delay = 0.0;
    bool is_setup_path = false;
};

enum class FinalAltReclaimCandidateKind {
    DeleteNewBuffer,
    ResizeNewBuffer,
    ResizeOriginal,
};

enum class PortfolioInitialStateProfile {
    Phase0Checkpoint,
    RandomAdjacentResize,
    PressureDirectedResize,
    LowPressureAreaShedding,
    AggressiveRandomResize,
};

struct FinalAltReclaimCandidate {
    FinalAltReclaimCandidateKind kind = FinalAltReclaimCandidateKind::ResizeNewBuffer;
    std::string node_name;
    int node_order = 0;
    int old_type_id = -1;
    int new_type_id = -1;
    double area_save = 0.0;
    double delay_delta = 0.0;
    double pressure = 0.0;
    double pressure_effect = 0.0;
    double score = 0.0;
};

struct Phase1APulseResult {
    bool time_limit = false;
    bool accepted = false;
    bool no_buffers_inserted = false;
    std::string stop_reason;
    TimingAnalysisResult before_timing;
    TimingAnalysisResult after_timing;
    double before_area = 0.0;
    double after_area = 0.0;
    int inserted_count = 0;
    int shared_candidates_tried = 0;
    int shared_insertions = 0;
    bool shared_path_repair_attempted = false;
    bool fast_timing_current = false;
    int batch_attempts = 0;
    int rejected_batches = 0;
    int batch_splits = 0;
    int split_depth_limit_hits = 0;
    int max_split_depth_reached = 0;
    std::vector<std::string> selected_targets;
    std::vector<std::string> inserted_buffers;
    std::vector<std::string> moves;
};

bool is_buffer_node(const ClockNode* node) {
    return node && !node->is_sink && !node->type.empty();
}

bool timing_not_worse(const TimingAnalysisResult& trial,
                      const TimingAnalysisResult& current,
                      double eps = 1e-9) {
    return trial.ss.tns >= current.ss.tns - eps &&
           trial.ff.tns >= current.ff.tns - eps &&
           trial.ss.wns >= current.ss.wns - eps &&
           trial.ff.wns >= current.ff.wns - eps;
}

double final_alt_timing_cost(const TimingAnalysisResult& timing) {
    return std::abs(timing.ss.tns) +
           std::abs(timing.ff.tns) +
           0.5 * (std::abs(timing.ss.wns) + std::abs(timing.ff.wns));
}

double repair_reclaim_progress_cost(const TimingAnalysisResult& timing) {
    return std::abs(std::min(0.0, timing.ss.tns)) +
           std::abs(std::min(0.0, timing.ff.tns)) +
           0.5 * (std::abs(std::min(0.0, timing.ss.wns)) +
                  std::abs(std::min(0.0, timing.ff.wns)));
}

bool final_alt_needs_repair(const TimingAnalysisResult& timing,
                            const OptimizerConfig& cfg) {
    return timing.ss.tns < cfg.final_alt_repair_ss_tns_threshold ||
           timing.ss.wns < cfg.final_alt_repair_ss_wns_threshold ||
           static_cast<int>(timing.ss.violating_paths) >
               cfg.final_alt_repair_ss_violation_threshold ||
           timing.ff.tns < cfg.final_alt_repair_ff_tns_threshold ||
           static_cast<int>(timing.ff.violating_paths) >
               cfg.final_alt_repair_ff_violation_threshold;
}

bool final_alt_within_reclaim_budget(const TimingAnalysisResult& trial,
                                     const TimingAnalysisResult& baseline,
                                     double allowed_cost_giveback,
                                     bool hard_wns_guard,
                                     double eps = 1e-9) {
    if (final_alt_timing_cost(trial) >
        final_alt_timing_cost(baseline) + allowed_cost_giveback + eps) {
        return false;
    }
    if (!hard_wns_guard) return true;
    return trial.ss.wns >= baseline.ss.wns - eps &&
           trial.ff.wns >= baseline.ff.wns - eps;
}

bool is_physically_smaller(const BufSpec& lhs, const BufSpec& rhs, const DelayModel& model) {
    constexpr double eps = 1e-9;
    const double lhs_area = model.estimate_buffer_area(lhs);
    const double rhs_area = model.estimate_buffer_area(rhs);
    if (lhs_area + eps < rhs_area) return true;
    if (rhs_area + eps < lhs_area) return false;

    const double lhs_delay = 0.5 * (model.buffer_delay_ss(lhs.name, 1) + model.buffer_delay_ff(lhs.name, 1));
    const double rhs_delay = 0.5 * (model.buffer_delay_ss(rhs.name, 1) + model.buffer_delay_ff(rhs.name, 1));
    return lhs_delay > rhs_delay + eps;
}

const BufSpec* choose_medium_buffer(const std::vector<BufSpec>& libs,
                                    const DelayModel& model,
                                    size_t fanout_a,
                                    size_t fanout_b) {
    std::vector<const BufSpec*> candidates;
    candidates.reserve(libs.size());
    for (const auto& lib : libs) {
        if (fanout_a > lib.ss_delay.size() || fanout_b > lib.ss_delay.size()) continue;
        candidates.push_back(&lib);
    }
    if (candidates.empty()) return nullptr;

    std::vector<double> areas;
    areas.reserve(candidates.size());
    for (const BufSpec* candidate : candidates) {
        areas.push_back(model.estimate_buffer_area(*candidate));
    }
    std::sort(areas.begin(), areas.end());
    double median_area = areas[areas.size() / 2];
    if (areas.size() % 2 == 0) {
        median_area = 0.5 * (areas[areas.size() / 2 - 1] + areas[areas.size() / 2]);
    }

    const size_t eval_fanout = std::max(fanout_a, fanout_b);
    const BufSpec* best = nullptr;
    double best_distance = std::numeric_limits<double>::infinity();
    double best_delay = std::numeric_limits<double>::infinity();
    constexpr double eps = 1e-9;

    for (const BufSpec* candidate : candidates) {
        const double area = model.estimate_buffer_area(*candidate);
        const double distance = std::abs(area - median_area);
        const double delay = 0.5 * (model.buffer_delay_ss(candidate->name, eval_fanout) + model.buffer_delay_ff(candidate->name, eval_fanout));
        if (!best || distance + eps < best_distance ||
            (std::abs(distance - best_distance) <= eps && delay + eps < best_delay)) {
            best = candidate;
            best_distance = distance;
            best_delay = delay;
        }
    }

    return best;
}

bool apply_parallel_merge(ClockTree& tree,
                          ClockNode* parent,
                          ClockNode* survivor,
                          const std::vector<size_t>& remove_indices,
                          ParallelMergeRollback& rollback) {
    if (!parent || !survivor) return false;

    rollback.parent_name = parent->name;
    rollback.survivor_name = survivor->name;
    rollback.removed_nodes.clear();

    std::vector<size_t> sorted_indices = remove_indices;
    std::sort(sorted_indices.begin(), sorted_indices.end(), std::greater<size_t>());

    for (size_t index : sorted_indices) {
        if (index >= parent->children.size()) return false;

        ClockNode* node = parent->children[index].get();
        if (!node || node == survivor || node->is_sink || node->type != survivor->type || node->original) {
            return false;
        }

        ParallelMergeRollbackEntry entry;
        entry.insert_index = index;
        entry.moved_child_count = node->children.size();
        entry.removed_node = std::move(parent->children[index]);

        for (auto& child : entry.removed_node->children) {
            if (!child) continue;
            child->parent = survivor;
            survivor->children.push_back(std::move(child));
        }
        entry.removed_node->children.clear();

        tree.remove_node_from_index(entry.removed_node->name);
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(index));
        rollback.removed_nodes.push_back(std::move(entry));
    }

    return true;
}

bool rollback_parallel_merge(ClockTree& tree, ParallelMergeRollback& rollback) {
    ClockNode* parent = tree.find_node(rollback.parent_name);
    ClockNode* survivor = tree.find_node(rollback.survivor_name);
    if (!parent || !survivor) return false;

    for (auto it = rollback.removed_nodes.rbegin(); it != rollback.removed_nodes.rend(); ++it) {
        ParallelMergeRollbackEntry& entry = *it;
        if (survivor->children.size() < entry.moved_child_count) return false;

        std::vector<std::unique_ptr<ClockNode>> moved_children;
        moved_children.reserve(entry.moved_child_count);
        for (size_t i = 0; i < entry.moved_child_count; ++i) {
            moved_children.push_back(std::move(survivor->children.back()));
            survivor->children.pop_back();
        }

        for (auto rit = moved_children.rbegin(); rit != moved_children.rend(); ++rit) {
            if (!*rit) continue;
            (*rit)->parent = entry.removed_node.get();
            entry.removed_node->children.push_back(std::move(*rit));
        }

        entry.removed_node->parent = parent;
        ClockNode* restored = entry.removed_node.get();
        size_t insert_index = std::min(entry.insert_index, parent->children.size());
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(insert_index),
                                std::move(entry.removed_node));
        tree.add_node_to_index(restored);
    }

    return true;
}

const BufSpec* find_lib_by_name(const std::vector<BufSpec>& libs, const std::string& name) {
    for (const auto& lib : libs) {
        if (lib.name == name) return &lib;
    }
    return nullptr;
}

double buffer_nominal_delay(const DelayModel& model, const BufSpec& buffer, size_t fanout) {
    return 0.5 * (model.buffer_delay_ss(buffer.name, fanout) + model.buffer_delay_ff(buffer.name, fanout));
}

double buffer_nominal_delay_from_lib(const BufSpec& buffer, size_t fanout) {
    if (fanout == 0 || buffer.ss_delay.empty() || buffer.ff_delay.empty()) return 0.0;
    const size_t ss_index = std::min(fanout - 1, buffer.ss_delay.size() - 1);
    const size_t ff_index = std::min(fanout - 1, buffer.ff_delay.size() - 1);
    return 0.5 * (buffer.ss_delay[ss_index] + buffer.ff_delay[ff_index]);
}

double buffer_area(const DelayModel& model, const BufSpec& buffer) {
    return model.estimate_buffer_area(buffer);
}

const BufSpec* choose_smallest_buffer(const std::vector<BufSpec>& libs, const DelayModel& model) {
    const BufSpec* best = nullptr;
    double best_area = std::numeric_limits<double>::infinity();
    double best_delay = std::numeric_limits<double>::infinity();
    constexpr double eps = 1e-9;

    for (const auto& lib : libs) {
        double area = buffer_area(model, lib);
        double delay = buffer_nominal_delay(model, lib, 1);
        if (area + eps < best_area || (std::abs(area - best_area) <= eps && delay + eps < best_delay)) {
            best = &lib;
            best_area = area;
            best_delay = delay;
        }
    }
    return best;
}

const BufSpec* choose_next_smaller_buffer(const std::vector<BufSpec>& libs,
                                          const DelayModel& model,
                                          const std::string& current_type,
                                          size_t fanout) {
    const BufSpec* current = find_lib_by_name(libs, current_type);
    if (!current) return nullptr;

    const double current_area = buffer_area(model, *current);
    const BufSpec* best = nullptr;
    double best_area = -std::numeric_limits<double>::infinity();
    double best_delay = std::numeric_limits<double>::infinity();
    constexpr double eps = 1e-9;

    for (const auto& lib : libs) {
        if (fanout > lib.ss_delay.size()) continue;
        double area = buffer_area(model, lib);
        if (area + eps >= current_area) continue;
        double delay = buffer_nominal_delay(model, lib, fanout);
        if (area > best_area + eps || (std::abs(area - best_area) <= eps && delay + eps < best_delay)) {
            best = &lib;
            best_area = area;
            best_delay = delay;
        }
    }
    return best;
}

std::vector<const BufSpec*> choose_smaller_buffers(
    const std::vector<BufSpec>& libs,
    const DelayModel& model,
    const std::string& current_type,
    size_t fanout,
    int max_types) {
    std::vector<const BufSpec*> candidates;
    if (max_types <= 0) return candidates;
    const BufSpec* current = find_lib_by_name(libs, current_type);
    if (!current) return candidates;

    constexpr double eps = 1e-9;
    const double current_area = buffer_area(model, *current);
    for (const auto& lib : libs) {
        if (fanout > lib.ss_delay.size() ||
            fanout > lib.ff_delay.size()) {
            continue;
        }
        if (buffer_area(model, lib) + eps >= current_area) continue;
        candidates.push_back(&lib);
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [&](const BufSpec* lhs, const BufSpec* rhs) {
                  const double lhs_area = buffer_area(model, *lhs);
                  const double rhs_area = buffer_area(model, *rhs);
                  if (std::abs(lhs_area - rhs_area) > eps) {
                      return lhs_area > rhs_area;
                  }
                  const double lhs_delay =
                      buffer_nominal_delay(model, *lhs, fanout);
                  const double rhs_delay =
                      buffer_nominal_delay(model, *rhs, fanout);
                  if (std::abs(lhs_delay - rhs_delay) > eps) {
                      return lhs_delay < rhs_delay;
                  }
                  return lhs->name < rhs->name;
              });
    if (candidates.size() > static_cast<size_t>(max_types)) {
        candidates.resize(static_cast<size_t>(max_types));
    }
    return candidates;
}

WorstPathChoice choose_worst_target(const TimingAnalysisResult& timing,
                                   const std::set<std::string>& blacklist) {
    WorstPathChoice best;
    double best_slack = std::numeric_limits<double>::infinity();

    for (const auto& path : timing.path_results) {
        const bool setup_bad = path.setup_slack < 0.0;
        const bool hold_bad = path.hold_slack < 0.0;
        if (!setup_bad && !hold_bad) continue;

        const bool use_setup = setup_bad && (!hold_bad || path.setup_slack <= path.hold_slack);
        const std::string& candidate_node = use_setup ? path.capture_ff : path.launch_ff;
        const double candidate_slack = use_setup ? path.setup_slack : path.hold_slack;
        if (blacklist.find(candidate_node) != blacklist.end()) continue;

        if (!best.valid || candidate_slack < best_slack) {
            best.valid = true;
            best.target_node = candidate_node;
            best.path_name = path.path_name;
            best.violation_slack = candidate_slack;
            best.is_setup = use_setup;
            best_slack = candidate_slack;
        }
    }

    return best;
}

void collect_preorder_helper(const ClockNode* node, std::vector<std::string>& names) {
    if (!node) return;
    names.push_back(node->name);
    for (const auto& child : node->children) {
        collect_preorder_helper(child.get(), names);
    }
}

std::vector<std::string> collect_preorder_names(const ClockNode* node) {
    std::vector<std::string> names;
    collect_preorder_helper(node, names);
    return names;
}

[[maybe_unused]] size_t total_violations(
    const TimingAnalysisResult& timing) {
    return timing.ss.violating_paths + timing.ff.violating_paths;
}

double average_library_delay(const BufSpec& lib) {
    double total = 0.0;
    size_t count = 0;
    for (double delay : lib.ss_delay) {
        total += delay;
        ++count;
    }
    for (double delay : lib.ff_delay) {
        total += delay;
        ++count;
    }
    return count == 0 ? std::numeric_limits<double>::infinity()
                      : total / static_cast<double>(count);
}

std::vector<ClockNode*> collect_buffer_nodes(ClockTree& tree) {
    std::vector<ClockNode*> nodes;
    std::function<void(ClockNode*)> dfs = [&](ClockNode* node) {
        if (!node) return;
        if (is_buffer_node(node)) nodes.push_back(node);
        for (auto& child : node->children) {
            dfs(child.get());
        }
    };
    dfs(tree.root.get());
    return nodes;
}

const BufSpec* choose_fastest_compatible_buffer(const std::vector<BufSpec>& libs,
                                                const DelayModel& model,
                                                size_t required_fanout) {
    const BufSpec* best = nullptr;
    double best_delay = std::numeric_limits<double>::infinity();
    double best_area = std::numeric_limits<double>::infinity();
    constexpr double eps = 1e-12;

    for (const auto& lib : libs) {
        if (lib.ss_delay.size() < required_fanout || lib.ff_delay.size() < required_fanout) continue;
        const double delay = average_library_delay(lib);
        const double area = model.estimate_buffer_area(lib);
        if (!best || delay + eps < best_delay ||
            (std::abs(delay - best_delay) <= eps && area + eps < best_area)) {
            best = &lib;
            best_delay = delay;
            best_area = area;
        }
    }
    return best;
}

double normalized_phase0_timing_cost(const TimingAnalysisResult& timing,
                                     const TimingAnalysisResult& normalization,
                                     double wns_weight) {
    constexpr double eps_norm = 1e-9;
    const double ss_tns_norm = std::max(std::abs(normalization.ss.tns), eps_norm);
    const double ff_tns_norm = std::max(std::abs(normalization.ff.tns), eps_norm);
    const double ss_wns_norm = std::max(std::abs(normalization.ss.wns), eps_norm);
    const double ff_wns_norm = std::max(std::abs(normalization.ff.wns), eps_norm);
    return std::abs(timing.ss.tns) / ss_tns_norm +
           std::abs(timing.ff.tns) / ff_tns_norm +
           wns_weight * (std::abs(timing.ss.wns) / ss_wns_norm +
                         std::abs(timing.ff.wns) / ff_wns_norm);
}

double weighted_objective_score(const DelayModel& model,
                                const TimingAnalysisResult& timing,
                                double area,
                                const BaselineSnapshot& baseline,
                                const OptimizerConfig& cfg) {
    return model.compute_score_metrics(timing,
                                       baseline.timing,
                                       area,
                                       baseline.area,
                                       cfg.alpha,
                                       cfg.beta,
                                       cfg.gamma).total_score;
}

bool score_not_worse(double trial_score,
                     double current_score,
                     const OptimizerConfig& cfg) {
    return trial_score >= current_score - cfg.score_acceptance_epsilon;
}

bool score_strictly_better(double trial_score,
                           double current_score,
                           const OptimizerConfig& cfg) {
    return trial_score > current_score + cfg.score_acceptance_epsilon;
}

std::unordered_map<std::string, double> compute_phase0_pressure(
    const ClockTree& tree,
    const TimingAnalysisResult& timing,
    const TimingAnalysisResult& normalization,
    const std::function<bool()>& stop_requested = {},
    bool* stopped_early = nullptr) {
    constexpr double eps_norm = 1e-9;
    const double ss_wns_norm = std::max(std::abs(normalization.ss.wns), eps_norm);
    const double ff_wns_norm = std::max(std::abs(normalization.ff.wns), eps_norm);
    std::unordered_map<std::string, double> pressure;
    bool cancelled = false;
    size_t work_items = 0;
    auto should_stop = [&]() {
        if (cancelled) return true;
        if ((work_items++ & 255U) != 0U || !stop_requested) return false;
        cancelled = stop_requested();
        return cancelled;
    };

    auto add_path_pressure = [&](const std::string& positive_ff,
                                 const std::string& negative_ff,
                                 double weight) {
        const ClockNode* positive = tree.find_node(positive_ff);
        const ClockNode* negative = tree.find_node(negative_ff);
        if (!positive || !negative || weight <= 0.0) return;

        std::unordered_set<const ClockNode*> negative_ancestors;
        for (const ClockNode* node = negative->parent; node; node = node->parent) {
            if (should_stop()) return;
            negative_ancestors.insert(node);
        }

        const ClockNode* lca = nullptr;
        for (const ClockNode* node = positive->parent; node; node = node->parent) {
            if (should_stop()) return;
            if (negative_ancestors.count(node) != 0) {
                lca = node;
                break;
            }
        }

        for (const ClockNode* node = positive->parent; node && node != lca; node = node->parent) {
            if (should_stop()) return;
            if (is_buffer_node(node)) pressure[node->name] += weight;
        }
        for (const ClockNode* node = negative->parent; node && node != lca; node = node->parent) {
            if (should_stop()) return;
            if (is_buffer_node(node)) pressure[node->name] -= weight;
        }
    };

    for (const auto& path : timing.path_results) {
        if (should_stop()) break;
        if (path.setup_slack < 0.0) {
            add_path_pressure(path.capture_ff,
                              path.launch_ff,
                              -path.setup_slack / ss_wns_norm);
        }
        if (path.hold_slack < 0.0) {
            add_path_pressure(path.launch_ff,
                              path.capture_ff,
                              -path.hold_slack / ff_wns_norm);
        }
        if (cancelled) break;
    }
    if (cancelled && stopped_early) *stopped_early = true;
    return pressure;
}

std::unordered_map<std::string, double> compute_endpoint_subtree_pressure(
    const ClockTree& tree,
    const TimingAnalysisResult& timing,
    const std::function<bool()>& stop_requested = {},
    bool* stopped_early = nullptr) {
    std::unordered_map<std::string, double> endpoint_delta;
    endpoint_delta.reserve(timing.path_results.size() * 2);
    size_t work_items = 0;
    bool cancelled = false;
    for (const auto& path : timing.path_results) {
        if ((work_items++ & 255U) == 0U && stop_requested && stop_requested()) {
            cancelled = true;
            break;
        }
        if (path.setup_slack < 0.0) {
            const double weight = -path.setup_slack;
            endpoint_delta[path.capture_ff] += weight;
            endpoint_delta[path.launch_ff] -= weight;
        }
        if (path.hold_slack < 0.0) {
            const double weight = -path.hold_slack;
            endpoint_delta[path.launch_ff] += weight;
            endpoint_delta[path.capture_ff] -= weight;
        }
    }

    std::unordered_map<std::string, double> pressure;
    pressure.reserve(tree.node_count());
    std::function<double(const ClockNode*)> postorder = [&](const ClockNode* node) -> double {
        if (!node || cancelled) return 0.0;
        if ((work_items++ & 255U) == 0U && stop_requested && stop_requested()) {
            cancelled = true;
            return 0.0;
        }
        double total = 0.0;
        auto it = endpoint_delta.find(node->name);
        if (it != endpoint_delta.end()) total += it->second;
        for (const auto& child : node->children) {
            total += postorder(child.get());
        }
        pressure[node->name] = total;
        return total;
    };
    if (!cancelled) postorder(tree.root.get());
    if (cancelled && stopped_early) *stopped_early = true;
    return pressure;
}

std::vector<ReclaimCandidate> build_pressure_guided_reclaim_candidates(
    ClockTree& tree,
    const LibraryCache& lib_cache,
    const std::unordered_map<std::string, double>& pressure_by_node,
    const OptimizerConfig& cfg,
    double* sorting_seconds = nullptr,
    const std::function<bool()>& stop_requested = {},
    bool* stopped_early = nullptr) {
    std::vector<ReclaimCandidate> candidates;
    const double eps = cfg.area_comparison_epsilon;
    const double area_weight = cfg.reclaim_rank_area_weight;
    const double timing_help_weight = cfg.reclaim_rank_timing_help_weight;
    const double timing_harm_weight = cfg.reclaim_rank_timing_harm_weight;
    const double delete_bonus = cfg.reclaim_rank_delete_bonus;

    std::vector<ClockNode*> buffer_nodes = collect_buffer_nodes(tree);
    candidates.reserve(buffer_nodes.size() * std::max<size_t>(1, lib_cache.type_by_id.size() / 2));

    size_t visited_nodes = 0;
    for (ClockNode* node : buffer_nodes) {
        if ((visited_nodes++ & 255U) == 0U && stop_requested && stop_requested()) {
            if (stopped_early) *stopped_early = true;
            return {};
        }
        if (!node) continue;
        const int current_type_id = node->buffer_type_id >= 0
            ? node->buffer_type_id
            : lib_cache.get_type_id(node->type);
        const BufSpec* current_lib = current_type_id < 0
            ? nullptr
            : lib_cache.type_by_id[static_cast<size_t>(current_type_id)];
        if (!current_lib) continue;

        const bool is_new_buffer = !node->original;
        const bool resize_allowed =
            (node->original && cfg.reclaim_allow_original_resize) ||
            (is_new_buffer && cfg.reclaim_allow_newbuf_resize);
        const double current_area = lib_cache.area[static_cast<size_t>(current_type_id)];
        const size_t fanout = node->children.size();
        const double current_delay = buffer_nominal_delay_from_lib(*current_lib, fanout);
        const double pressure = [&]() {
            auto it = pressure_by_node.find(node->name);
            return it == pressure_by_node.end() ? 0.0 : it->second;
        }();

        if (resize_allowed) {
            static const std::vector<int> no_types;
            const auto& by_fanout =
                lib_cache.smaller_types_by_type_and_fanout[static_cast<size_t>(current_type_id)];
            const auto& smaller_types = fanout < by_fanout.size()
                ? by_fanout[fanout]
                : no_types;
            for (int new_type_id : smaller_types) {
                const BufSpec& lib = *lib_cache.type_by_id[static_cast<size_t>(new_type_id)];
                const double new_area = lib_cache.area[static_cast<size_t>(new_type_id)];
                const double area_save = current_area - new_area;
                if (!cfg.reclaim_allow_area_increasing_moves && area_save <= eps) continue;
                if (area_save <= eps) continue;

                const double new_delay = buffer_nominal_delay_from_lib(lib, fanout);
                ReclaimCandidate candidate;
                candidate.kind = node->original ? ReclaimCandidateKind::ResizeOriginal
                                                : ReclaimCandidateKind::ResizeNewBuffer;
                candidate.node_name = node->name;
                candidate.old_type = current_lib->name;
                candidate.new_type = lib.name;
                candidate.area_save = area_save;
                candidate.pressure = pressure;
                candidate.delay_delta = new_delay - current_delay;
                candidate.pressure_effect = pressure * candidate.delay_delta;
                candidate.score =
                    area_save * area_weight +
                    std::max(0.0, candidate.pressure_effect) * timing_help_weight -
                    std::max(0.0, -candidate.pressure_effect) * timing_harm_weight;
                candidates.push_back(candidate);
            }
        }

        if (is_new_buffer && cfg.reclaim_allow_newbuf_delete) {
            ReclaimCandidate candidate;
            candidate.kind = ReclaimCandidateKind::DeleteNewBuffer;
            candidate.node_name = node->name;
            candidate.old_type = current_lib->name;
            candidate.area_save = current_area;
            candidate.pressure = pressure;
            candidate.delay_delta = -current_delay;
            candidate.pressure_effect = pressure * candidate.delay_delta;
            candidate.score =
                current_area * area_weight +
                std::max(0.0, candidate.pressure_effect) * timing_help_weight -
                std::max(0.0, -candidate.pressure_effect) * timing_harm_weight +
                delete_bonus;
            candidates.push_back(candidate);
        }
    }

    if (stop_requested && stop_requested()) {
        if (stopped_early) *stopped_early = true;
        return {};
    }
    const auto sort_start = std::chrono::steady_clock::now();
    std::sort(candidates.begin(), candidates.end(), [](const ReclaimCandidate& lhs,
                                                       const ReclaimCandidate& rhs) {
        constexpr double sort_eps = 1e-9;
        if (std::abs(lhs.score - rhs.score) > sort_eps) return lhs.score > rhs.score;
        if (std::abs(lhs.area_save - rhs.area_save) > sort_eps) return lhs.area_save > rhs.area_save;
        const bool lhs_helpful = lhs.pressure_effect >= 0.0;
        const bool rhs_helpful = rhs.pressure_effect >= 0.0;
        if (lhs_helpful != rhs_helpful) return lhs_helpful;
        if (std::abs(lhs.pressure_effect - rhs.pressure_effect) > sort_eps) {
            return lhs.pressure_effect > rhs.pressure_effect;
        }
        if (lhs.kind != rhs.kind) {
            return lhs.kind == ReclaimCandidateKind::DeleteNewBuffer;
        }
        return lhs.node_name < rhs.node_name;
    });
    if (sorting_seconds) {
        *sorting_seconds +=
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - sort_start).count();
    }
    if (stop_requested && stop_requested()) {
        if (stopped_early) *stopped_early = true;
        return {};
    }

    return candidates;
}

double tns_abs_degradation(const TimingCornerResult& trial,
                           const TimingCornerResult& baseline) {
    return std::max(0.0, std::abs(trial.tns) - std::abs(baseline.tns));
}

bool timing_within_reclaim_budget(const TimingAnalysisResult& trial,
                                  const TimingAnalysisResult& post_pulse,
                                  double allowed_ss_tns_giveback,
                                  double allowed_ff_tns_giveback,
                                  const OptimizerConfig& cfg) {
    constexpr double eps = 1e-9;
    const size_t allowed_ss_violations =
        post_pulse.ss.violating_paths +
        static_cast<size_t>(std::ceil(
            cfg.reclaim_allowed_violation_growth_ratio *
            static_cast<double>(post_pulse.ss.violating_paths)));
    const size_t allowed_ff_violations =
        post_pulse.ff.violating_paths +
        static_cast<size_t>(std::ceil(
            cfg.reclaim_allowed_violation_growth_ratio *
            static_cast<double>(post_pulse.ff.violating_paths)));

    return std::abs(trial.ss.tns) <= std::abs(post_pulse.ss.tns) + allowed_ss_tns_giveback + eps &&
           std::abs(trial.ff.tns) <= std::abs(post_pulse.ff.tns) + allowed_ff_tns_giveback + eps &&
           trial.ss.wns >= post_pulse.ss.wns - cfg.reclaim_allowed_wns_worsen - eps &&
           trial.ff.wns >= post_pulse.ff.wns - cfg.reclaim_allowed_wns_worsen - eps &&
           trial.ss.violating_paths <= allowed_ss_violations &&
           trial.ff.violating_paths <= allowed_ff_violations;
}

bool buffer_type_supports_fanout(const BufSpec* lib, size_t fanout) {
    return lib && fanout <= lib->ss_delay.size() && fanout <= lib->ff_delay.size();
}

bool local_resize_legality(const ClockNode* node,
                           const BufSpec* new_lib) {
    return node &&
           !node->is_sink &&
           !node->type.empty() &&
           buffer_type_supports_fanout(new_lib, node->children.size());
}

LibraryCache build_library_cache(const std::vector<BufSpec>& libs,
                                 long long* lookups = nullptr,
                                 long long* misses = nullptr) {
    LibraryCache cache;
    cache.lookups = lookups;
    cache.misses = misses;
    cache.type_id_by_name.reserve(libs.size());
    cache.type_by_id.reserve(libs.size());
    cache.type_name.reserve(libs.size());
    cache.area.reserve(libs.size());
    cache.ss_delay.reserve(libs.size());
    cache.ff_delay.reserve(libs.size());
    cache.max_fanout.reserve(libs.size());
    for (size_t i = 0; i < libs.size(); ++i) {
        const int type_id = static_cast<int>(i);
        cache.type_id_by_name.emplace(libs[i].name, type_id);
        cache.type_by_id.push_back(&libs[i]);
        cache.type_name.push_back(libs[i].name);
        cache.area.push_back(libs[i].width * libs[i].height);
        cache.ss_delay.push_back(libs[i].ss_delay);
        cache.ff_delay.push_back(libs[i].ff_delay);
        cache.max_fanout.push_back(std::min(libs[i].ss_delay.size(), libs[i].ff_delay.size()));
        cache.types_by_area_ascending.push_back(type_id);
        cache.types_by_area_descending.push_back(type_id);
    }
    auto area_less = [&](int lhs, int rhs) {
        const double lhs_area = cache.area[static_cast<size_t>(lhs)];
        const double rhs_area = cache.area[static_cast<size_t>(rhs)];
        if (std::abs(lhs_area - rhs_area) > 1e-12) return lhs_area < rhs_area;
        return lhs < rhs;
    };
    std::sort(cache.types_by_area_ascending.begin(),
              cache.types_by_area_ascending.end(),
              area_less);
    cache.types_by_area_descending = cache.types_by_area_ascending;
    std::reverse(cache.types_by_area_descending.begin(),
                 cache.types_by_area_descending.end());
    size_t global_max_fanout = 0;
    for (size_t value : cache.max_fanout) global_max_fanout = std::max(global_max_fanout, value);
    cache.smaller_types_by_type_and_fanout.resize(libs.size());
    for (size_t current = 0; current < libs.size(); ++current) {
        auto& by_fanout = cache.smaller_types_by_type_and_fanout[current];
        by_fanout.resize(global_max_fanout + 1);
        for (size_t fanout = 0; fanout <= global_max_fanout; ++fanout) {
            for (int candidate : cache.types_by_area_ascending) {
                if (cache.area[static_cast<size_t>(candidate)] + 1e-12 >= cache.area[current]) continue;
                if (cache.supports_fanout(candidate, fanout)) by_fanout[fanout].push_back(candidate);
            }
        }
    }
    return cache;
}

TreeIndexCache build_tree_index_cache(ClockTree& tree) {
    TreeIndexCache index;
    if (!tree.root) return index;
    index.nodes.reserve(tree.node_count());
    index.parent_id.reserve(tree.node_count());
    index.children_ids.reserve(tree.node_count());

    std::function<int(ClockNode*, int)> dfs = [&](ClockNode* node, int parent_id) -> int {
        if (!node) return -1;
        const int node_id = static_cast<int>(index.nodes.size());
        index.nodes.push_back(node);
        index.parent_id.push_back(parent_id);
        index.children_ids.emplace_back();
        index.subtree_end.push_back(node_id + 1);
        index.node_id_by_name.emplace(node->name, node_id);
        index.preorder.push_back(node_id);

        if (node->is_sink) {
            const int ff_id = static_cast<int>(index.ff_node_id_by_ff_id.size());
            index.ff_id_by_name.emplace(node->name, ff_id);
            index.ff_node_id_by_ff_id.push_back(node_id);
            index.ff_node_ids.push_back(node_id);
        }

        for (auto& child : node->children) {
            const int child_id = dfs(child.get(), node_id);
            if (child_id >= 0) index.children_ids[static_cast<size_t>(node_id)].push_back(child_id);
        }
        index.subtree_end[static_cast<size_t>(node_id)] =
            static_cast<int>(index.nodes.size());
        index.postorder.push_back(node_id);
        return node_id;
    };

    dfs(tree.root.get(), -1);
    return index;
}

std::vector<IndexedPathInfo> build_indexed_paths(
    const std::vector<PathInfo>& paths,
    const TreeIndexCache& tree_index,
    bool is_setup_path) {
    std::vector<IndexedPathInfo> indexed;
    indexed.reserve(paths.size());
    for (const PathInfo& path : paths) {
        auto launch_it = tree_index.ff_id_by_name.find(path.launch_ff);
        auto capture_it = tree_index.ff_id_by_name.find(path.capture_ff);
        if (launch_it == tree_index.ff_id_by_name.end() ||
            capture_it == tree_index.ff_id_by_name.end()) {
            continue;
        }
        IndexedPathInfo item;
        item.original = &path;
        item.launch_ff_id = launch_it->second;
        item.capture_ff_id = capture_it->second;
        item.data_delay = path.data_delay;
        item.is_setup_path = is_setup_path;
        indexed.push_back(item);
    }
    return indexed;
}

void compute_clock_arrivals_indexed(const TreeIndexCache& tree_index,
                                    const LibraryCache& lib_cache,
                                    bool ss_corner,
                                    std::vector<double>& arrival_by_ff_id) {
    arrival_by_ff_id.assign(tree_index.ff_node_id_by_ff_id.size(), 0.0);
    if (tree_index.nodes.empty()) return;

    std::vector<double> arrival_by_node(tree_index.nodes.size(), 0.0);
    for (int node_id : tree_index.preorder) {
        const ClockNode* node = tree_index.nodes[static_cast<size_t>(node_id)];
        double node_delay = 0.0;
        if (node && !node->is_sink && !node->type.empty()) {
            const int type_id = lib_cache.get_type_id(node->type);
            node_delay = lib_cache.delay(type_id, node->children.size(), ss_corner);
        }
        const double child_arrival = arrival_by_node[static_cast<size_t>(node_id)] + node_delay;
        for (int child_id : tree_index.children_ids[static_cast<size_t>(node_id)]) {
            arrival_by_node[static_cast<size_t>(child_id)] = child_arrival;
        }
    }

    for (size_t ff_id = 0; ff_id < tree_index.ff_node_id_by_ff_id.size(); ++ff_id) {
        const int node_id = tree_index.ff_node_id_by_ff_id[ff_id];
        arrival_by_ff_id[ff_id] = arrival_by_node[static_cast<size_t>(node_id)];
    }
}

TimingAnalysisResult analyze_timing_indexed(
    const std::vector<double>& ss_arrival,
    const std::vector<double>& ff_arrival,
    const std::vector<IndexedPathInfo>& indexed_ss_paths,
    const std::vector<IndexedPathInfo>& indexed_ff_paths,
    double clock_period) {
    TimingAnalysisResult result;
    result.clock_period = clock_period;
    result.t_setup = 0.08 * clock_period;
    result.t_hold = 0.05 * clock_period;

    std::unordered_map<long long, size_t> merged_by_pair;
    merged_by_pair.reserve(indexed_ss_paths.size() + indexed_ff_paths.size());
    auto pair_key = [](int launch_id, int capture_id) -> long long {
        return (static_cast<long long>(launch_id) << 32) ^
               static_cast<unsigned int>(capture_id);
    };

    for (const IndexedPathInfo& path : indexed_ss_paths) {
        if (!path.original || path.launch_ff_id < 0 || path.capture_ff_id < 0) continue;
        const double launch_clk = ss_arrival[static_cast<size_t>(path.launch_ff_id)];
        const double capture_clk = ss_arrival[static_cast<size_t>(path.capture_ff_id)];
        const double skew = capture_clk - launch_clk;
        const double setup_slack =
            clock_period - result.t_setup - path.data_delay + skew;
        const long long key = pair_key(path.launch_ff_id, path.capture_ff_id);
        auto it = merged_by_pair.find(key);
        if (it == merged_by_pair.end()) {
            TimingPathResult item;
            item.path_name = path.original->name;
            item.launch_ff = path.original->launch_ff;
            item.capture_ff = path.original->capture_ff;
            item.data_delay = path.data_delay;
            item.launch_clk_delay = launch_clk;
            item.capture_clk_delay = capture_clk;
            item.skew = skew;
            item.setup_slack = setup_slack;
            merged_by_pair.emplace(key, result.path_results.size());
            result.path_results.push_back(item);
        } else {
            TimingPathResult& item = result.path_results[it->second];
            item.setup_slack = std::min(item.setup_slack, setup_slack);
        }
    }

    for (const IndexedPathInfo& path : indexed_ff_paths) {
        if (!path.original || path.launch_ff_id < 0 || path.capture_ff_id < 0) continue;
        const double launch_clk = ff_arrival[static_cast<size_t>(path.launch_ff_id)];
        const double capture_clk = ff_arrival[static_cast<size_t>(path.capture_ff_id)];
        const double skew = capture_clk - launch_clk;
        const double hold_slack = path.data_delay - result.t_hold - skew;
        const long long key = pair_key(path.launch_ff_id, path.capture_ff_id);
        auto it = merged_by_pair.find(key);
        if (it == merged_by_pair.end()) {
            TimingPathResult item;
            item.path_name = path.original->name;
            item.launch_ff = path.original->launch_ff;
            item.capture_ff = path.original->capture_ff;
            item.data_delay = path.data_delay;
            item.launch_clk_delay = launch_clk;
            item.capture_clk_delay = capture_clk;
            item.skew = skew;
            item.hold_slack = hold_slack;
            merged_by_pair.emplace(key, result.path_results.size());
            result.path_results.push_back(item);
        } else {
            TimingPathResult& item = result.path_results[it->second];
            item.launch_clk_delay = launch_clk;
            item.capture_clk_delay = capture_clk;
            item.skew = skew;
            item.hold_slack = std::min(item.hold_slack, hold_slack);
        }
    }

    for (const TimingPathResult& path : result.path_results) {
        if (path.setup_slack < 0.0) {
            result.ss.tns += path.setup_slack;
            result.ss.violating_paths += 1;
            result.ss.wns =
                result.ss.violating_paths == 1
                    ? path.setup_slack
                    : std::min(result.ss.wns, path.setup_slack);
        }
        if (path.hold_slack < 0.0) {
            result.ff.tns += path.hold_slack;
            result.ff.violating_paths += 1;
            result.ff.wns =
                result.ff.violating_paths == 1
                    ? path.hold_slack
                    : std::min(result.ff.wns, path.hold_slack);
        }
    }
    if (result.ss.violating_paths == 0) result.ss.wns = 0.0;
    if (result.ff.violating_paths == 0) result.ff.wns = 0.0;
    std::sort(result.path_results.begin(), result.path_results.end(),
              [](const TimingPathResult& lhs, const TimingPathResult& rhs) {
                  return lhs.path_name < rhs.path_name;
              });
    return result;
}

std::unordered_map<std::string, double> compute_endpoint_delta_pressure_indexed(
    const TreeIndexCache& tree_index,
    const TimingAnalysisResult& timing,
    double ss_weight_scale = 1.0,
    double ff_weight_scale = 1.0,
    const std::function<bool()>& stop_requested = {},
    bool* stopped_early = nullptr) {
    std::vector<double> pressure_by_node(tree_index.nodes.size(), 0.0);
    size_t work_items = 0;
    bool cancelled = false;
    for (const TimingPathResult& path : timing.path_results) {
        if ((work_items++ & 255U) == 0U &&
            stop_requested && stop_requested()) {
            cancelled = true;
            break;
        }
        if (path.setup_slack < 0.0) {
            const double weight = -path.setup_slack * ss_weight_scale;
            auto capture_it = tree_index.node_id_by_name.find(path.capture_ff);
            auto launch_it = tree_index.node_id_by_name.find(path.launch_ff);
            if (capture_it != tree_index.node_id_by_name.end()) {
                pressure_by_node[static_cast<size_t>(capture_it->second)] += weight;
            }
            if (launch_it != tree_index.node_id_by_name.end()) {
                pressure_by_node[static_cast<size_t>(launch_it->second)] -= weight;
            }
        }
        if (path.hold_slack < 0.0) {
            const double weight = -path.hold_slack * ff_weight_scale;
            auto launch_it = tree_index.node_id_by_name.find(path.launch_ff);
            auto capture_it = tree_index.node_id_by_name.find(path.capture_ff);
            if (launch_it != tree_index.node_id_by_name.end()) {
                pressure_by_node[static_cast<size_t>(launch_it->second)] += weight;
            }
            if (capture_it != tree_index.node_id_by_name.end()) {
                pressure_by_node[static_cast<size_t>(capture_it->second)] -= weight;
            }
        }
    }

    for (int node_id : tree_index.postorder) {
        if (cancelled) break;
        if ((work_items++ & 255U) == 0U &&
            stop_requested && stop_requested()) {
            cancelled = true;
            break;
        }
        const int parent_id = tree_index.parent_id[static_cast<size_t>(node_id)];
        if (parent_id >= 0) {
            pressure_by_node[static_cast<size_t>(parent_id)] +=
                pressure_by_node[static_cast<size_t>(node_id)];
        }
    }

    if (cancelled) {
        if (stopped_early) *stopped_early = true;
        return {};
    }

    std::unordered_map<std::string, double> pressure;
    pressure.reserve(tree_index.nodes.size());
    for (size_t node_id = 0; node_id < tree_index.nodes.size(); ++node_id) {
        const ClockNode* node = tree_index.nodes[node_id];
        if (node) pressure.emplace(node->name, pressure_by_node[node_id]);
    }
    return pressure;
}

bool locally_legal_reclaim_resize(const ClockNode* node,
                                  const BufSpec* new_lib) {
    return node &&
           !node->is_sink &&
           buffer_type_supports_fanout(new_lib, node->children.size());
}

void finalize_repair_reclaim_cycle_report_fields(RepairReclaimCycleRecord& cycle,
                                                 double reclaim_giveback_ratio) {
    constexpr double eps = 1e-9;
    cycle.pulse_area_increase = cycle.after_pulse_area - cycle.before_pulse_area;
    cycle.reclaim_area_saved = cycle.after_pulse_area - cycle.after_reclaim_area;
    cycle.net_cycle_area_delta = cycle.after_reclaim_area - cycle.before_pulse_area;
    cycle.reclaim_coverage =
        cycle.reclaim_area_saved / std::max(cycle.pulse_area_increase, eps);

    cycle.pulse_ss_tns_gain =
        std::max(0.0, std::abs(cycle.before_pulse_timing.ss.tns) -
                      std::abs(cycle.after_pulse_timing.ss.tns));
    cycle.pulse_ff_tns_gain =
        std::max(0.0, std::abs(cycle.before_pulse_timing.ff.tns) -
                      std::abs(cycle.after_pulse_timing.ff.tns));
    cycle.allowed_ss_tns_giveback = reclaim_giveback_ratio * cycle.pulse_ss_tns_gain;
    cycle.allowed_ff_tns_giveback = reclaim_giveback_ratio * cycle.pulse_ff_tns_gain;
    cycle.ss_tns_giveback_used =
        std::max(0.0, std::abs(cycle.after_reclaim_timing.ss.tns) -
                      std::abs(cycle.after_pulse_timing.ss.tns));
    cycle.ff_tns_giveback_used =
        std::max(0.0, std::abs(cycle.after_reclaim_timing.ff.tns) -
                      std::abs(cycle.after_pulse_timing.ff.tns));
    cycle.ss_giveback_utilization =
        cycle.ss_tns_giveback_used / std::max(cycle.allowed_ss_tns_giveback, eps);
    cycle.ff_giveback_utilization =
        cycle.ff_tns_giveback_used / std::max(cycle.allowed_ff_tns_giveback, eps);
    cycle.ss_wns_worsen =
        std::max(0.0, cycle.after_pulse_timing.ss.wns -
                      cycle.after_reclaim_timing.ss.wns);
    cycle.ff_wns_worsen =
        std::max(0.0, cycle.after_pulse_timing.ff.wns -
                      cycle.after_reclaim_timing.ff.wns);

    cycle.reclaim_accept_rate =
        static_cast<double>(cycle.reclaim_candidates_accepted) /
        static_cast<double>(std::max(cycle.reclaim_candidates_tried, 1));
    cycle.reclaim_trials_per_second =
        static_cast<double>(cycle.reclaim_candidates_tried) /
        std::max(cycle.reclaim_runtime_seconds, eps);
    cycle.reclaim_area_saved_per_second =
        cycle.reclaim_area_saved / std::max(cycle.reclaim_runtime_seconds, eps);
    cycle.reclaim_area_saved_per_accepted_move =
        cycle.reclaim_area_saved /
        static_cast<double>(std::max(cycle.reclaim_candidates_accepted, 1));
}

void finalize_repair_reclaim_summary_report_fields(RepairReclaimSummary& summary) {
    constexpr double eps = 1e-9;
    summary.total_cycles = static_cast<int>(summary.cycles.size());
    summary.total_pulse_area_increase = 0.0;
    summary.total_reclaim_area_saved = 0.0;
    summary.total_net_cycle_area_delta = 0.0;
    summary.average_reclaim_coverage = 0.0;
    summary.weighted_reclaim_coverage = 0.0;
    summary.total_reclaim_runtime_seconds = 0.0;
    summary.total_reclaim_candidates_built = 0;
    summary.total_reclaim_candidates_tried = 0;
    summary.total_reclaim_candidates_accepted = 0;
    summary.total_reclaim_accept_rate = 0.0;
    summary.total_original_resize_area_saved = 0.0;
    summary.total_newbuf_resize_area_saved = 0.0;
    summary.total_newbuf_delete_area_saved = 0.0;

    for (const RepairReclaimCycleRecord& cycle : summary.cycles) {
        summary.total_pulse_area_increase += cycle.pulse_area_increase;
        summary.total_reclaim_area_saved += cycle.reclaim_area_saved;
        summary.total_net_cycle_area_delta += cycle.net_cycle_area_delta;
        summary.average_reclaim_coverage += cycle.reclaim_coverage;
        summary.total_reclaim_runtime_seconds += cycle.reclaim_runtime_seconds;
        summary.total_reclaim_candidates_built += cycle.reclaim_candidates_built;
        summary.total_reclaim_candidates_tried += cycle.reclaim_candidates_tried;
        summary.total_reclaim_candidates_accepted += cycle.reclaim_candidates_accepted;
        summary.total_original_resize_area_saved += cycle.original_resize_area_saved;
        summary.total_newbuf_resize_area_saved += cycle.newbuf_resize_area_saved;
        summary.total_newbuf_delete_area_saved += cycle.newbuf_delete_area_saved;
    }

    if (!summary.cycles.empty()) {
        summary.average_reclaim_coverage /=
            static_cast<double>(summary.cycles.size());
    }
    summary.weighted_reclaim_coverage =
        summary.total_reclaim_area_saved /
        std::max(summary.total_pulse_area_increase, eps);
    summary.total_reclaim_accept_rate =
        static_cast<double>(summary.total_reclaim_candidates_accepted) /
        static_cast<double>(std::max(summary.total_reclaim_candidates_tried, 1));
}

AdaptiveRepairReclaimParams choose_adaptive_repair_reclaim_params(
    const OptimizerConfig& cfg,
    const BaselineSnapshot& baseline,
    const Phase0Summary& phase0) {
    AdaptiveRepairReclaimParams params;
    params.enabled = cfg.enable_adaptive_repair_reclaim_params;
    params.manual_reclaim_giveback_ratio = cfg.reclaim_giveback_ratio;
    params.manual_phase1a_pulse_delay_scale = cfg.phase1a_pulse_delay_scale;
    params.selected_reclaim_giveback_ratio = cfg.reclaim_giveback_ratio;
    params.selected_phase1a_pulse_delay_scale = cfg.phase1a_pulse_delay_scale;

    if (!cfg.enable_adaptive_repair_reclaim_params) {
        return params;
    }

    params.selected_reclaim_giveback_ratio = cfg.adaptive_reclaim_giveback_ratio;
    params.selected_phase1a_pulse_delay_scale =
        cfg.small_case_phase1a_pulse_delay_scale;
    params.classified_case_size = "small";

    if (!baseline.valid) {
        params.missing_statistics = true;
        return params;
    }

    params.baseline_ss_violations = baseline.timing.ss.violating_paths;
    params.phase0_pressure_candidates =
        std::max<size_t>(phase0.ranked_candidates_available,
                         static_cast<size_t>(std::max(0,
                                                      phase0.positive_pressure_candidates) +
                                             std::max(0,
                                                      phase0.negative_pressure_candidates) +
                                             std::max(0,
                                                      phase0.zero_pressure_candidates)));
    params.phase0_batch_used = phase0.batch_mode_enabled;

    const bool is_large_case =
        params.baseline_ss_violations >=
            static_cast<size_t>(std::max(0,
                                         cfg.large_case_min_ss_violations)) ||
        params.phase0_pressure_candidates >=
            static_cast<size_t>(std::max(0,
                                         cfg.large_case_min_pressure_candidates));

    const bool is_medium_case =
        params.baseline_ss_violations >=
            static_cast<size_t>(std::max(0,
                                         cfg.medium_case_min_ss_violations)) ||
        params.phase0_pressure_candidates >=
            static_cast<size_t>(std::max(0,
                                         cfg.medium_case_min_pressure_candidates)) ||
        (cfg.adaptive_use_phase0_batch_signal && params.phase0_batch_used);

    if (is_large_case) {
        params.classified_case_size = "large";
        params.selected_phase1a_pulse_delay_scale =
            cfg.large_case_phase1a_pulse_delay_scale;
    } else if (is_medium_case) {
        params.classified_case_size = "medium";
        params.selected_phase1a_pulse_delay_scale =
            cfg.medium_case_phase1a_pulse_delay_scale;
    }

    return params;
}

void verify_cached_timing_impl(const std::string& context,
                               const DelayModel& model,
                               const ClockTree& tree,
                               const std::vector<PathInfo>& ss_paths,
                               const std::vector<PathInfo>& ff_paths,
                               double clock_period,
                               const TimingAnalysisResult& cached,
                               bool enabled) {
    if (!enabled) return;
    constexpr double epsilon = 1e-6;
    const TimingAnalysisResult full = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);

    auto differs = [&](double cached_value, double full_value) {
        return !std::isfinite(cached_value) ||
               !std::isfinite(full_value) ||
               std::abs(cached_value - full_value) > epsilon;
    };

    const bool mismatch =
        differs(cached.ss.tns, full.ss.tns) ||
        differs(cached.ss.wns, full.ss.wns) ||
        cached.ss.violating_paths != full.ss.violating_paths ||
        differs(cached.ff.tns, full.ff.tns) ||
        differs(cached.ff.wns, full.ff.wns) ||
        cached.ff.violating_paths != full.ff.violating_paths;

    if (!mismatch) return;

    auto print_corner = [&](const char* corner,
                            const TimingCornerResult& cached_corner,
                            const TimingCornerResult& full_corner) {
        std::cerr << "  " << corner
                  << " cached: TNS=" << cached_corner.tns
                  << " WNS=" << cached_corner.wns
                  << " violations=" << cached_corner.violating_paths << '\n'
                  << "  " << corner
                  << " full:   TNS=" << full_corner.tns
                  << " WNS=" << full_corner.wns
                  << " violations=" << full_corner.violating_paths << '\n'
                  << "  " << corner
                  << " delta:  TNS=" << (cached_corner.tns - full_corner.tns)
                  << " WNS=" << (cached_corner.wns - full_corner.wns)
                  << " violations="
                  << (static_cast<long long>(cached_corner.violating_paths) -
                      static_cast<long long>(full_corner.violating_paths))
                  << '\n';
    };

    std::cerr << std::setprecision(17)
              << "Timing cache consistency failure after " << context
              << " (epsilon=" << epsilon << ")\n";
    print_corner("SS", cached.ss, full.ss);
    print_corner("FF", cached.ff, full.ff);
    std::abort();
}

bool timing_metrics_close(const TimingAnalysisResult& lhs,
                          const TimingAnalysisResult& rhs,
                          double epsilon) {
    return std::abs(lhs.ss.tns - rhs.ss.tns) <= epsilon &&
           std::abs(lhs.ss.wns - rhs.ss.wns) <= epsilon &&
           lhs.ss.violating_paths == rhs.ss.violating_paths &&
           std::abs(lhs.ff.tns - rhs.ff.tns) <= epsilon &&
           std::abs(lhs.ff.wns - rhs.ff.wns) <= epsilon &&
           lhs.ff.violating_paths == rhs.ff.violating_paths;
}

void print_timing_metric_mismatch(const std::string& context,
                                  const TimingAnalysisResult& incremental,
                                  const TimingAnalysisResult& full,
                                  double epsilon) {
    std::cerr << std::setprecision(17)
              << "Phase0 incremental timing mismatch after " << context
              << " (epsilon=" << epsilon << ")\n"
              << "  incremental SS: TNS=" << incremental.ss.tns
              << " WNS=" << incremental.ss.wns
              << " violations=" << incremental.ss.violating_paths << '\n'
              << "  full        SS: TNS=" << full.ss.tns
              << " WNS=" << full.ss.wns
              << " violations=" << full.ss.violating_paths << '\n'
              << "  incremental FF: TNS=" << incremental.ff.tns
              << " WNS=" << incremental.ff.wns
              << " violations=" << incremental.ff.violating_paths << '\n'
              << "  full        FF: TNS=" << full.ff.tns
              << " WNS=" << full.ff.wns
              << " violations=" << full.ff.violating_paths << '\n';
}

std::string timing_path_key(const std::string& launch_ff, const std::string& capture_ff) {
    return launch_ff + "->" + capture_ff;
}

double node_delay_for_corner(const DelayModel& model, const ClockNode* node, bool ss_corner) {
    if (!node || node->is_sink || node->type.empty()) return 0.0;
    return ss_corner ? model.buffer_delay_ss(node->type, node->children.size())
                     : model.buffer_delay_ff(node->type, node->children.size());
}

double compute_node_input_arrival(const DelayModel& model, const ClockNode* node, bool ss_corner) {
    std::vector<const ClockNode*> ancestors;
    for (const ClockNode* cur = node ? node->parent : nullptr; cur; cur = cur->parent) {
        ancestors.push_back(cur);
    }

    double arrival = 0.0;
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        arrival += node_delay_for_corner(model, *it, ss_corner);
    }
    return arrival;
}

void update_subtree_arrivals(const DelayModel& model,
                             const ClockNode* node,
                             bool ss_corner,
                             std::unordered_map<std::string, double>& arrival_by_ff,
                             ArrivalRollback& rollback) {
    const double input_arrival = compute_node_input_arrival(model, node, ss_corner);
    rollback.old_values.clear();

    std::function<void(const ClockNode*, double)> dfs = [&](const ClockNode* cur, double arrival) {
        if (!cur) return;
        if (cur->is_sink) {
            auto it = arrival_by_ff.find(cur->name);
            rollback.old_values.push_back({cur->name, it == arrival_by_ff.end() ? 0.0 : it->second});
            arrival_by_ff[cur->name] = arrival;
            return;
        }

        const double next_arrival = arrival + node_delay_for_corner(model, cur, ss_corner);
        for (const auto& child : cur->children) {
            dfs(child.get(), next_arrival);
        }
    };

    dfs(node, input_arrival);
}

void rollback_arrivals(std::unordered_map<std::string, double>& arrival_by_ff,
                       const ArrivalRollback& rollback) {
    for (auto it = rollback.old_values.rbegin(); it != rollback.old_values.rend(); ++it) {
        arrival_by_ff[it->first] = it->second;
    }
}

double arrival_or_zero(const std::unordered_map<std::string, double>& arrival_by_ff,
                       const std::string& ff_name) {
    auto it = arrival_by_ff.find(ff_name);
    return it == arrival_by_ff.end() ? 0.0 : it->second;
}

TimingPathResult recompute_group_timing(const Phase1bPathGroup& group,
                                        const std::unordered_map<std::string, double>& ss_arrival,
                                        const std::unordered_map<std::string, double>& ff_arrival,
                                        double clock_period,
                                        double t_setup,
                                        double t_hold) {
    TimingPathResult item;
    bool has_setup = false;
    bool has_hold = false;

    for (const PathInfo* path : group.ss_paths) {
        if (!path) continue;
        const double launch_clk = arrival_or_zero(ss_arrival, path->launch_ff);
        const double capture_clk = arrival_or_zero(ss_arrival, path->capture_ff);
        const double skew = capture_clk - launch_clk;
        const double setup_slack = clock_period - t_setup - path->data_delay + skew;

        if (!has_setup) {
            item.path_name = path->name;
            item.launch_ff = path->launch_ff;
            item.capture_ff = path->capture_ff;
            item.data_delay = path->data_delay;
            item.launch_clk_delay = launch_clk;
            item.capture_clk_delay = capture_clk;
            item.skew = skew;
            item.setup_slack = setup_slack;
            has_setup = true;
        } else {
            item.setup_slack = std::min(item.setup_slack, setup_slack);
        }
    }

    for (const PathInfo* path : group.ff_paths) {
        if (!path) continue;
        const double launch_clk = arrival_or_zero(ff_arrival, path->launch_ff);
        const double capture_clk = arrival_or_zero(ff_arrival, path->capture_ff);
        const double skew = capture_clk - launch_clk;
        const double hold_slack = path->data_delay - t_hold - skew;

        if (!has_setup && !has_hold) {
            item.path_name = path->name;
            item.launch_ff = path->launch_ff;
            item.capture_ff = path->capture_ff;
            item.data_delay = path->data_delay;
            item.hold_slack = hold_slack;
            has_hold = true;
        } else {
            item.hold_slack = std::min(item.hold_slack, hold_slack);
            has_hold = true;
        }

        item.launch_clk_delay = launch_clk;
        item.capture_clk_delay = capture_clk;
        item.skew = skew;
        if (item.path_name.empty()) item.path_name = path->name;
    }

    return item;
}

void add_path_to_summary(Phase1bTimingCache& cache, const TimingPathResult& path) {
    if (path.setup_slack < 0.0) {
        cache.timing.ss.tns += path.setup_slack;
        cache.timing.ss.violating_paths += 1;
        cache.ss_violations.insert(path.setup_slack);
    }
    if (path.hold_slack < 0.0) {
        cache.timing.ff.tns += path.hold_slack;
        cache.timing.ff.violating_paths += 1;
        cache.ff_violations.insert(path.hold_slack);
    }
}

void remove_path_from_summary(Phase1bTimingCache& cache, const TimingPathResult& path) {
    if (path.setup_slack < 0.0) {
        cache.timing.ss.tns -= path.setup_slack;
        cache.timing.ss.violating_paths -= 1;
        auto it = cache.ss_violations.find(path.setup_slack);
        if (it != cache.ss_violations.end()) cache.ss_violations.erase(it);
    }
    if (path.hold_slack < 0.0) {
        cache.timing.ff.tns -= path.hold_slack;
        cache.timing.ff.violating_paths -= 1;
        auto it = cache.ff_violations.find(path.hold_slack);
        if (it != cache.ff_violations.end()) cache.ff_violations.erase(it);
    }
}

void refresh_wns(Phase1bTimingCache& cache) {
    cache.timing.ss.wns = cache.ss_violations.empty() ? 0.0 : *cache.ss_violations.begin();
    cache.timing.ff.wns = cache.ff_violations.empty() ? 0.0 : *cache.ff_violations.begin();
}

Phase1bTimingCache build_phase1b_timing_cache(const std::vector<PathInfo>& ss_paths,
                                              const std::vector<PathInfo>& ff_paths,
                                              const std::unordered_map<std::string, double>& ss_arrival,
                                              const std::unordered_map<std::string, double>& ff_arrival,
                                              double clock_period) {
    Phase1bTimingCache cache;
    cache.timing.clock_period = clock_period;
    cache.timing.t_setup = 0.08 * clock_period;
    cache.timing.t_hold = 0.05 * clock_period;

    std::vector<std::pair<std::string, Phase1bPathGroup>> keyed_groups;
    std::unordered_map<std::string, size_t> temp_index;

    auto ensure_group = [&](const PathInfo& path) -> Phase1bPathGroup& {
        const std::string key = timing_path_key(path.launch_ff, path.capture_ff);
        auto it = temp_index.find(key);
        if (it == temp_index.end()) {
            const size_t index = keyed_groups.size();
            temp_index.emplace(key, index);
            keyed_groups.push_back({key, Phase1bPathGroup{}});
            return keyed_groups.back().second;
        }
        return keyed_groups[it->second].second;
    };

    for (const auto& path : ss_paths) {
        ensure_group(path).ss_paths.push_back(&path);
    }
    for (const auto& path : ff_paths) {
        ensure_group(path).ff_paths.push_back(&path);
    }

    std::vector<std::pair<std::string, TimingPathResult>> keyed_results;
    keyed_results.reserve(keyed_groups.size());
    for (const auto& kv : keyed_groups) {
        keyed_results.push_back({kv.first, recompute_group_timing(kv.second,
                                                                  ss_arrival,
                                                                  ff_arrival,
                                                                  clock_period,
                                                                  cache.timing.t_setup,
                                                                  cache.timing.t_hold)});
    }

    std::sort(keyed_results.begin(), keyed_results.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second.path_name < rhs.second.path_name;
    });

    cache.groups.reserve(keyed_results.size());
    cache.timing.path_results.reserve(keyed_results.size());
    for (const auto& keyed_result : keyed_results) {
        const std::string& key = keyed_result.first;
        auto group_it = temp_index.find(key);
        if (group_it == temp_index.end()) continue;

        const size_t index = cache.groups.size();
        cache.groups.push_back(std::move(keyed_groups[group_it->second].second));
        cache.timing.path_results.push_back(keyed_result.second);
        add_path_to_summary(cache, keyed_result.second);

        const TimingPathResult& result = cache.timing.path_results.back();
        cache.groups_by_ff[result.launch_ff].push_back(index);
        if (result.capture_ff != result.launch_ff) {
            cache.groups_by_ff[result.capture_ff].push_back(index);
        }
    }

    refresh_wns(cache);
    return cache;
}

std::vector<size_t> affected_path_groups(const Phase1bTimingCache& cache,
                                         const ArrivalRollback& arrival_rollback) {
    std::vector<size_t> indices;
    indices.reserve(arrival_rollback.old_values.size() * 2);
    std::unordered_set<size_t> seen;
    seen.reserve(arrival_rollback.old_values.size() * 2);

    for (const auto& old_value : arrival_rollback.old_values) {
        auto it = cache.groups_by_ff.find(old_value.first);
        if (it == cache.groups_by_ff.end()) continue;
        for (size_t index : it->second) {
            if (seen.insert(index).second) {
                indices.push_back(index);
            }
        }
    }

    return indices;
}

PathTimingRollback update_affected_path_groups(Phase1bTimingCache& cache,
                                               const std::vector<size_t>& indices,
                                               const std::unordered_map<std::string, double>& ss_arrival,
                                               const std::unordered_map<std::string, double>& ff_arrival) {
    PathTimingRollback rollback;
    rollback.old_results.reserve(indices.size());

    for (size_t index : indices) {
        if (index >= cache.groups.size() || index >= cache.timing.path_results.size()) continue;
        TimingPathResult old_result = cache.timing.path_results[index];
        rollback.old_results.push_back({index, old_result});

        remove_path_from_summary(cache, old_result);
        TimingPathResult new_result = recompute_group_timing(cache.groups[index],
                                                             ss_arrival,
                                                             ff_arrival,
                                                             cache.timing.clock_period,
                                                             cache.timing.t_setup,
                                                             cache.timing.t_hold);
        cache.timing.path_results[index] = new_result;
        add_path_to_summary(cache, new_result);
    }

    refresh_wns(cache);
    return rollback;
}

ArrivalRollback combine_arrival_rollbacks(const ArrivalRollback& lhs,
                                          const ArrivalRollback& rhs) {
    ArrivalRollback combined;
    combined.old_values.reserve(lhs.old_values.size() + rhs.old_values.size());
    combined.old_values.insert(combined.old_values.end(),
                               lhs.old_values.begin(),
                               lhs.old_values.end());
    combined.old_values.insert(combined.old_values.end(),
                               rhs.old_values.begin(),
                               rhs.old_values.end());
    return combined;
}

void rollback_path_groups(Phase1bTimingCache& cache, const PathTimingRollback& rollback) {
    for (auto it = rollback.old_results.rbegin(); it != rollback.old_results.rend(); ++it) {
        const size_t index = it->first;
        if (index >= cache.timing.path_results.size()) continue;
        remove_path_from_summary(cache, cache.timing.path_results[index]);
        cache.timing.path_results[index] = it->second;
        add_path_to_summary(cache, it->second);
    }
    refresh_wns(cache);
}

bool remove_buffer_node(ClockTree& tree, const std::string& node_name, RemovedBufferState& state) {
    ClockNode* node = tree.find_node(node_name);
    if (!node || !node->parent || node->is_sink || node->original) return false;

    ClockNode* parent = node->parent;
    size_t node_index = 0;
    bool found = false;
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i].get() == node) {
            node_index = i;
            found = true;
            break;
        }
    }
    if (!found) return false;

    state.name = node->name;
    state.type = node->type;
    state.parent_name = parent->name;
    state.parent_index = node_index;
    state.original = node->original;
    state.child_names.clear();
    for (const auto& child : node->children) {
        if (child) state.child_names.push_back(child->name);
    }

    const std::string removed_name = state.name;

    std::vector<std::unique_ptr<ClockNode>> moved_children;
    moved_children.reserve(node->children.size());
    for (auto& child : node->children) {
        if (!child) continue;
        child->parent = parent;
        moved_children.push_back(std::move(child));
    }
    node->children.clear();

    parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(node_index));
    if (!moved_children.empty()) {
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(node_index),
                                std::make_move_iterator(moved_children.begin()),
                                std::make_move_iterator(moved_children.end()));
    }

    tree.remove_node_from_index(removed_name);
    return true;
}

bool restore_buffer_node(ClockTree& tree, const RemovedBufferState& state) {
    ClockNode* parent = tree.find_node(state.parent_name);
    if (!parent) return false;

    const size_t expected_matches = static_cast<size_t>(std::count_if(parent->children.begin(), parent->children.end(), [&](const std::unique_ptr<ClockNode>& child) {
        return child && std::find(state.child_names.begin(), state.child_names.end(), child->name) != state.child_names.end();
    }));
    if (expected_matches != state.child_names.size()) {
        return false;
    }

    std::vector<std::unique_ptr<ClockNode>> moved_children;
    std::vector<std::unique_ptr<ClockNode>> kept_children;
    moved_children.reserve(state.child_names.size());
    kept_children.reserve(parent->children.size());

    for (auto& child : parent->children) {
        if (child && std::find(state.child_names.begin(), state.child_names.end(), child->name) != state.child_names.end()) {
            moved_children.push_back(std::move(child));
        } else {
            kept_children.push_back(std::move(child));
        }
    }
    parent->children = std::move(kept_children);

    auto buffer = std::make_unique<ClockNode>();
    buffer->name = state.name;
    buffer->type = state.type;
    buffer->buffer_type_id = tree.lookup_buffer_type_id(state.type);
    buffer->is_sink = false;
    buffer->original = state.original;
    buffer->parent = parent;

    for (auto& child : moved_children) {
        if (!child) continue;
        child->parent = buffer.get();
        buffer->children.push_back(std::move(child));
    }

    size_t insert_index = std::min(state.parent_index, parent->children.size());
    ClockNode* buffer_ptr = buffer.get();
    parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(buffer));
    tree.add_node_to_index(buffer_ptr);
    return true;
}

} // namespace

Phase0Summary Optimizer::run_phase0_timing_conditioning(
    ClockTree& tree,
    const std::vector<BufSpec>& libs,
    const std::vector<PathInfo>& ss_paths,
    const std::vector<PathInfo>& ff_paths,
    double clock_period,
    bool reset_based,
    const std::string& branch_name,
    const std::chrono::steady_clock::time_point& global_start) {
    Phase0Summary summary;
    summary.enabled = true;
    summary.reset_based = reset_based;
    summary.branch_name = branch_name;
    summary.wns_weight = cfg.phase0_wns_weight;
    summary.area_tiebreak_penalty = cfg.phase0_area_tiebreak_penalty;
    summary.time_budget_seconds = cfg.phase0_time_budget_seconds;
    summary.max_trial_nodes = cfg.phase0_max_trial_nodes;
    summary.max_types_per_node = cfg.phase0_max_types_per_node;
    summary.max_passes = cfg.phase0_max_passes;
    summary.max_consecutive_rejects = cfg.phase0_max_consecutive_rejects;
    summary.max_consecutive_failed_batches = cfg.phase0_max_consecutive_failed_batches;
    summary.unlimited_by_count = cfg.phase0_max_trial_nodes <= 0;
    summary.node_fraction = cfg.phase0_node_fraction;
    summary.incremental_timing_enabled = cfg.enable_phase0_incremental_timing;
    summary.incremental_verify_enabled = cfg.enable_phase0_incremental_verify;
    summary.batch_manual_enabled = cfg.enable_phase0_batch_trial;
    summary.batch_auto_enabled = cfg.enable_phase0_batch_auto;
    summary.batch_size = cfg.phase0_batch_size;
    summary.fast_batch_timing_enabled = cfg.enable_phase0_fast_batch_timing;
    summary.batch_split_on_fail = cfg.phase0_batch_split_on_fail;
    summary.batch_max_split_depth = cfg.phase0_batch_max_split_depth;
    summary.batch_auto_min_ss_violations = cfg.phase0_batch_auto_min_ss_violations;
    summary.batch_auto_min_pressure_candidates = cfg.phase0_batch_auto_min_pressure_candidates;

    if (!tree.root || libs.empty()) return summary;

    DelayModel model(libs);
    LibraryCache lib_cache = build_library_cache(libs);
    std::unordered_map<std::string, const BufSpec*> lib_by_name;
    lib_by_name.reserve(libs.size());
    for (const auto& lib : libs) {
        lib_by_name.emplace(lib.name, &lib);
    }
    auto cached_lib = [&](const std::string& name) -> const BufSpec* {
        if (cfg.enable_type_id_cache) {
            const BufSpec* lib = lib_cache.get(name);
            if (!lib) {
                ++summary.library_lookup_cache_misses;
                return nullptr;
            }
            ++summary.library_lookup_cache_hits;
            return lib;
        }
        auto it = lib_by_name.find(name);
        if (it == lib_by_name.end()) {
            ++summary.library_lookup_cache_misses;
            return nullptr;
        }
        ++summary.library_lookup_cache_hits;
        return it->second;
    };
    auto phase0_local_resize_ok = [&](const ClockNode* node,
                                      const std::string& new_type) {
        ++summary.local_legality_checks_phase0;
        return local_resize_legality(node, cached_lib(new_type));
    };
    auto verify_phase0_full_validation = [&](const std::string& context) {
        if (!cfg.enable_phase0_trial_full_validation_verify) return;
        ++summary.full_legality_validations_phase0_trials;
        const LegalityReport legality = model.validate_legality(tree, libs);
        if (!legality.ok) {
            std::cerr << "Phase0 full validation failed after " << context << '\n';
            for (const auto& issue : legality.issues) {
                std::cerr << "  - " << issue << '\n';
            }
            std::abort();
        }
    };
    auto verify_phase0_area = [&](const std::string& context,
                                  double tracked_area) {
        if (!cfg.enable_incremental_area_verify) return;
        ++summary.full_area_recomputations;
        const double full_area = model.compute_tree_area(tree);
        if (std::abs(full_area - tracked_area) > 1e-6) {
            std::cerr << "Phase0 area mismatch after " << context
                      << ": tracked=" << tracked_area
                      << " full=" << full_area << '\n';
            std::abort();
        }
    };
    const auto phase0_start = std::chrono::steady_clock::now();
    auto elapsed_seconds = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - phase0_start).count();
    };
    auto global_elapsed_seconds = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - global_start).count();
    };
    auto global_time_up = [&]() {
        return global_elapsed_seconds() >= cfg.time_limit_seconds;
    };
    auto time_up = [&]() {
        return elapsed_seconds() >= cfg.phase0_time_budget_seconds || global_time_up();
    };
    auto capture = [&](StageSnapshot& snapshot,
                       const std::string& name,
                       int resizes) {
        snapshot.valid = true;
        snapshot.name = name;
        snapshot.downsized_buffers = resizes;
        snapshot.runtime_seconds = elapsed_seconds();
        snapshot.timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        ++summary.full_area_recomputations;
        snapshot.area = model.compute_tree_area(tree);
        snapshot.legality = model.validate_legality(tree, libs);
    };

    capture(summary.before_phase0, "Baseline", 0);
    baseline.timing = summary.before_phase0.timing;
    summary.before_phase0.timing =
        compact_timing_snapshot(summary.before_phase0.timing);
    baseline.area = summary.before_phase0.area;
    baseline.valid = true;

    if (reset_based) {
        std::vector<ClockNode*> buffer_nodes = collect_buffer_nodes(tree);
        size_t maximum_fanout = 0;
        for (const ClockNode* node : buffer_nodes) {
            maximum_fanout = std::max(maximum_fanout, node->children.size());
        }
        const BufSpec* fastest = choose_fastest_compatible_buffer(libs, model, maximum_fanout);
        if (fastest) {
            summary.fastest_buffer_type = fastest->name;
            for (ClockNode* node : buffer_nodes) {
                if (node->type == fastest->name) continue;
                if (tree.set_buffer_type(node->name, fastest->name)) {
                    ++summary.reset_resizes;
                }
            }
        }
        capture(summary.after_reset, "After Reset Experiment", summary.reset_resizes);
        summary.after_reset.timing =
            compact_timing_snapshot(summary.after_reset.timing);
    }

    TimingAnalysisResult current_timing =
        model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
    auto phase0_ss_arrival = model.compute_clock_arrivals(tree, true);
    auto phase0_ff_arrival = model.compute_clock_arrivals(tree, false);
    Phase1bTimingCache phase0_timing_cache = build_phase1b_timing_cache(ss_paths,
                                                                        ff_paths,
                                                                        phase0_ss_arrival,
                                                                        phase0_ff_arrival,
                                                                        clock_period);
    if (cfg.enable_phase0_incremental_timing) {
        current_timing = phase0_timing_cache.timing;
    }
    ++summary.full_area_recomputations;
    double current_area = model.compute_tree_area(tree);
    std::unique_ptr<FastTimingEngine> phase0_fast_timing;
    if (cfg.enable_fast_timing_engine) {
        phase0_fast_timing = std::make_unique<FastTimingEngine>(libs,
                                                                ss_paths,
                                                                ff_paths,
                                                                clock_period);
        if (phase0_fast_timing->sync_from_tree(tree)) {
            if (cfg.enable_fast_timing_verify &&
                !phase0_fast_timing->verify_against_delay_model(tree,
                                                                model,
                                                                ss_paths,
                                                                ff_paths,
                                                                clock_period,
                                                                "Phase0 sync",
                                                                1e-6,
                                                                &std::cerr)) {
                std::abort();
            }
            current_timing = phase0_fast_timing->timing();
            current_area = phase0_fast_timing->area();
        } else {
            phase0_fast_timing.reset();
        }
    }
    double current_cost = normalized_phase0_timing_cost(current_timing,
                                                        baseline.timing,
                                                        cfg.phase0_wns_weight);
    double current_score = weighted_objective_score(model,
                                                    current_timing,
                                                    current_area,
                                                    baseline,
                                                    cfg);
    summary.original_timing_cost =
        normalized_phase0_timing_cost(baseline.timing, baseline.timing, cfg.phase0_wns_weight);
    summary.area_before_phase0 = current_area;
    const size_t baseline_ss_violations = baseline.timing.ss.violating_paths;
    bool use_phase0_batch_mode = false;

    std::vector<const BufSpec*> ordered_libs;
    ordered_libs.reserve(libs.size());
    for (const auto& lib : libs) ordered_libs.push_back(&lib);
    std::sort(ordered_libs.begin(), ordered_libs.end(), [](const BufSpec* lhs, const BufSpec* rhs) {
        const double lhs_delay = average_library_delay(*lhs);
        const double rhs_delay = average_library_delay(*rhs);
        if (lhs_delay != rhs_delay) return lhs_delay < rhs_delay;
        return lhs->name < rhs->name;
    });
    std::unordered_map<std::string, int> type_rank;
    for (size_t i = 0; i < ordered_libs.size(); ++i) {
        type_rank[ordered_libs[i]->name] = static_cast<int>(i);
    }
    const TreeIndexCache phase0_tree_index = build_tree_index_cache(tree);

    const double pressure_eps = cfg.phase0_pressure_epsilon;
    const double timing_improvement_eps = cfg.phase0_timing_improvement_epsilon;
    const double wns_tolerance = cfg.phase0_wns_tolerance;
    const double tns_tolerance = cfg.phase0_tns_tolerance;
    const double area_tolerance = cfg.area_comparison_epsilon;
    constexpr double phase0_verify_eps = 1e-6;
    constexpr double phase0_ranking_norm_eps = 1e-9;
    const bool phase0_ss_ranking_active =
        std::abs(baseline.timing.ss.tns) > phase0_ranking_norm_eps ||
        std::abs(baseline.timing.ss.wns) > phase0_ranking_norm_eps;
    const bool phase0_ff_ranking_active =
        std::abs(baseline.timing.ff.tns) > phase0_ranking_norm_eps ||
        std::abs(baseline.timing.ff.wns) > phase0_ranking_norm_eps;
    const double phase0_ss_delay_norm =
        std::max(std::abs(baseline.timing.ss.wns),
                 phase0_ranking_norm_eps);
    const double phase0_ff_delay_norm =
        std::max(std::abs(baseline.timing.ff.wns),
                 phase0_ranking_norm_eps);
    const double phase0_area_norm =
        std::max(std::abs(baseline.area), phase0_ranking_norm_eps);

    auto phase0_estimated_ranking_value =
        [&](double pressure,
            double ss_delay_delta,
            double ff_delay_delta,
            double area_delta) {
            if (!cfg.enable_phase0_score_aligned_ranking) {
                const double average_delta =
                    0.5 * (ss_delay_delta + ff_delay_delta);
                return std::abs(pressure) * std::abs(average_delta) -
                       cfg.phase0_area_tiebreak_penalty *
                           std::max(0.0, area_delta);
            }

            const double estimated_ss_gain =
                phase0_ss_ranking_active
                    ? std::abs(pressure) * std::abs(ss_delay_delta) /
                          phase0_ss_delay_norm
                    : 0.0;
            const double estimated_ff_gain =
                phase0_ff_ranking_active
                    ? std::abs(pressure) * std::abs(ff_delay_delta) /
                          phase0_ff_delay_norm
                    : 0.0;
            const double estimated_area_gain =
                -area_delta / phase0_area_norm;
            return cfg.alpha * estimated_ss_gain +
                   cfg.beta * estimated_ff_gain +
                   cfg.gamma * estimated_area_gain;
        };

    summary.node_ranking_method =
        cfg.enable_phase0_score_aligned_ranking
            ? "weighted_score_estimated_value"
            : "best_type_estimated_value";

    auto phase0_pareto_acceptable =
        [&](const TimingAnalysisResult& trial_timing,
            double trial_area) {
            const bool all_metrics_not_worse =
                trial_timing.ss.wns >=
                    current_timing.ss.wns - wns_tolerance &&
                trial_timing.ss.tns >=
                    current_timing.ss.tns - tns_tolerance &&
                trial_timing.ff.wns >=
                    current_timing.ff.wns - wns_tolerance &&
                trial_timing.ff.tns >=
                    current_timing.ff.tns - tns_tolerance &&
                trial_area <= current_area + area_tolerance;
            const bool at_least_one_metric_better =
                trial_timing.ss.wns >
                    current_timing.ss.wns + wns_tolerance ||
                trial_timing.ss.tns >
                    current_timing.ss.tns + tns_tolerance ||
                trial_timing.ff.wns >
                    current_timing.ff.wns + wns_tolerance ||
                trial_timing.ff.tns >
                    current_timing.ff.tns + tns_tolerance ||
                trial_area < current_area - area_tolerance;
            return all_metrics_not_worse && at_least_one_metric_better;
        };

    struct Phase0IncrementalTrial {
        bool applied = false;
        std::string old_type;
        TimingAnalysisResult previous_timing;
        ArrivalRollback ss_rollback;
        ArrivalRollback ff_rollback;
        PathTimingRollback path_rollback;
    };

    auto apply_incremental_type_change = [&](ClockNode* target,
                                             const std::string& new_type,
                                             Phase0IncrementalTrial& trial) -> bool {
        if (!is_buffer_node(target)) return false;

        trial.old_type = target->type;
        trial.previous_timing = phase0_timing_cache.timing;
        if (!tree.set_buffer_type(target->name, new_type)) return false;

        update_subtree_arrivals(model, target, true, phase0_ss_arrival, trial.ss_rollback);
        update_subtree_arrivals(model, target, false, phase0_ff_arrival, trial.ff_rollback);
        const ArrivalRollback affected = combine_arrival_rollbacks(trial.ss_rollback,
                                                                   trial.ff_rollback);
        const std::vector<size_t> affected_groups = affected_path_groups(phase0_timing_cache,
                                                                         affected);
        trial.path_rollback = update_affected_path_groups(phase0_timing_cache,
                                                          affected_groups,
                                                          phase0_ss_arrival,
                                                          phase0_ff_arrival);
        trial.applied = true;
        return true;
    };

    auto rollback_incremental_type_change = [&](ClockNode* target,
                                                const Phase0IncrementalTrial& trial) {
        if (!trial.applied) return;
        rollback_path_groups(phase0_timing_cache, trial.path_rollback);
        rollback_arrivals(phase0_ss_arrival, trial.ss_rollback);
        rollback_arrivals(phase0_ff_arrival, trial.ff_rollback);
        if (target) {
            tree.set_buffer_type(target->name, trial.old_type);
        }
        phase0_timing_cache.timing = trial.previous_timing;
    };

    auto verify_phase0_incremental_state = [&](const std::string& context) {
        if (!cfg.enable_phase0_incremental_verify) return;
        const TimingAnalysisResult full_timing =
            model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        if (!timing_metrics_close(phase0_timing_cache.timing,
                                  full_timing,
                                  phase0_verify_eps)) {
            ++summary.incremental_verify_failures;
            print_timing_metric_mismatch(context,
                                         phase0_timing_cache.timing,
                                         full_timing,
                                         phase0_verify_eps);
            std::abort();
        }
    };

    auto set_phase0_stop_reason = [&](const std::string& reason) {
        summary.early_stop_reason = reason;
        if (reason == "consecutive_rejects" ||
            reason == "consecutive_failed_batches" ||
            reason == "time_budget" ||
            reason == "global_time_budget") {
            summary.early_stop_triggered = true;
        }
    };

    auto batch_is_independent = [&](const std::vector<Phase0BatchCandidate>& batch,
                                    const std::string& node_name) {
        const auto node_it =
            phase0_tree_index.node_id_by_name.find(node_name);
        if (node_it == phase0_tree_index.node_id_by_name.end()) return false;
        const int node_id = node_it->second;
        auto is_ancestor_id = [&](int maybe_ancestor, int node) {
            return maybe_ancestor >= 0 &&
                   node >= 0 &&
                   static_cast<size_t>(maybe_ancestor) <
                       phase0_tree_index.subtree_end.size() &&
                   maybe_ancestor < node &&
                   node <
                       phase0_tree_index
                           .subtree_end[static_cast<size_t>(maybe_ancestor)];
        };
        for (const Phase0BatchCandidate& existing : batch) {
            if (existing.node_name == node_name) return false;
            const auto other_it =
                phase0_tree_index.node_id_by_name.find(existing.node_name);
            if (other_it == phase0_tree_index.node_id_by_name.end()) return false;
            const int other_id = other_it->second;
            if (is_ancestor_id(node_id, other_id) ||
                is_ancestor_id(other_id, node_id)) {
                return false;
            }
        }
        return true;
    };

    FastTimingEngine::Transaction phase0_fast_txn;
    std::function<bool(const std::vector<Phase0BatchCandidate>&, int)>
        try_phase0_batch;
    try_phase0_batch =
        [&](const std::vector<Phase0BatchCandidate>& batch,
            int split_depth) -> bool {
        if (batch.empty() || time_up()) return false;

        ++summary.batch_attempts;
        summary.attempted_resizes += static_cast<int>(batch.size());

        struct SavedType {
            std::string node_name;
            std::string old_type;
        };

        std::vector<SavedType> saved_types;
        saved_types.reserve(batch.size());
        for (const Phase0BatchCandidate& candidate : batch) {
            ClockNode* node = tree.find_node(candidate.node_name);
            if (!phase0_local_resize_ok(node, candidate.new_type)) {
                ++summary.batch_rejected;
                return false;
            }
        }

        auto split_rejected_batch = [&]() -> bool {
            if (cfg.phase0_batch_split_on_fail &&
                batch.size() > 1 &&
                split_depth < cfg.phase0_batch_max_split_depth) {
                ++summary.batch_split_count;
                const size_t mid = batch.size() / 2;
                const bool accepted_left =
                    try_phase0_batch(std::vector<Phase0BatchCandidate>(
                                         batch.begin(),
                                         batch.begin() +
                                             static_cast<std::ptrdiff_t>(mid)),
                                     split_depth + 1);
                const bool accepted_right =
                    !time_up() &&
                    try_phase0_batch(std::vector<Phase0BatchCandidate>(
                                         batch.begin() +
                                             static_cast<std::ptrdiff_t>(mid),
                                         batch.end()),
                                     split_depth + 1);
                return accepted_left || accepted_right;
            }
            if (cfg.phase0_batch_split_on_fail &&
                batch.size() > 1 &&
                split_depth >= cfg.phase0_batch_max_split_depth) {
                ++summary.batch_split_depth_limit_hits;
            }
            return false;
        };

        auto record_accepted_batch =
            [&](const TimingAnalysisResult& trial_timing,
                double trial_area,
                double trial_cost,
                double trial_score,
                bool used_fast_batch) {
                const double previous_cost = current_cost;
                const double previous_area = current_area;
                ++summary.batch_accepted;
                summary.batch_accepted_candidates +=
                    static_cast<int>(batch.size());
                summary.accepted_resizes += static_cast<int>(batch.size());
                current_timing = trial_timing;
                current_cost = trial_cost;
                current_score = trial_score;
                current_area = trial_area;
                ++summary.incremental_area_updates;

                for (const Phase0BatchCandidate& candidate : batch) {
                    Phase0MoveRecord record;
                    record.node_name = candidate.node_name;
                    record.old_type = candidate.old_type;
                    record.new_type = candidate.new_type;
                    record.pressure = candidate.pressure;
                    record.old_timing_cost = previous_cost;
                    record.new_timing_cost = trial_cost;
                    record.old_area = previous_area;
                    record.new_area = current_area;
                    record.area_delta = candidate.area_delta;
                    summary.move_records.push_back(record);

                    std::ostringstream move;
                    move << "Phase0 batch resize " << record.node_name
                         << " " << record.old_type << " -> "
                         << record.new_type
                         << " pressure=" << record.pressure
                         << " area_delta=" << record.area_delta
                         << (used_fast_batch ? " [fast_batch]" : "");
                    summary.applied_moves.push_back(move.str());
                }

                if (!used_fast_batch) {
                    verify_phase0_incremental_state(
                        "accepted Phase0 batch");
                }
                verify_phase0_full_validation("accepted Phase0 batch");
                verify_phase0_area("accepted Phase0 batch", current_area);
                return true;
            };

        if (cfg.enable_phase0_fast_batch_timing && phase0_fast_timing) {
            summary.fast_batch_timing_used = true;
            std::vector<FastTimingResizeRequest> requests;
            requests.reserve(batch.size());
            for (const Phase0BatchCandidate& candidate : batch) {
                requests.push_back(
                    {candidate.node_name, candidate.new_type});
            }

            const auto trial_start = std::chrono::steady_clock::now();
            FastTimingTrialSummary trial =
                phase0_fast_timing->trial_resize_batch(tree,
                                                       requests,
                                                       phase0_fast_txn);
            const double trial_cost =
                trial.ok
                    ? normalized_phase0_timing_cost(trial.timing,
                                                    baseline.timing,
                                                    cfg.phase0_wns_weight)
                    : std::numeric_limits<double>::infinity();
            const double trial_score =
                trial.ok
                    ? weighted_objective_score(model,
                                               trial.timing,
                                               trial.area,
                                               baseline,
                                               cfg)
                    : -std::numeric_limits<double>::infinity();
            const bool accepted =
                trial.ok &&
                (cfg.enable_phase0_score_based_acceptance
                     ? score_strictly_better(trial_score,
                                             current_score,
                                             cfg)
                     : phase0_pareto_acceptable(trial.timing,
                                                trial.area));
            summary.total_trial_time_seconds +=
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - trial_start)
                    .count();

            if (accepted &&
                phase0_fast_timing->commit(tree, phase0_fast_txn)) {
                if (cfg.enable_fast_timing_verify) {
                    const int interval = cfg.fast_timing_verify_interval;
                    const bool verify_now =
                        interval <= 0 ||
                        phase0_fast_timing->counters().commit_count %
                                interval ==
                            0;
                    if (verify_now &&
                        !phase0_fast_timing->verify_against_delay_model(
                            tree,
                            model,
                            ss_paths,
                            ff_paths,
                            clock_period,
                            "accepted Phase0 fast batch",
                            phase0_verify_eps,
                            &std::cerr)) {
                        std::abort();
                    }
                }
                return record_accepted_batch(trial.timing,
                                             trial.area,
                                             trial_cost,
                                             trial_score,
                                             true);
            }

            phase0_fast_timing->rollback(phase0_fast_txn);
            ++summary.batch_rejected;
            return split_rejected_batch();
        }

        TimingAnalysisResult previous_timing = phase0_timing_cache.timing;
        std::vector<ArrivalRollback> ss_rollbacks;
        std::vector<ArrivalRollback> ff_rollbacks;
        ss_rollbacks.reserve(batch.size());
        ff_rollbacks.reserve(batch.size());
        bool applied_all = true;

        const auto trial_start = std::chrono::steady_clock::now();
        for (const Phase0BatchCandidate& candidate : batch) {
            ClockNode* node = tree.find_node(candidate.node_name);
            if (!is_buffer_node(node)) {
                applied_all = false;
                break;
            }
            saved_types.push_back({candidate.node_name, node->type});
            if (!tree.set_buffer_type(candidate.node_name, candidate.new_type)) {
                applied_all = false;
                break;
            }

            ss_rollbacks.emplace_back();
            ff_rollbacks.emplace_back();
            update_subtree_arrivals(model, node, true, phase0_ss_arrival, ss_rollbacks.back());
            update_subtree_arrivals(model, node, false, phase0_ff_arrival, ff_rollbacks.back());
        }

        ArrivalRollback affected;
        for (const ArrivalRollback& rollback : ss_rollbacks) {
            affected.old_values.insert(affected.old_values.end(),
                                       rollback.old_values.begin(),
                                       rollback.old_values.end());
        }
        for (const ArrivalRollback& rollback : ff_rollbacks) {
            affected.old_values.insert(affected.old_values.end(),
                                       rollback.old_values.begin(),
                                       rollback.old_values.end());
        }

        PathTimingRollback path_rollback;
        if (applied_all) {
            const std::vector<size_t> affected_groups = affected_path_groups(phase0_timing_cache,
                                                                             affected);
            path_rollback = update_affected_path_groups(phase0_timing_cache,
                                                        affected_groups,
                                                        phase0_ss_arrival,
                                                        phase0_ff_arrival);
        }

        const TimingAnalysisResult trial_timing = phase0_timing_cache.timing;
        double area_delta = 0.0;
        for (const Phase0BatchCandidate& candidate : batch) {
            area_delta += candidate.area_delta;
        }
        const double trial_area = current_area + area_delta;
        const double trial_cost =
            applied_all
                ? normalized_phase0_timing_cost(trial_timing,
                                                baseline.timing,
                                                cfg.phase0_wns_weight)
                : std::numeric_limits<double>::infinity();
        const double trial_score =
            applied_all
                ? weighted_objective_score(model,
                                           trial_timing,
                                           trial_area,
                                           baseline,
                                           cfg)
                : -std::numeric_limits<double>::infinity();
        const bool accepted =
            applied_all &&
            (cfg.enable_phase0_score_based_acceptance
                 ? score_strictly_better(trial_score, current_score, cfg)
                 : phase0_pareto_acceptable(trial_timing,
                                            trial_area));

        summary.total_trial_time_seconds +=
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - trial_start).count();

        if (accepted) {
            return record_accepted_batch(trial_timing,
                                         trial_area,
                                         trial_cost,
                                         trial_score,
                                         false);
        }

        ++summary.batch_rejected;
        rollback_path_groups(phase0_timing_cache, path_rollback);
        for (auto it = ss_rollbacks.rbegin(); it != ss_rollbacks.rend(); ++it) {
            rollback_arrivals(phase0_ss_arrival, *it);
        }
        for (auto it = ff_rollbacks.rbegin(); it != ff_rollbacks.rend(); ++it) {
            rollback_arrivals(phase0_ff_arrival, *it);
        }
        for (auto it = saved_types.rbegin(); it != saved_types.rend(); ++it) {
            tree.set_buffer_type(it->node_name, it->old_type);
        }
        phase0_timing_cache.timing = previous_timing;

        return split_rejected_batch();
    };

    for (int pass = 0; pass < cfg.phase0_max_passes && !time_up(); ++pass) {
        std::vector<ClockNode*> buffer_nodes = collect_buffer_nodes(tree);
        const auto pressure_start = std::chrono::steady_clock::now();
        const TimingAnalysisResult pressure_timing =
            current_timing.path_results.empty()
                ? model.analyze_timing(tree, ss_paths, ff_paths, clock_period)
                : current_timing;
        std::unordered_map<std::string, double> pressure_by_node;
        if (cfg.enable_phase0_endpoint_delta_pressure) {
            summary.pressure_method = "indexed_endpoint_delta";
            TreeIndexCache tree_index = build_tree_index_cache(tree);
            const double ss_weight_scale =
                1.0 / std::max(std::abs(baseline.timing.ss.wns), 1e-9);
            const double ff_weight_scale =
                1.0 / std::max(std::abs(baseline.timing.ff.wns), 1e-9);
            bool pressure_timed_out = false;
            pressure_by_node =
                compute_endpoint_delta_pressure_indexed(tree_index,
                                                        pressure_timing,
                                                        ss_weight_scale,
                                                        ff_weight_scale,
                                                        time_up,
                                                        &pressure_timed_out);
            if (pressure_timed_out) {
                set_phase0_stop_reason(global_time_up()
                                           ? "global_time_budget"
                                           : "time_budget");
                break;
            }
            if (cfg.enable_phase0_pressure_verify) {
                const auto reference =
                    compute_phase0_pressure(tree,
                                            pressure_timing,
                                            baseline.timing);
                double max_abs_diff = 0.0;
                for (const ClockNode* node : buffer_nodes) {
                    if (!node) continue;
                    const auto reference_it = reference.find(node->name);
                    const double reference_value =
                        reference_it == reference.end()
                            ? 0.0
                            : reference_it->second;
                    const auto it = pressure_by_node.find(node->name);
                    const double indexed_value =
                        it == pressure_by_node.end() ? 0.0 : it->second;
                    max_abs_diff =
                        std::max(max_abs_diff,
                                 std::abs(indexed_value - reference_value));
                }
                summary.pressure_verify_max_abs_diff =
                    std::max(summary.pressure_verify_max_abs_diff, max_abs_diff);
                if (max_abs_diff > 1e-6) {
                    std::cerr << "Phase0 endpoint pressure mismatch in pass "
                              << pass << ": max_abs_diff=" << max_abs_diff << '\n';
                    std::abort();
                }
            }
        } else {
            summary.pressure_method = "old_lca";
            bool pressure_timed_out = false;
            pressure_by_node =
                compute_phase0_pressure(tree,
                                        pressure_timing,
                                        baseline.timing,
                                        time_up,
                                        &pressure_timed_out);
            if (pressure_timed_out) {
                set_phase0_stop_reason(global_time_up()
                                           ? "global_time_budget"
                                           : "time_budget");
                break;
            }
        }
        summary.pressure_time_seconds +=
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - pressure_start).count();
        std::vector<Phase0NodeCandidate> node_candidates;
        node_candidates.reserve(buffer_nodes.size());

        int positive_count = 0;
        int negative_count = 0;
        int zero_count = 0;
        bool phase0_ranking_timed_out = false;
        size_t phase0_ranked_nodes = 0;
        for (const ClockNode* node : buffer_nodes) {
            if ((phase0_ranked_nodes++ & 255U) == 0U && time_up()) {
                phase0_ranking_timed_out = true;
                break;
            }
            const auto pressure_it = pressure_by_node.find(node->name);
            const double pressure = pressure_it == pressure_by_node.end() ? 0.0 : pressure_it->second;
            auto estimate_best_type_value = [&]() {
                if (!is_buffer_node(node)) {
                    return std::numeric_limits<double>::lowest();
                }
                const BufSpec* current_lib = cached_lib(node->type);
                if (!current_lib) {
                    return std::numeric_limits<double>::lowest();
                }
                const size_t fanout = node->children.size();
                if (fanout == 0 ||
                    current_lib->ss_delay.size() < fanout ||
                    current_lib->ff_delay.size() < fanout) {
                    return std::numeric_limits<double>::lowest();
                }

                const double current_ss_delay =
                    model.buffer_delay_ss(current_lib->name, fanout);
                const double current_ff_delay =
                    model.buffer_delay_ff(current_lib->name, fanout);
                const double current_buffer_area =
                    model.estimate_buffer_area(*current_lib);
                double best_value = std::numeric_limits<double>::lowest();
                for (const BufSpec& lib : libs) {
                    if (lib.name == current_lib->name ||
                        lib.ss_delay.size() < fanout ||
                        lib.ff_delay.size() < fanout) {
                        continue;
                    }
                    const double ss_delta =
                        model.buffer_delay_ss(lib.name, fanout) -
                        current_ss_delay;
                    const double ff_delta =
                        model.buffer_delay_ff(lib.name, fanout) -
                        current_ff_delay;
                    const bool useful_direction =
                        pressure > 0.0
                            ? (ss_delta >= -pressure_eps &&
                               ff_delta >= -pressure_eps &&
                               (ss_delta > pressure_eps ||
                                ff_delta > pressure_eps))
                            : (ss_delta <= pressure_eps &&
                               ff_delta <= pressure_eps &&
                               (ss_delta < -pressure_eps ||
                                ff_delta < -pressure_eps));
                    if (!useful_direction) continue;

                    const double area_delta =
                        model.estimate_buffer_area(lib) -
                        current_buffer_area;
                    const double estimated_value =
                        phase0_estimated_ranking_value(pressure,
                                                       ss_delta,
                                                       ff_delta,
                                                       area_delta);
                    best_value = std::max(best_value, estimated_value);
                }
                return best_value;
            };
            if (pressure > pressure_eps) {
                ++positive_count;
                node_candidates.push_back(
                    {node->name, pressure, estimate_best_type_value()});
            } else if (pressure < -pressure_eps) {
                ++negative_count;
                node_candidates.push_back(
                    {node->name, pressure, estimate_best_type_value()});
            } else {
                ++zero_count;
            }
        }
        if (phase0_ranking_timed_out || time_up()) {
            set_phase0_stop_reason(global_time_up() ? "global_time_budget" : "time_budget");
            break;
        }
        if (pass == 0) {
            summary.positive_pressure_candidates = positive_count;
            summary.negative_pressure_candidates = negative_count;
            summary.zero_pressure_candidates = zero_count;
        }

        std::sort(node_candidates.begin(), node_candidates.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.estimated_value != rhs.estimated_value) {
                return lhs.estimated_value > rhs.estimated_value;
            }
            if (std::abs(lhs.pressure) != std::abs(rhs.pressure)) {
                return std::abs(lhs.pressure) > std::abs(rhs.pressure);
            }
            return lhs.node_name < rhs.node_name;
        });
        if (time_up()) {
            set_phase0_stop_reason(global_time_up() ? "global_time_budget" : "time_budget");
            break;
        }
        summary.ranked_candidates_available =
            std::max(summary.ranked_candidates_available, node_candidates.size());

        if (pass == 0) {
            const int pressure_candidate_count =
                positive_count + negative_count + zero_count;
            if (cfg.enable_phase0_batch_trial) {
                use_phase0_batch_mode = cfg.enable_phase0_incremental_timing;
                summary.batch_auto_reason = "manual_on";
            } else if (!cfg.enable_phase0_batch_auto) {
                use_phase0_batch_mode = false;
                summary.batch_auto_reason = "disabled";
            } else if (baseline_ss_violations >=
                       static_cast<size_t>(std::max(0, cfg.phase0_batch_auto_min_ss_violations))) {
                use_phase0_batch_mode = cfg.enable_phase0_incremental_timing;
                summary.batch_auto_reason = "large_ss_violations";
            } else if (pressure_candidate_count >=
                       cfg.phase0_batch_auto_min_pressure_candidates) {
                use_phase0_batch_mode = cfg.enable_phase0_incremental_timing;
                summary.batch_auto_reason = "large_pressure_candidate_count";
            } else {
                use_phase0_batch_mode = false;
                summary.batch_auto_reason = "below_threshold";
            }
            summary.batch_mode_enabled = use_phase0_batch_mode;
        }

        size_t node_limit = node_candidates.size();
        if (cfg.phase0_max_trial_nodes > 0) {
            const size_t fraction_limit = buffer_nodes.empty()
                ? 0
                : std::max<size_t>(1, static_cast<size_t>(
                      std::ceil(cfg.phase0_node_fraction *
                                static_cast<double>(buffer_nodes.size()))));
            node_limit = std::min({
                node_candidates.size(),
                static_cast<size_t>(cfg.phase0_max_trial_nodes),
                fraction_limit,
            });
            const size_t total_budget =
                static_cast<size_t>(cfg.phase0_max_trial_nodes);
            const size_t already_scanned =
                static_cast<size_t>(std::max(0, summary.candidates_scanned));
            if (already_scanned >= total_budget) {
                set_phase0_stop_reason("candidate_budget");
                break;
            }
            node_limit =
                std::min(node_limit, total_budget - already_scanned);
        }

        const int accepted_before_pass = summary.accepted_resizes;
        int accepted_this_pass = 0;
        int consecutive_rejects = 0;
        int consecutive_failed_batches = 0;
        bool stop_current_pass = false;
        std::vector<Phase0BatchCandidate> pending_batch;
        pending_batch.reserve(static_cast<size_t>(std::max(1, cfg.phase0_batch_size)));
        for (size_t node_index = 0;
             node_index < node_limit && !time_up() && !stop_current_pass;
             ++node_index) {
            ++summary.candidates_scanned;
            const Phase0NodeCandidate& node_candidate = node_candidates[node_index];
            ClockNode* node = tree.find_node(node_candidate.node_name);
            if (!is_buffer_node(node)) continue;

            const BufSpec* current_lib = cached_lib(node->type);
            if (!current_lib) continue;
            const size_t fanout = node->children.size();
            if (fanout == 0 ||
                current_lib->ss_delay.size() < fanout ||
                current_lib->ff_delay.size() < fanout) {
                continue;
            }

            const double current_ss_delay = model.buffer_delay_ss(current_lib->name, fanout);
            const double current_ff_delay = model.buffer_delay_ff(current_lib->name, fanout);
            const double current_buffer_area = model.estimate_buffer_area(*current_lib);
            std::vector<Phase0TypeCandidate> type_candidates;

            for (const auto& lib : libs) {
                if (lib.name == current_lib->name ||
                    lib.ss_delay.size() < fanout ||
                    lib.ff_delay.size() < fanout) {
                    continue;
                }

                const double ss_delta = model.buffer_delay_ss(lib.name, fanout) - current_ss_delay;
                const double ff_delta = model.buffer_delay_ff(lib.name, fanout) - current_ff_delay;
                const bool useful_direction =
                    node_candidate.pressure > 0.0
                        ? (ss_delta >= -pressure_eps && ff_delta >= -pressure_eps &&
                           (ss_delta > pressure_eps || ff_delta > pressure_eps))
                        : (ss_delta <= pressure_eps && ff_delta <= pressure_eps &&
                           (ss_delta < -pressure_eps || ff_delta < -pressure_eps));
                if (!useful_direction) continue;

                const double average_delta = 0.5 * (ss_delta + ff_delta);
                const double area_delta = model.estimate_buffer_area(lib) - current_buffer_area;
                const double ranking_score =
                    phase0_estimated_ranking_value(node_candidate.pressure,
                                                   ss_delta,
                                                   ff_delta,
                                                   area_delta);
                const int current_rank = type_rank.count(current_lib->name)
                    ? type_rank[current_lib->name]
                    : 0;
                const int candidate_rank = type_rank.count(lib.name) ? type_rank[lib.name] : 0;
                type_candidates.push_back({
                    &lib,
                    ranking_score,
                    average_delta,
                    area_delta,
                    std::abs(candidate_rank - current_rank),
                });
            }

            std::sort(type_candidates.begin(), type_candidates.end(), [](const auto& lhs, const auto& rhs) {
                constexpr double eps = 1e-12;
                if (std::abs(lhs.ranking_score - rhs.ranking_score) > eps) {
                    return lhs.ranking_score > rhs.ranking_score;
                }
                if (std::abs(lhs.area_delta - rhs.area_delta) > eps) {
                    return lhs.area_delta < rhs.area_delta;
                }
                if (std::abs(std::abs(lhs.average_delay_delta) -
                             std::abs(rhs.average_delay_delta)) > eps) {
                    return std::abs(lhs.average_delay_delta) < std::abs(rhs.average_delay_delta);
                }
                if (lhs.type_distance != rhs.type_distance) {
                    return lhs.type_distance < rhs.type_distance;
                }
                return lhs.lib->name < rhs.lib->name;
            });

            const size_t type_limit = std::min(
                type_candidates.size(),
                static_cast<size_t>(std::max(0, cfg.phase0_max_types_per_node)));
            if (use_phase0_batch_mode) {
                if (type_limit == 0) continue;
                const Phase0TypeCandidate& candidate = type_candidates.front();
                Phase0BatchCandidate batch_candidate;
                batch_candidate.node_name = node->name;
                batch_candidate.old_type = current_lib->name;
                batch_candidate.new_type = candidate.lib->name;
                batch_candidate.pressure = node_candidate.pressure;
                batch_candidate.area_delta = candidate.area_delta;

                const size_t batch_size =
                    static_cast<size_t>(std::max(1, cfg.phase0_batch_size));
                if (!pending_batch.empty() &&
                    (!batch_is_independent(pending_batch, batch_candidate.node_name) ||
                     pending_batch.size() >= batch_size)) {
                    const bool accepted_batch =
                        try_phase0_batch(pending_batch, 0);
                    if (accepted_batch) {
                        consecutive_failed_batches = 0;
                    } else {
                        ++consecutive_failed_batches;
                        if (cfg.phase0_max_consecutive_failed_batches > 0 &&
                            consecutive_failed_batches >=
                                cfg.phase0_max_consecutive_failed_batches) {
                            set_phase0_stop_reason("consecutive_failed_batches");
                            stop_current_pass = true;
                        }
                    }
                    pending_batch.clear();
                    if (stop_current_pass) break;
                }

                if (batch_is_independent(pending_batch, batch_candidate.node_name)) {
                    pending_batch.push_back(batch_candidate);
                }
                if (pending_batch.size() >= batch_size) {
                    const bool accepted_batch =
                        try_phase0_batch(pending_batch, 0);
                    if (accepted_batch) {
                        consecutive_failed_batches = 0;
                    } else {
                        ++consecutive_failed_batches;
                        if (cfg.phase0_max_consecutive_failed_batches > 0 &&
                            consecutive_failed_batches >=
                                cfg.phase0_max_consecutive_failed_batches) {
                            set_phase0_stop_reason("consecutive_failed_batches");
                            stop_current_pass = true;
                        }
                    }
                    pending_batch.clear();
                }
                continue;
            }

            const Phase0TypeCandidate* best_candidate = nullptr;
            TimingAnalysisResult best_timing;
            double best_cost = std::numeric_limits<double>::infinity();
            double best_score = -std::numeric_limits<double>::infinity();
            double best_area = std::numeric_limits<double>::infinity();

            for (size_t type_index = 0; type_index < type_limit && !time_up(); ++type_index) {
                const Phase0TypeCandidate& candidate = type_candidates[type_index];
                ++summary.attempted_resizes;
                const auto trial_start = std::chrono::steady_clock::now();

                TimingAnalysisResult trial_timing;
                double trial_cost = std::numeric_limits<double>::infinity();
                double trial_score = -std::numeric_limits<double>::infinity();
                const double trial_area = current_area + candidate.area_delta;
                bool trial_applied = false;
                const bool local_legality_ok =
                    phase0_local_resize_ok(node, candidate.lib->name);

                if (local_legality_ok && phase0_fast_timing) {
                    FastTimingTrialSummary trial =
                        phase0_fast_timing->trial_resize(tree,
                                                         node->name,
                                                         candidate.lib->name,
                                                         phase0_fast_txn);
                    if (trial.ok) {
                        trial_applied = true;
                        trial_timing = trial.timing;
                        trial_cost = normalized_phase0_timing_cost(trial_timing,
                                                                   baseline.timing,
                                                                   cfg.phase0_wns_weight);
                        phase0_fast_timing->rollback(phase0_fast_txn);
                    }
                } else if (local_legality_ok && cfg.enable_phase0_incremental_timing) {
                    Phase0IncrementalTrial trial;
                    trial_applied = apply_incremental_type_change(node,
                                                                  candidate.lib->name,
                                                                  trial);
                    if (trial_applied) {
                        trial_timing = phase0_timing_cache.timing;
                        trial_cost = normalized_phase0_timing_cost(trial_timing,
                                                                   baseline.timing,
                                                                   cfg.phase0_wns_weight);
                    }
                    rollback_incremental_type_change(node, trial);
                } else if (local_legality_ok) {
                    trial_applied = tree.set_buffer_type(node->name, candidate.lib->name);
                    if (trial_applied) {
                        trial_timing = model.analyze_timing(tree,
                                                            ss_paths,
                                                            ff_paths,
                                                            clock_period);
                        trial_cost = normalized_phase0_timing_cost(trial_timing,
                                                                   baseline.timing,
                                                                   cfg.phase0_wns_weight);
                        tree.set_buffer_type(node->name, current_lib->name);
                    }
                }

                summary.total_trial_time_seconds +=
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - trial_start).count();
                if (!trial_applied) continue;
                trial_score = weighted_objective_score(model,
                                                       trial_timing,
                                                       trial_area,
                                                       baseline,
                                                       cfg);

                const bool acceptable =
                    cfg.enable_phase0_score_based_acceptance
                        ? score_strictly_better(trial_score,
                                                current_score,
                                                cfg)
                        : phase0_pareto_acceptable(trial_timing,
                                                   trial_area);
                if (!acceptable) {
                    ++consecutive_rejects;
                    if (cfg.phase0_max_consecutive_rejects > 0 &&
                        consecutive_rejects >= cfg.phase0_max_consecutive_rejects) {
                        set_phase0_stop_reason("consecutive_rejects");
                        stop_current_pass = true;
                        break;
                    }
                    continue;
                }

                const bool better =
                    !best_candidate ||
                    (cfg.enable_phase0_score_based_acceptance
                         ? score_strictly_better(trial_score, best_score, cfg)
                         : trial_cost < best_cost - timing_improvement_eps) ||
                    ((cfg.enable_phase0_score_based_acceptance
                          ? std::abs(trial_score - best_score) <=
                                cfg.score_acceptance_epsilon
                          : std::abs(trial_cost - best_cost) <=
                                timing_improvement_eps) &&
                     (trial_area < best_area - 1e-9 ||
                      (std::abs(trial_area - best_area) <= 1e-9 &&
                       (std::abs(candidate.average_delay_delta) <
                            std::abs(best_candidate->average_delay_delta) - 1e-12 ||
                        (std::abs(std::abs(candidate.average_delay_delta) -
                                  std::abs(best_candidate->average_delay_delta)) <= 1e-12 &&
                         candidate.type_distance < best_candidate->type_distance)))));
                if (better) {
                    best_candidate = &candidate;
                    best_timing = trial_timing;
                    best_cost = trial_cost;
                    best_score = trial_score;
                    best_area = trial_area;
                }
            }

            if (!best_candidate) continue;
            const double accepted_old_area = current_area;
            if (phase0_fast_timing) {
                FastTimingTrialSummary trial =
                    phase0_fast_timing->trial_resize(tree,
                                                     node->name,
                                                     best_candidate->lib->name,
                                                     phase0_fast_txn);
                if (!trial.ok ||
                    !phase0_fast_timing->commit(tree, phase0_fast_txn)) {
                    continue;
                }
                current_timing = trial.timing;
                current_area = trial.area;
                if (cfg.enable_fast_timing_verify) {
                    const int interval = cfg.fast_timing_verify_interval;
                    const bool verify_now =
                        interval <= 0 ||
                        phase0_fast_timing->counters().commit_count % interval == 0;
                    if (verify_now &&
                        !phase0_fast_timing->verify_against_delay_model(tree,
                                                                        model,
                                                                        ss_paths,
                                                                        ff_paths,
                                                                        clock_period,
                                                                        "accepted Phase0 resize " + node->name,
                                                                        phase0_verify_eps,
                                                                        &std::cerr)) {
                        std::abort();
                    }
                }
            } else if (cfg.enable_phase0_incremental_timing) {
                Phase0IncrementalTrial accepted_trial;
                if (!apply_incremental_type_change(node,
                                                   best_candidate->lib->name,
                                                   accepted_trial)) {
                    continue;
                }
                current_timing = phase0_timing_cache.timing;
                if (cfg.enable_phase0_incremental_verify) {
                    const TimingAnalysisResult full_timing =
                        model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
                    if (!timing_metrics_close(current_timing,
                                              full_timing,
                                              phase0_verify_eps)) {
                        ++summary.incremental_verify_failures;
                        print_timing_metric_mismatch("accepted Phase0 resize " + node->name,
                                                     current_timing,
                                                     full_timing,
                                                     phase0_verify_eps);
                        std::abort();
                    }
                }
            } else {
                if (!tree.set_buffer_type(node->name, best_candidate->lib->name)) continue;
                current_timing = best_timing;
            }

            Phase0MoveRecord record;
            record.node_name = node->name;
            record.old_type = current_lib->name;
            record.new_type = best_candidate->lib->name;
            record.pressure = node_candidate.pressure;
            record.old_timing_cost = current_cost;
            record.new_timing_cost = best_cost;
            record.old_area = accepted_old_area;
            record.new_area = best_area;
            record.area_delta = best_area - accepted_old_area;
            summary.move_records.push_back(record);

            std::ostringstream move;
            move << "Phase0 resize " << record.node_name
                 << " " << record.old_type << " -> " << record.new_type
                 << " pressure=" << record.pressure
                 << " cost=" << record.old_timing_cost << "->" << record.new_timing_cost
                 << " area_delta=" << record.area_delta;
            summary.applied_moves.push_back(move.str());
            ++summary.accepted_resizes;
            ++summary.fallback_individual_accepted;
            ++accepted_this_pass;
            consecutive_rejects = 0;
            current_cost = best_cost;
            current_score = best_score;
            current_area = best_area;
            ++summary.incremental_area_updates;
            verify_phase0_full_validation("accepted Phase0 resize " + node->name);
            verify_phase0_area("accepted Phase0 resize " + node->name, current_area);
        }

        if (use_phase0_batch_mode && !pending_batch.empty()) {
            const bool accepted_batch = try_phase0_batch(pending_batch, 0);
            if (accepted_batch) {
                consecutive_failed_batches = 0;
            } else {
                ++consecutive_failed_batches;
                if (cfg.phase0_max_consecutive_failed_batches > 0 &&
                    consecutive_failed_batches >=
                        cfg.phase0_max_consecutive_failed_batches) {
                    set_phase0_stop_reason("consecutive_failed_batches");
                }
            }
            pending_batch.clear();
        }
        if (use_phase0_batch_mode) {
            accepted_this_pass = summary.accepted_resizes - accepted_before_pass;
        }

        if (time_up()) {
            set_phase0_stop_reason(global_time_up() ? "global_time_budget" : "time_budget");
            break;
        }
        if (accepted_this_pass == 0) {
            if (summary.early_stop_reason == "unknown") {
                set_phase0_stop_reason("candidates_exhausted");
            }
            break;
        }
    }

    if (summary.early_stop_reason == "unknown") {
        if (time_up()) {
            set_phase0_stop_reason(global_time_up() ? "global_time_budget" : "time_budget");
        } else {
            set_phase0_stop_reason("max_passes");
        }
    }

    summary.rejected_resizes = summary.attempted_resizes - summary.accepted_resizes;
    summary.average_trial_time_seconds =
        summary.attempted_resizes > 0
            ? summary.total_trial_time_seconds / static_cast<double>(summary.attempted_resizes)
            : 0.0;
    summary.final_phase0_timing_cost = current_cost;
    summary.area_after_phase0 = current_area;
    summary.area_ratio_after_phase0 =
        summary.area_before_phase0 > 0.0
            ? summary.area_after_phase0 / summary.area_before_phase0
            : 1.0;
    summary.runtime_seconds = elapsed_seconds();
    capture(summary.after_phase0, "After Phase0", summary.accepted_resizes);
    summary.after_phase0.timing =
        compact_timing_snapshot(summary.after_phase0.timing);
    if (phase0_fast_timing) {
        const FastTimingCounters& fast = phase0_fast_timing->counters();
        summary.fast_engine_build_time_seconds += fast.build_time_seconds;
        summary.fast_engine_sync_time_seconds += fast.sync_time_seconds;
        summary.fast_trial_time_seconds += fast.trial_time_seconds;
        summary.fast_group_collection_time_seconds +=
            fast.group_collection_time_seconds;
        summary.fast_group_update_time_seconds +=
            fast.group_update_time_seconds;
        summary.fast_sync_count += fast.sync_count;
        summary.fast_trial_count += fast.trial_count;
        summary.fast_resize_trials += fast.resize_trials;
        summary.fast_resize_batch_trials += fast.resize_batch_trials;
        summary.fast_resize_batch_candidates +=
            fast.resize_batch_candidates;
        summary.fast_insert_trials += fast.insert_trials;
        summary.fast_delete_trials += fast.delete_trials;
        summary.fast_commit_count += fast.commit_count;
        summary.fast_rollback_count += fast.rollback_count;
        summary.fast_verify_count += fast.verify_count;
        summary.fast_fallback_count += fast.fallback_count;
        summary.fast_arrival_snapshot_count += fast.arrival_snapshot_count;
        summary.fast_affected_group_count += fast.affected_group_count;
        summary.fast_max_arrival_snapshots_per_trial =
            std::max(summary.fast_max_arrival_snapshots_per_trial,
                     fast.max_arrival_snapshots_per_trial);
        summary.fast_max_affected_groups_per_trial =
            std::max(summary.fast_max_affected_groups_per_trial,
                     fast.max_affected_groups_per_trial);
    }
    return summary;
}

OptimizationSummary Optimizer::optimize(ClockTree& tree,
                                        const std::vector<BufSpec>& libs,
                                        const std::vector<PathInfo>& ss_paths,
                                        const std::vector<PathInfo>& ff_paths,
                                        double clock_period,
                                        bool reset_phase0,
                                        const std::string& phase0_branch_name) {
    OptimizationSummary summary;
    summary.runtime_profile.enabled = cfg.enable_runtime_profiling;
    summary.phase0_enabled = cfg.enable_phase0;
    summary.phase0_reset_experiment_enabled = cfg.enable_phase0_reset_experiment;
    summary.final_alt.enabled = cfg.enable_final_alternating_greedy;
    summary.final_alt.replace_phase1b_and_phase2 = cfg.final_alt_replace_phase1b_and_phase2;
    summary.final_alt.max_iters = cfg.final_alt_max_iters;
    summary.final_alt.repair_insertions_per_iter = cfg.final_alt_repair_insertions_per_iter;
    summary.final_alt.reclaim_giveback_ratio = cfg.final_alt_reclaim_giveback_ratio;
    summary.final_alt.hard_wns_guard = cfg.final_alt_reclaim_hard_wns_guard;
    summary.final_alt.area_decrease_only = cfg.final_alt_reclaim_area_decrease_only;
    summary.final_alt.target_checkpoint_enabled =
        cfg.enable_final_alt_target_checkpoint;
    const size_t testcase_path_count =
        std::max(ss_paths.size(), ff_paths.size());
    const bool use_large_case_finalization_reserve =
        cfg.perturb_recover_large_case_min_paths > 0 &&
        testcase_path_count >= cfg.perturb_recover_large_case_min_paths;
    const bool perturb_recover_disabled_for_large_case =
        cfg.perturb_recover_disable_on_large_case &&
        use_large_case_finalization_reserve;
    const bool existing_shared_disabled_for_large_case =
        cfg.existing_shared_disable_on_large_case &&
        use_large_case_finalization_reserve;
    summary.existing_buffer_shared_reclaim_skipped_for_large_case =
        existing_shared_disabled_for_large_case;
    const bool perturb_recover_enabled =
        cfg.enable_perturb_recover &&
        cfg.enable_final_alternating_greedy &&
        cfg.final_alt_replace_phase1b_and_phase2 &&
        !perturb_recover_disabled_for_large_case;
    const double effective_finalization_reserve_seconds =
        std::max(
            0.0,
            use_large_case_finalization_reserve
                ? std::max(
                      cfg.perturb_recover_validation_reserve_seconds,
                      cfg.perturb_recover_large_case_validation_reserve_seconds)
                : cfg.perturb_recover_validation_reserve_seconds);
    const int effective_final_alt_reclaim_top_k =
        use_large_case_finalization_reserve &&
                cfg.final_alt_large_case_reclaim_top_k_candidates > 0
            ? cfg.final_alt_large_case_reclaim_top_k_candidates
            : cfg.final_alt_reclaim_top_k_candidates;
    const int effective_final_alt_reclaim_top_k_per_kind =
        use_large_case_finalization_reserve &&
                cfg.final_alt_large_case_reclaim_top_k_per_kind > 0
            ? cfg.final_alt_large_case_reclaim_top_k_per_kind
            : cfg.final_alt_reclaim_top_k_per_kind;
    summary.perturb_recover.enabled = perturb_recover_enabled;
    summary.perturb_recover.recovery_score_based =
        cfg.enable_perturb_recover_score_based_repair_reclaim;
    summary.perturb_recover.target_checkpoint_enabled =
        cfg.enable_perturb_recover_target_checkpoint;
    summary.perturb_recover.target_outer_acceptance_enabled =
        cfg.enable_perturb_recover_target_outer_acceptance;
    summary.perturb_recover.target_checkpoint_alpha =
        cfg.perturb_recover_checkpoint_alpha;
    summary.perturb_recover.target_checkpoint_beta =
        cfg.perturb_recover_checkpoint_beta;
    summary.perturb_recover.target_checkpoint_gamma =
        cfg.perturb_recover_checkpoint_gamma;
    summary.perturb_recover.max_cycles_without_target_improvement =
        cfg.perturb_recover_max_cycles_without_target_improvement;
    if (cfg.enable_perturb_recover && !perturb_recover_enabled) {
        summary.perturb_recover.stop_reason =
            perturb_recover_disabled_for_large_case
                ? "disabled_for_large_case"
                : "requires_final_alt_replacement";
    }
    summary.perturb_recover.random_seed = cfg.perturb_recover_random_seed;
    summary.perturb_recover.time_budget_seconds =
        cfg.perturb_recover_time_budget_seconds;
    summary.perturb_recover.validation_reserve_seconds =
        effective_finalization_reserve_seconds;
    summary.perturb_recover.max_cycles = cfg.perturb_recover_max_cycles;
    summary.perturb_recover.brutal_area_path_enabled =
        cfg.perturb_recover_use_brutal_area_path;
    summary.perturb_recover.brutal_recovery_max_cycles =
        cfg.perturb_recover_brutal_recovery_max_cycles;
    summary.perturb_recover.candidates_per_cycle =
        cfg.perturb_recover_use_brutal_area_path
            ? 1
            : cfg.perturb_recover_candidates_per_cycle;
    summary.perturb_recover.candidate_profiles_enabled =
        cfg.perturb_recover_enable_candidate_profiles;
    summary.perturb_recover.guided_target_ratio =
        cfg.perturb_recover_guided_target_ratio;
    summary.perturb_recover.min_moves = cfg.perturb_recover_min_moves;
    summary.perturb_recover.max_moves = cfg.perturb_recover_max_moves;
    summary.perturb_recover.intensity_step_streak =
        cfg.perturb_recover_intensity_step_streak;
    summary.perturb_recover.move_attempt_multiplier =
        cfg.perturb_recover_move_attempt_multiplier;
    summary.perturb_recover.quick_recovery_iters =
        cfg.perturb_recover_quick_recovery_iters;
    summary.perturb_recover.quick_repair_attempts =
        cfg.perturb_recover_quick_repair_attempts;
    summary.perturb_recover.quick_reclaim_candidates =
        cfg.perturb_recover_quick_reclaim_candidates;
    summary.perturb_recover.cycle_deep_recovery_enabled =
        cfg.perturb_recover_use_cycle_deep_recovery;
    summary.perturb_recover.bootstrap_reclaim_candidates =
        cfg.perturb_recover_bootstrap_reclaim_candidates;
    summary.perturb_recover.bootstrap_protect_inserted =
        cfg.perturb_recover_bootstrap_protect_inserted;
    summary.perturb_recover.recovery_iters =
        cfg.perturb_recover_recovery_iters;
    summary.perturb_recover.repair_attempts =
        cfg.perturb_recover_repair_attempts;
    summary.perturb_recover.reclaim_candidates =
        cfg.perturb_recover_reclaim_candidates;
    summary.diagnostics.incremental_area_verify = cfg.enable_incremental_area_verify;
    summary.diagnostics.phase0_trial_full_validation_verify =
        cfg.enable_phase0_trial_full_validation_verify;
    summary.diagnostics.final_alt_trial_full_validation_verify =
        cfg.enable_final_alt_trial_full_validation_verify;
    summary.fast_timing.enabled = cfg.enable_fast_timing_engine;
    summary.fast_timing.verify_enabled = cfg.enable_fast_timing_verify;
    summary.fast_timing.verify_interval = cfg.fast_timing_verify_interval;
    summary.fast_timing.resize_trials_enabled = cfg.enable_fast_timing_engine;
    summary.fast_timing.delete_trials_enabled = cfg.enable_fast_timing_engine;
    summary.fast_timing.insert_trials_enabled = cfg.enable_fast_timing_engine;
    summary.second_round.type_id_cache_enabled = cfg.enable_type_id_cache;
    summary.second_round.indexed_timing_enabled = cfg.enable_indexed_timing_paths;
    summary.second_round.phase0_endpoint_delta_pressure_enabled =
        cfg.enable_phase0_endpoint_delta_pressure;
    summary.second_round.final_alt_ranked_reclaim_enabled =
        cfg.enable_final_alt_ranked_reclaim_candidates;
    summary.second_round.final_alt_topk_limit =
        effective_final_alt_reclaim_top_k;
    summary.second_round.final_alt_topk_per_kind_limit =
        effective_final_alt_reclaim_top_k_per_kind;
    if (!tree.root) {
        summary.message = "Optimization skipped: tree has no root.";
        return summary;
    }

    DelayModel model(libs);
    std::unique_ptr<FastTimingEngine> fast_timing_engine;
    if (cfg.enable_fast_timing_engine) {
        fast_timing_engine = std::make_unique<FastTimingEngine>(libs,
                                                                ss_paths,
                                                                ff_paths,
                                                                clock_period);
    }
    LibraryCache lib_cache =
        build_library_cache(libs,
                            &summary.second_round.library_cache_lookups,
                            &summary.second_round.library_cache_misses);
    std::unordered_map<std::string, const BufSpec*> lib_by_name;
    lib_by_name.reserve(libs.size());
    for (const auto& lib : libs) {
        lib_by_name.emplace(lib.name, &lib);
    }
    if (cfg.enable_type_id_cache_verify) {
        for (const auto& lib : libs) {
            const int type_id = lib_cache.get_type_id(lib.name);
            const BufSpec* cached = type_id >= 0 ? lib_cache.type_by_id[static_cast<size_t>(type_id)] : nullptr;
            if (cached != &lib) {
                std::cerr << "Library type-id cache mismatch for " << lib.name << '\n';
                std::abort();
            }
        }
    }
    auto cached_lib = [&](const std::string& name) -> const BufSpec* {
        if (cfg.enable_type_id_cache) {
            const BufSpec* lib = lib_cache.get(name);
            if (!lib) {
                ++summary.diagnostics.library_lookup_cache_misses;
                return nullptr;
            }
            ++summary.diagnostics.library_lookup_cache_hits;
            return lib;
        }
        auto it = lib_by_name.find(name);
        if (it == lib_by_name.end()) {
            ++summary.diagnostics.library_lookup_cache_misses;
            ++summary.second_round.linear_library_scans_remaining;
            return nullptr;
        }
        ++summary.diagnostics.library_lookup_cache_hits;
        ++summary.second_round.linear_library_scans_remaining;
        return it->second;
    };
    auto compute_area_full = [&]() {
        ++summary.diagnostics.full_area_recomputations;
        return model.compute_tree_area(tree);
    };
    auto validate_full = [&]() {
        ++summary.diagnostics.full_legality_validations_total;
        return model.validate_legality(tree, libs);
    };
    auto cached_reclaim_delete_ok = [&](const ClockNode* node) {
        if (!node || node->is_sink || node->original || !node->parent) return false;
        const ClockNode* parent = node->parent;
        const size_t parent_fanout_after_delete =
            parent->children.empty()
                ? 0
                : parent->children.size() - 1 + node->children.size();
        if (parent->parent && !parent->type.empty()) {
            return buffer_type_supports_fanout(cached_lib(parent->type),
                                               parent_fanout_after_delete);
        }
        return true;
    };
    auto verify_incremental_area = [&](const std::string& context,
                                       double tracked_area) {
        if (!cfg.enable_incremental_area_verify) return;
        const double full_area = compute_area_full();
        if (std::abs(full_area - tracked_area) > 1e-6) {
            std::cerr << "Incremental area mismatch after " << context
                      << ": tracked=" << tracked_area
                      << " full=" << full_area << '\n';
            std::abort();
        }
    };
    const auto start_time = std::chrono::steady_clock::now();
    auto elapsed_seconds = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    };
    auto is_time_up = [&]() {
        return elapsed_seconds() >= cfg.time_limit_seconds;
    };
    auto seconds_since = [](const std::chrono::steady_clock::time_point& start) {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    };
    auto verify_cached_timing = [&](const std::string& context,
                                    const DelayModel& timing_model,
                                    const ClockTree& timing_tree,
                                    const std::vector<PathInfo>& timing_ss_paths,
                                    const std::vector<PathInfo>& timing_ff_paths,
                                    double timing_clock_period,
                                    const TimingAnalysisResult& cached) {
        verify_cached_timing_impl(context,
                                  timing_model,
                                  timing_tree,
                                  timing_ss_paths,
                                  timing_ff_paths,
                                  timing_clock_period,
                                  cached,
                                  cfg.enable_timing_cache_verify);
    };
    auto verify_fast_timing = [&](const std::string& context) {
        if (!cfg.enable_fast_timing_verify || !fast_timing_engine) return true;
        const bool ok =
            fast_timing_engine->verify_against_delay_model(tree,
                                                           model,
                                                           ss_paths,
                                                           ff_paths,
                                                           clock_period,
                                                           context,
                                                           1e-6,
                                                           &std::cerr);
        if (!ok) {
            std::cerr << "FastTimingEngine disabled by verification failure after "
                      << context << '\n';
            return false;
        }
        return true;
    };
    auto sync_fast_timing = [&](const std::string& context) {
        if (!fast_timing_engine) return false;
        if (!fast_timing_engine->sync_from_tree(tree)) return false;
        return verify_fast_timing(context);
    };
    auto verify_fast_timing_after_commit = [&](const std::string& context) {
        if (!cfg.enable_fast_timing_verify || !fast_timing_engine) return true;
        const int interval = cfg.fast_timing_verify_interval;
        if (interval > 0 &&
            fast_timing_engine->counters().commit_count % interval != 0) {
            return true;
        }
        return verify_fast_timing(context);
    };

    // Establish one immutable normalization baseline and one replaceable best
    // checkpoint for the complete pipeline.
    if (!external_baseline) {
        baseline.timing =
            model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        baseline.area = compute_area_full();
        baseline.valid = true;
    }

    // Keep the broad, low-risk portion of the search area-oriented, then
    // smoothly trade that bias for timing closure.  The schedule advances once
    // per complete Repair/Reclaim cycle so every transactional trial inside a
    // cycle is evaluated against one stable objective.
    auto apply_score_weight_schedule = [&](int repair_cycle_index,
                                           bool force_final_weights) {
        if (!cfg.enable_score_weight_scheduler) return;
        const int hold_cycles = std::max(
            0, cfg.score_weight_schedule_hold_repair_cycles);
        const int transition_cycles = std::max(
            1, cfg.score_weight_schedule_transition_repair_cycles);
        double transition_progress = 0.0;
        if (force_final_weights) {
            transition_progress = 1.0;
        } else if (repair_cycle_index >= hold_cycles) {
            transition_progress = std::clamp(
                static_cast<double>(repair_cycle_index - hold_cycles + 1) /
                    static_cast<double>(transition_cycles),
                0.0,
                1.0);
        }
        const double cosine_progress =
            0.5 - 0.5 * std::cos(std::acos(-1.0) * transition_progress);
        auto interpolate = [&](double start, double end) {
            return start + (end - start) * cosine_progress;
        };
        cfg.alpha = interpolate(cfg.score_weight_schedule_start_alpha,
                                cfg.score_weight_schedule_end_alpha);
        cfg.beta = interpolate(cfg.score_weight_schedule_start_beta,
                               cfg.score_weight_schedule_end_beta);
        cfg.gamma = interpolate(cfg.score_weight_schedule_start_gamma,
                                cfg.score_weight_schedule_end_gamma);
    };
    if (cfg.enable_score_weight_scheduler &&
        cfg.score_weight_schedule_apply_to_phase0) {
        apply_score_weight_schedule(0, false);
    }

    // This checkpoint is intentionally captured before the main Phase0 run,
    // but it is never executed first: full_score remains the fallback route.
    // It lets a later route test a genuinely different initial basin by
    // perturbing the original tree and then re-running Phase0.
    const bool retain_prephase_diversification_checkpoint =
        cfg.enable_post_phase0_full_pipeline_fork_experiment &&
        cfg.enable_phase0 &&
        (cfg.portfolio_allow_prephase_routes_on_large_case ||
         !use_large_case_finalization_reserve) &&
        (cfg.portfolio_include_aggressive_prephase_branches ||
         cfg.portfolio_include_cosine_schedule_branch);
    ClockTree prephase_diversification_checkpoint;
    if (retain_prephase_diversification_checkpoint) {
        prephase_diversification_checkpoint = tree.clone();
    }

    ClockTree best_checkpoint_tree;
    double best_checkpoint_score =
        weighted_objective_score(model,
                                 baseline.timing,
                                 baseline.area,
                                 baseline,
                                 cfg);
    summary.checkpoint.enabled = cfg.enable_best_checkpoint;
    summary.checkpoint.initial_score = best_checkpoint_score;
    summary.checkpoint.best_score = best_checkpoint_score;
    summary.checkpoint.best_area = baseline.area;
    summary.checkpoint.best_timing = compact_timing_snapshot(baseline.timing);
    if (cfg.enable_best_checkpoint) {
        best_checkpoint_tree = tree.clone();
    }

    auto update_best_checkpoint = [&](const TimingAnalysisResult& timing,
                                      double area,
                                      const std::string& stage) {
        if (!cfg.enable_best_checkpoint) return;
        const double score =
            weighted_objective_score(model, timing, area, baseline, cfg);
        if (!score_strictly_better(score, best_checkpoint_score, cfg)) return;
        best_checkpoint_tree = tree.clone();
        best_checkpoint_score = score;
        summary.checkpoint.updates += 1;
        summary.checkpoint.best_stage = stage;
        summary.checkpoint.best_score = score;
        summary.checkpoint.best_area = area;
        summary.checkpoint.best_timing = compact_timing_snapshot(timing);
    };

    if (cfg.enable_phase0) {
        const auto phase0_profile_start = std::chrono::steady_clock::now();
        summary.phase0 = run_phase0_timing_conditioning(tree,
                                                       libs,
                                                       ss_paths,
                                                       ff_paths,
                                                       clock_period,
                                                       reset_phase0,
                                                       phase0_branch_name,
                                                       start_time);
        summary.diagnostics.full_legality_validations_phase0_trials +=
            summary.phase0.full_legality_validations_phase0_trials;
        summary.diagnostics.full_legality_validations_total +=
            summary.phase0.full_legality_validations_phase0_trials;
        summary.diagnostics.local_legality_checks_phase0 +=
            summary.phase0.local_legality_checks_phase0;
        summary.diagnostics.incremental_area_updates +=
            summary.phase0.incremental_area_updates;
        summary.diagnostics.full_area_recomputations +=
            summary.phase0.full_area_recomputations;
        summary.diagnostics.library_lookup_cache_hits +=
            summary.phase0.library_lookup_cache_hits;
        summary.diagnostics.library_lookup_cache_misses +=
            summary.phase0.library_lookup_cache_misses;
        summary.second_round.library_cache_lookups +=
            summary.phase0.library_lookup_cache_hits +
            summary.phase0.library_lookup_cache_misses;
        summary.second_round.library_cache_misses +=
            summary.phase0.library_lookup_cache_misses;
        summary.second_round.phase0_pressure_method =
            summary.phase0.pressure_method;
        summary.second_round.phase0_pressure_time_seconds +=
            summary.phase0.pressure_time_seconds;
        summary.second_round.phase0_pressure_verify_max_abs_diff =
            std::max(summary.second_round.phase0_pressure_verify_max_abs_diff,
                     summary.phase0.pressure_verify_max_abs_diff);
        summary.fast_timing.build_time_seconds +=
            summary.phase0.fast_engine_build_time_seconds;
        summary.fast_timing.sync_time_seconds +=
            summary.phase0.fast_engine_sync_time_seconds;
        summary.fast_timing.trial_time_seconds +=
            summary.phase0.fast_trial_time_seconds;
        summary.fast_timing.group_collection_time_seconds +=
            summary.phase0.fast_group_collection_time_seconds;
        summary.fast_timing.group_update_time_seconds +=
            summary.phase0.fast_group_update_time_seconds;
        summary.fast_timing.sync_count += summary.phase0.fast_sync_count;
        summary.fast_timing.trial_count += summary.phase0.fast_trial_count;
        summary.fast_timing.resize_trials += summary.phase0.fast_resize_trials;
        summary.fast_timing.resize_batch_trials +=
            summary.phase0.fast_resize_batch_trials;
        summary.fast_timing.resize_batch_candidates +=
            summary.phase0.fast_resize_batch_candidates;
        summary.fast_timing.insert_trials += summary.phase0.fast_insert_trials;
        summary.fast_timing.delete_trials += summary.phase0.fast_delete_trials;
        summary.fast_timing.commit_count += summary.phase0.fast_commit_count;
        summary.fast_timing.rollback_count += summary.phase0.fast_rollback_count;
        summary.fast_timing.verify_count += summary.phase0.fast_verify_count;
        summary.fast_timing.fallback_count += summary.phase0.fast_fallback_count;
        summary.fast_timing.arrival_snapshot_count +=
            summary.phase0.fast_arrival_snapshot_count;
        summary.fast_timing.affected_group_count +=
            summary.phase0.fast_affected_group_count;
        summary.fast_timing.max_arrival_snapshots_per_trial =
            std::max(summary.fast_timing.max_arrival_snapshots_per_trial,
                     summary.phase0.fast_max_arrival_snapshots_per_trial);
        summary.fast_timing.max_affected_groups_per_trial =
            std::max(summary.fast_timing.max_affected_groups_per_trial,
                     summary.phase0.fast_max_affected_groups_per_trial);
        if (summary.runtime_profile.enabled) {
            summary.runtime_profile.phase0_seconds +=
                seconds_since(phase0_profile_start);
        }
        if (summary.phase0.after_phase0.valid) {
            update_best_checkpoint(summary.phase0.after_phase0.timing,
                                   summary.phase0.after_phase0.area,
                                   "phase0");
        }
    }

    const bool portfolio_allowed =
        cfg.enable_post_phase0_full_pipeline_fork_experiment &&
        !(cfg.portfolio_disable_on_large_case &&
          use_large_case_finalization_reserve);
    // Retain only aggregate metrics after this point; a portfolio branch never
    // needs another copy of every per-path record from the shared checkpoint.
    TimingAnalysisResult phase0_timing =
        portfolio_allowed
            ? model.analyze_timing(tree, ss_paths, ff_paths, clock_period)
            : TimingAnalysisResult{};
    if (portfolio_allowed) {
        const auto portfolio_start = std::chrono::steady_clock::now();
        const ClockTree phase0_checkpoint = tree.clone();
        const double phase0_area = compute_area_full();
        const double phase0_score =
            weighted_objective_score(model,
                                     phase0_timing,
                                     phase0_area,
                                     baseline,
                                     cfg);
        phase0_timing.path_results.clear();
        phase0_timing.path_results.shrink_to_fit();

        const double portfolio_safety_reserve_seconds =
            testcase_path_count >= cfg.portfolio_large_case_min_paths
                ? std::max(0.0,
                           cfg.portfolio_large_finalization_reserve_seconds)
                : testcase_path_count >= cfg.portfolio_medium_case_min_paths
                      ? std::max(0.0,
                                 cfg.portfolio_medium_finalization_reserve_seconds)
                      : std::max(0.0,
                                 cfg.portfolio_small_finalization_reserve_seconds);
        int launched_routes = 0;
        auto remaining_route_seconds = [&]() {
            return static_cast<double>(cfg.time_limit_seconds) -
                   elapsed_seconds() - portfolio_safety_reserve_seconds;
        };
        auto can_launch_additional_route = [&]() {
            const bool route_slot_available =
                cfg.portfolio_max_routes <= 0 ||
                launched_routes < cfg.portfolio_max_routes;
            return route_slot_available &&
                   (!cfg.portfolio_enforce_shared_global_deadline ||
                    remaining_route_seconds() >=
                        std::max(0.0, cfg.portfolio_min_route_seconds));
        };

        struct PortfolioBranchRun {
            std::string name;
            std::string policy;
            std::string initial_state_policy = "phase0_checkpoint";
            int initial_state_resizes = 0;
            unsigned int initial_state_seed = 0;
            double initial_state_perturb_fraction = 0.0;
            bool reruns_phase0 = false;
            TimingAnalysisResult initial_state_timing;
            double initial_state_area = 0.0;
            bool score_based = false;
            bool alternating = false;
            bool objective_weight_scheduler = false;
            bool diversified = false;
            ClockTree tree;
            OptimizationSummary summary;
        };
        std::vector<PortfolioBranchRun> branch_runs;
        branch_runs.reserve(4);

        auto portfolio_target_score =
            [&](const TimingAnalysisResult& timing, double area) {
                return model.compute_score_metrics(
                                timing,
                                baseline.timing,
                                area,
                                baseline.area,
                                cfg.portfolio_target_alpha,
                                cfg.portfolio_target_beta,
                                cfg.portfolio_target_gamma)
                    .total_score;
            };

        auto summary_is_better =
            [&](const OptimizationSummary& a,
                const OptimizationSummary& b) {
                if (a.success != b.success) return a.success;
                if (a.final_legality.ok != b.final_legality.ok) {
                    return a.final_legality.ok;
                }
                const double a_target =
                    portfolio_target_score(a.final_timing, a.final_area);
                const double b_target =
                    portfolio_target_score(b.final_timing, b.final_area);
                if (a_target > b_target + cfg.score_acceptance_epsilon) {
                    return true;
                }
                if (b_target > a_target + cfg.score_acceptance_epsilon) {
                    return false;
                }
                return a.final_area < b.final_area;
            };
        ClockTree portfolio_best_tree;
        OptimizationSummary portfolio_best_summary;
        size_t portfolio_best_tree_index = 0;
        bool portfolio_best_valid = false;

        auto make_branch_config =
            [&](bool score_based,
                bool alternating,
                bool override_internal_weights,
                bool override_delay_scale,
                bool enable_objective_weight_scheduler) {
                OptimizerConfig branch_cfg = cfg;
                branch_cfg.enable_phase0 = false;
                branch_cfg.enable_phase0_reset_experiment = false;
                branch_cfg.enable_post_phase0_full_pipeline_fork_experiment =
                    false;
                branch_cfg.portfolio_experiment_branch_label.clear();
                branch_cfg.enable_repair_reclaim_score_based_acceptance =
                    score_based;
                branch_cfg.enable_repair_reclaim_alternating_experiment =
                    alternating;
                branch_cfg.enable_score_weight_scheduler =
                    enable_objective_weight_scheduler;
                if (override_internal_weights) {
                    branch_cfg.alpha = cfg.portfolio_midweight_alpha;
                    branch_cfg.beta = cfg.portfolio_midweight_beta;
                    branch_cfg.gamma = cfg.portfolio_midweight_gamma;
                }
                if (override_delay_scale) {
                    const double scale =
                        std::max(0.0,
                                 cfg.portfolio_delay_scale_multiplier);
                    branch_cfg.small_case_phase1a_pulse_delay_scale *= scale;
                    branch_cfg.medium_case_phase1a_pulse_delay_scale *= scale;
                    branch_cfg.large_case_phase1a_pulse_delay_scale *= scale;
                    branch_cfg.phase1a_pulse_delay_scale *= scale;
                }
                if (cfg.portfolio_enforce_shared_global_deadline &&
                    !cfg.portfolio_experiment_disable_time_limits) {
                    const double remaining = remaining_route_seconds();
                    branch_cfg.time_limit_seconds =
                        std::max(1.0, remaining);
                    branch_cfg.repair_reclaim_cycle_end_time_seconds =
                        std::min(
                            branch_cfg.repair_reclaim_cycle_end_time_seconds,
                            static_cast<double>(
                                branch_cfg.time_limit_seconds));
                    branch_cfg.perturb_recover_time_budget_seconds =
                        std::min(
                            branch_cfg.perturb_recover_time_budget_seconds,
                            std::max(
                                0.0,
                                0.25 * branch_cfg.time_limit_seconds));
                }
                if (cfg.portfolio_experiment_disable_time_limits) {
                    constexpr int experiment_time_limit_seconds = 86400;
                    branch_cfg.time_limit_seconds =
                        experiment_time_limit_seconds;
                    branch_cfg.repair_reclaim_cycle_end_time_seconds =
                        static_cast<double>(experiment_time_limit_seconds);
                    branch_cfg.cycle_reclaim_time_budget_seconds = 0.0;
                    branch_cfg.final_alt_time_budget_seconds = 0.0;
                    branch_cfg.final_alt_safety_margin_seconds = 0.0;
                    branch_cfg.perturb_recover_time_budget_seconds = 0.0;
                    branch_cfg.perturb_recover_validation_reserve_seconds =
                        0.0;
                }
                return branch_cfg;
            };

        auto apply_seeded_resize_initial_state =
            [&](ClockTree& candidate_tree,
                PortfolioBranchRun& run,
                PortfolioInitialStateProfile profile,
                double aggressive_fraction,
                unsigned int seed,
                const ClockTree& rollback_tree) {
                const bool pressure_directed =
                    profile ==
                    PortfolioInitialStateProfile::PressureDirectedResize;
                const bool area_shedding =
                    profile ==
                    PortfolioInitialStateProfile::LowPressureAreaShedding;
                const bool aggressive_random =
                    profile ==
                    PortfolioInitialStateProfile::AggressiveRandomResize;
                switch (profile) {
                case PortfolioInitialStateProfile::RandomAdjacentResize:
                    run.initial_state_policy =
                        "seeded_adjacent_original_resize";
                    break;
                case PortfolioInitialStateProfile::PressureDirectedResize:
                    run.initial_state_policy =
                        "pressure_directed_extreme_original_resize";
                    break;
                case PortfolioInitialStateProfile::LowPressureAreaShedding:
                    run.initial_state_policy =
                        "low_pressure_area_shedding_original_resize";
                    break;
                case PortfolioInitialStateProfile::AggressiveRandomResize: {
                    std::ostringstream label;
                    label << "aggressive_random_all_type_resize_"
                          << std::lround(
                                 100.0 * std::max(0.0, aggressive_fraction))
                          << "pct";
                    run.initial_state_policy = label.str();
                    break;
                }
                case PortfolioInitialStateProfile::Phase0Checkpoint:
                    run.initial_state_policy = "phase0_checkpoint";
                    break;
                }
                run.initial_state_seed = seed;
                run.initial_state_perturb_fraction =
                    aggressive_random ? std::max(0.0, aggressive_fraction)
                                      : 0.0;

                std::vector<const BufSpec*> ordered_libs;
                ordered_libs.reserve(libs.size());
                for (const BufSpec& lib : libs) ordered_libs.push_back(&lib);
                std::sort(ordered_libs.begin(), ordered_libs.end(),
                          [](const BufSpec* lhs, const BufSpec* rhs) {
                    const double lhs_delay = average_library_delay(*lhs);
                    const double rhs_delay = average_library_delay(*rhs);
                    if (lhs_delay != rhs_delay) return lhs_delay < rhs_delay;
                    return lhs->name < rhs->name;
                });
                std::unordered_map<std::string, int> type_rank;
                type_rank.reserve(ordered_libs.size());
                for (size_t i = 0; i < ordered_libs.size(); ++i) {
                    type_rank.emplace(ordered_libs[i]->name,
                                      static_cast<int>(i));
                }

                std::vector<ClockNode*> nodes;
                std::function<void(ClockNode*)> collect =
                    [&](ClockNode* node) {
                        if (!node) return;
                        if (node->original && is_buffer_node(node)) {
                            nodes.push_back(node);
                        }
                        for (auto& child : node->children) collect(child.get());
                };
                collect(candidate_tree.root.get());

                const TimingAnalysisResult seed_before_timing =
                    model.analyze_timing(candidate_tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period);
                std::unordered_map<std::string, double> pressure_by_node;
                if (pressure_directed || area_shedding) {
                    const TreeIndexCache tree_index =
                        build_tree_index_cache(candidate_tree);
                    pressure_by_node =
                        compute_endpoint_delta_pressure_indexed(
                            tree_index,
                            seed_before_timing,
                            1.0 / std::max(
                                      std::abs(baseline.timing.ss.wns), 1e-9),
                            1.0 / std::max(
                                      std::abs(baseline.timing.ff.wns), 1e-9));
                    std::sort(nodes.begin(), nodes.end(),
                              [&](const ClockNode* lhs,
                                  const ClockNode* rhs) {
                        const double lhs_pressure =
                            pressure_by_node.count(lhs->name)
                                ? pressure_by_node.at(lhs->name)
                                : 0.0;
                        const double rhs_pressure =
                            pressure_by_node.count(rhs->name)
                                ? pressure_by_node.at(rhs->name)
                                : 0.0;
                        if (area_shedding) {
                            const auto lhs_rank = type_rank.find(lhs->type);
                            const auto rhs_rank = type_rank.find(rhs->type);
                            const double lhs_area =
                                lhs_rank == type_rank.end()
                                    ? 0.0
                                    : model.estimate_buffer_area(
                                          *ordered_libs[static_cast<size_t>(
                                              lhs_rank->second)]);
                            const double rhs_area =
                                rhs_rank == type_rank.end()
                                    ? 0.0
                                    : model.estimate_buffer_area(
                                          *ordered_libs[static_cast<size_t>(
                                              rhs_rank->second)]);
                            const double lhs_value =
                                lhs_area / (1.0 + std::abs(lhs_pressure));
                            const double rhs_value =
                                rhs_area / (1.0 + std::abs(rhs_pressure));
                            if (lhs_value != rhs_value) {
                                return lhs_value > rhs_value;
                            }
                        } else if (std::abs(lhs_pressure) !=
                                   std::abs(rhs_pressure)) {
                            return std::abs(lhs_pressure) >
                                   std::abs(rhs_pressure);
                        }
                        return lhs->name < rhs->name;
                    });
                } else {
                    std::mt19937 rng(run.initial_state_seed);
                    std::shuffle(nodes.begin(), nodes.end(), rng);
                }
                std::mt19937 rng(run.initial_state_seed);
                const int max_distance = std::max(
                    1, cfg.portfolio_seeded_resize_max_type_distance);
                const int requested_moves = aggressive_random
                    ? (nodes.empty() || aggressive_fraction <= 0.0
                           ? 0
                           : std::max(
                                 1,
                                 static_cast<int>(std::ceil(
                                     aggressive_fraction *
                                     static_cast<double>(nodes.size())))))
                    : std::max(0, cfg.portfolio_seeded_resize_moves);
                for (ClockNode* node : nodes) {
                    if (run.initial_state_resizes >= requested_moves) break;
                    const auto current_rank_it = type_rank.find(node->type);
                    if (current_rank_it == type_rank.end()) continue;

                    if (area_shedding) {
                        const BufSpec* best_smaller = nullptr;
                        double best_area = std::numeric_limits<double>::infinity();
                        const double current_area =
                            model.estimate_buffer_area(
                                *ordered_libs[static_cast<size_t>(
                                    current_rank_it->second)]);
                        for (const BufSpec* candidate : ordered_libs) {
                            const int candidate_rank =
                                type_rank.at(candidate->name);
                            if (std::abs(candidate_rank -
                                         current_rank_it->second) >
                                    max_distance ||
                                !local_resize_legality(node, candidate)) {
                                continue;
                            }
                            const double candidate_area =
                                model.estimate_buffer_area(*candidate);
                            if (candidate_area >= current_area -
                                    cfg.area_comparison_epsilon ||
                                candidate_area >= best_area) {
                                continue;
                            }
                            best_smaller = candidate;
                            best_area = candidate_area;
                        }
                        if (best_smaller &&
                            candidate_tree.set_buffer_type(
                                node->name, best_smaller->name)) {
                            ++run.initial_state_resizes;
                        }
                        continue;
                    }

                    if (pressure_directed) {
                        const auto pressure_it =
                            pressure_by_node.find(node->name);
                        const double pressure =
                            pressure_it == pressure_by_node.end()
                                ? 0.0
                                : pressure_it->second;
                        if (std::abs(pressure) <= 1e-12) continue;
                        const int direction = pressure > 0.0 ? 1 : -1;
                        for (int distance = max_distance;
                             distance >= 1;
                             --distance) {
                            const int target_rank =
                                current_rank_it->second +
                                direction * distance;
                            if (target_rank < 0 ||
                                static_cast<size_t>(target_rank) >=
                                    ordered_libs.size()) {
                                continue;
                            }
                            const BufSpec* target =
                                ordered_libs[static_cast<size_t>(target_rank)];
                            if (!local_resize_legality(node, target)) continue;
                            if (candidate_tree.set_buffer_type(node->name,
                                                               target->name)) {
                                ++run.initial_state_resizes;
                            }
                            break;
                        }
                        continue;
                    }

                    std::vector<const BufSpec*> alternatives;
                    alternatives.reserve(2 * static_cast<size_t>(max_distance));
                    for (const BufSpec* lib : ordered_libs) {
                        const auto rank_it = type_rank.find(lib->name);
                        if (!lib || rank_it == type_rank.end() ||
                            lib->name == node->type ||
                            (!aggressive_random &&
                             std::abs(rank_it->second -
                                      current_rank_it->second) >
                                 max_distance) ||
                            !local_resize_legality(node, lib)) {
                            continue;
                        }
                        alternatives.push_back(lib);
                    }
                    if (alternatives.empty()) continue;
                    std::uniform_int_distribution<size_t> pick(
                        0, alternatives.size() - 1);
                    if (candidate_tree.set_buffer_type(
                            node->name, alternatives[pick(rng)]->name)) {
                        ++run.initial_state_resizes;
                    }
                }

                if (run.initial_state_resizes > 0 &&
                    !model.validate_legality(candidate_tree, libs).ok) {
                    candidate_tree = rollback_tree.clone();
                    run.initial_state_policy =
                        "initial_state_resize_rollback_illegal";
                    run.initial_state_resizes = 0;
                }
                run.initial_state_timing =
                    model.analyze_timing(candidate_tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period);
                run.initial_state_area = model.compute_tree_area(candidate_tree);
            };

        auto run_branch =
            [&](const std::string& name,
                const std::string& policy,
                bool score_based,
                bool alternating,
                bool override_internal_weights,
                bool override_delay_scale,
                PortfolioInitialStateProfile initial_state_profile,
                bool rerun_phase0,
                double aggressive_fraction,
                unsigned int initial_state_seed,
                bool enable_objective_weight_scheduler) {
                PortfolioBranchRun run;
                run.name = name;
                run.policy = policy;
                run.score_based = score_based;
                run.alternating = alternating;
                run.objective_weight_scheduler =
                    enable_objective_weight_scheduler;
                run.reruns_phase0 = rerun_phase0;
                run.diversified = initial_state_profile !=
                    PortfolioInitialStateProfile::Phase0Checkpoint;
                run.tree = rerun_phase0
                    ? prephase_diversification_checkpoint.clone()
                    : phase0_checkpoint.clone();
                if (run.diversified) {
                    apply_seeded_resize_initial_state(
                        run.tree,
                        run,
                        initial_state_profile,
                        aggressive_fraction,
                        initial_state_seed,
                        rerun_phase0
                            ? prephase_diversification_checkpoint
                            : phase0_checkpoint);
                } else if (rerun_phase0) {
                    // This route starts from the original, unmodified input
                    // tree and owns its own Phase0 run.  Its entry metrics
                    // are therefore the immutable baseline rather than the
                    // shared Phase0 checkpoint.
                    run.initial_state_policy = "original_tree";
                    run.initial_state_timing =
                        compact_timing_snapshot(baseline.timing);
                    run.initial_state_area = baseline.area;
                } else {
                    run.initial_state_timing = phase0_timing;
                    run.initial_state_area = phase0_area;
                }
                OptimizerConfig branch_cfg =
                    make_branch_config(score_based,
                                       alternating,
                                       override_internal_weights,
                                       override_delay_scale,
                                       enable_objective_weight_scheduler);
                branch_cfg.enable_phase0 = rerun_phase0;
                branch_cfg.portfolio_experiment_branch_label = name;
                Optimizer branch_optimizer(branch_cfg, baseline);
                run.summary =
                    branch_optimizer.optimize(run.tree,
                                              libs,
                                              ss_paths,
                                              ff_paths,
                                              clock_period,
                                              false,
                                              name);

                // A full timing result owns one path record per testcase path.
                // Keeping three complete branches with several hundred cycle
                // snapshots would duplicate millions of path records. Branch
                // selection and reporting only need the aggregate corner
                // metrics, so compact archived snapshots before retaining them.
                auto strip_timing_paths =
                    [](TimingAnalysisResult& timing) {
                        timing.path_results.clear();
                        timing.path_results.shrink_to_fit();
                    };
                auto strip_stage =
                    [&](StageSnapshot& stage) {
                        strip_timing_paths(stage.timing);
                    };
                strip_timing_paths(run.summary.final_timing);
                strip_timing_paths(run.summary.checkpoint.best_timing);
                strip_timing_paths(run.initial_state_timing);
                strip_stage(run.summary.after_phase1a);
                strip_stage(run.summary.after_phase1b);
                strip_stage(run.summary.after_phase2);
                strip_stage(run.summary.after_final_alt);
                strip_stage(run.summary.after_perturb_recover);
                for (StageSnapshot& snapshot :
                     run.summary.phase1a_iteration_snapshots) {
                    strip_stage(snapshot);
                }
                for (StageSnapshot& snapshot :
                     run.summary.phase2_iteration_snapshots) {
                    strip_stage(snapshot);
                }
                for (RepairReclaimCycleRecord& cycle :
                     run.summary.repair_reclaim.cycles) {
                    strip_timing_paths(cycle.before_pulse_timing);
                    strip_timing_paths(cycle.after_pulse_timing);
                    strip_timing_paths(cycle.after_reclaim_timing);
                }
                for (FinalAltIterationRecord& iteration :
                     run.summary.final_alt_iterations) {
                    strip_timing_paths(iteration.before_timing);
                    strip_timing_paths(iteration.after_repair_timing);
                    strip_timing_paths(iteration.after_timing);
                }
                strip_timing_paths(
                    run.summary.perturb_recover.timing_before);
                strip_timing_paths(
                    run.summary.perturb_recover.timing_after);
                for (PerturbRecoverCycleRecord& cycle :
                     run.summary.perturb_recover.cycle_records) {
                    strip_timing_paths(cycle.timing_before);
                    strip_timing_paths(cycle.timing_after);
                }
                run.summary.applied_moves.clear();
                run.summary.applied_moves.shrink_to_fit();

                // Keep exactly one complete tree checkpoint.  Deadline filling
                // can create thousands of fast small-case routes, so retaining
                // a tree per route would turn extra runtime into unbounded RAM.
                const size_t run_index = branch_runs.size();
                const bool replaces_best =
                    !portfolio_best_valid ||
                    summary_is_better(run.summary, portfolio_best_summary);
                if (replaces_best) {
                    portfolio_best_summary = run.summary;
                    portfolio_best_tree = std::move(run.tree);
                    portfolio_best_tree_index = run_index;
                    portfolio_best_valid = true;
                }

                // Route-table selection only needs aggregate results. Preserve
                // detailed stage history once, in portfolio_best_summary.
                run.summary.phase1a_iteration_snapshots.clear();
                run.summary.phase2_iteration_snapshots.clear();
                run.summary.existing_buffer_shared_reclaim_cycles.clear();
                run.summary.repair_reclaim.cycles.clear();
                run.summary.final_alt_iterations.clear();
                run.summary.perturb_recover.cycle_records.clear();
                branch_runs.push_back(std::move(run));
                ++launched_routes;
            };

        // The first route is the non-negotiable fallback: it receives the
        // normal complete remaining Pipeline before any diversification is
        // considered. Later routes exist only when it stopped early enough.
        run_branch("full_score",
                   "complete remaining pipeline; score-based Repair/Reclaim",
                   true,
                   false,
                   false,
                   false,
                   PortfolioInitialStateProfile::Phase0Checkpoint,
                   false,
                   0.0,
                   0U,
                   false);
        if (can_launch_additional_route()) {
            run_branch("full_heuristic",
                       "complete remaining pipeline; heuristic Repair/Reclaim",
                       false,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::Phase0Checkpoint,
                       false,
                       0.0,
                       0U,
                       false);
        }
        if (cfg.portfolio_include_cosine_schedule_branch &&
            retain_prephase_diversification_checkpoint &&
            can_launch_additional_route()) {
            run_branch("cosine_heuristic",
                       "complete full pipeline; rerun Phase0 and heuristic Repair/Reclaim with the area-to-timing cosine objective schedule",
                       false,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::Phase0Checkpoint,
                       true,
                       0.0,
                       0U,
                       true);
        }
        if (cfg.portfolio_include_aggressive_postphase_branches &&
            can_launch_additional_route() &&
            remaining_route_seconds() >=
                cfg.portfolio_seeded_resize_min_remaining_seconds) {
            run_branch("postphase_aggressive_5pct",
                       "complete remaining pipeline; score-based Repair/Reclaim from a 5% all-type perturbed Phase0 state",
                       true,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::AggressiveRandomResize,
                       false,
                       cfg.portfolio_aggressive_perturb_fraction_small,
                       cfg.portfolio_aggressive_perturb_seed + 5U,
                       false);
        }
        if (cfg.portfolio_include_aggressive_postphase_branches &&
            can_launch_additional_route() &&
            remaining_route_seconds() >=
                cfg.portfolio_seeded_resize_min_remaining_seconds) {
            run_branch("postphase_aggressive_10pct",
                       "complete remaining pipeline; score-based Repair/Reclaim from a 10% all-type perturbed Phase0 state",
                       true,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::AggressiveRandomResize,
                       false,
                       cfg.portfolio_aggressive_perturb_fraction_large,
                       cfg.portfolio_aggressive_perturb_seed + 10U,
                       false);
        }
        if (cfg.portfolio_include_aggressive_prephase_branches &&
            retain_prephase_diversification_checkpoint &&
            can_launch_additional_route() &&
            remaining_route_seconds() >=
                cfg.portfolio_seeded_resize_min_remaining_seconds) {
            run_branch("prephase_aggressive_5pct",
                       "complete full pipeline; Phase0 is rerun after a 5% all-type perturbation of the original tree",
                       true,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::AggressiveRandomResize,
                       true,
                       cfg.portfolio_aggressive_perturb_fraction_small,
                       cfg.portfolio_aggressive_perturb_seed + 105U,
                       false);
        }
        if (cfg.portfolio_include_aggressive_prephase_branches &&
            retain_prephase_diversification_checkpoint &&
            can_launch_additional_route() &&
            remaining_route_seconds() >=
                cfg.portfolio_seeded_resize_min_remaining_seconds) {
            run_branch("prephase_aggressive_10pct",
                       "complete full pipeline; Phase0 is rerun after a 10% all-type perturbation of the original tree",
                       true,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::AggressiveRandomResize,
                       true,
                       cfg.portfolio_aggressive_perturb_fraction_large,
                       cfg.portfolio_aggressive_perturb_seed + 110U,
                       false);
        }
        if (cfg.portfolio_include_seeded_resize_branch &&
            can_launch_additional_route() &&
            remaining_route_seconds() >=
                cfg.portfolio_seeded_resize_min_remaining_seconds) {
            run_branch("seeded_resize",
                       "complete remaining pipeline; score-based Repair/Reclaim from a deterministic resized Phase0 state",
                       true,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::RandomAdjacentResize,
                       false,
                       0.0,
                       cfg.portfolio_seeded_resize_random_seed,
                       false);
        }
        if (cfg.portfolio_include_pressure_directed_resize_branch &&
            can_launch_additional_route() &&
            remaining_route_seconds() >=
                cfg.portfolio_seeded_resize_min_remaining_seconds) {
            run_branch("pressure_resize",
                       "complete remaining pipeline; score-based Repair/Reclaim from a pressure-directed resized Phase0 state",
                       true,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::PressureDirectedResize,
                       false,
                       0.0,
                       cfg.portfolio_seeded_resize_random_seed,
                       false);
        }
        if (cfg.portfolio_include_area_shedding_resize_branch &&
            can_launch_additional_route() &&
            remaining_route_seconds() >=
                cfg.portfolio_seeded_resize_min_remaining_seconds) {
            run_branch("area_shedding_resize",
                       "complete remaining pipeline; score-based Repair/Reclaim from a low-pressure area-shedding Phase0 state",
                       true,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::LowPressureAreaShedding,
                       false,
                       0.0,
                       cfg.portfolio_seeded_resize_random_seed,
                       false);
        }
        if (cfg.portfolio_include_midweight_branch &&
            can_launch_additional_route()) {
            run_branch("full_midweight",
                       "complete remaining pipeline; score-based Repair/Reclaim with alternative internal weights",
                       true,
                       false,
                       true,
                       false,
                       PortfolioInitialStateProfile::Phase0Checkpoint,
                       false,
                       0.0,
                       0U,
                       false);
        }
        if (cfg.portfolio_include_delay_scale_branch &&
            can_launch_additional_route()) {
            run_branch("full_delay075",
                       "complete remaining pipeline; score-based Repair/Reclaim with alternative adaptive pulse-delay scales",
                       true,
                       false,
                       false,
                       true,
                       PortfolioInitialStateProfile::Phase0Checkpoint,
                       false,
                       0.0,
                       0U,
                       false);
        }
        if (cfg.portfolio_include_alternating_branch &&
            can_launch_additional_route()) {
            run_branch("alternating_delete",
                       "complete remaining pipeline; alternating heuristic/score Repair/Reclaim with recent-buffer deletion",
                       false,
                       true,
                       false,
                       false,
                       PortfolioInitialStateProfile::Phase0Checkpoint,
                       false,
                       0.0,
                       0U,
                       false);
        }

        // The mandatory fallback and the fixed diverse routes above establish
        // a valid answer first.  Spend only the remaining safe window on fresh
        // deterministic basins.  Profiles intentionally rotate acceptance
        // policy, scheduler use, perturbation strength, stage, and seed.
        int deadline_restart_index = 0;
        while (cfg.portfolio_fill_remaining_time_with_restarts &&
               can_launch_additional_route() &&
               remaining_route_seconds() >=
                   std::max(cfg.portfolio_min_route_seconds,
                            cfg.portfolio_restart_min_remaining_seconds) &&
               (cfg.portfolio_max_restart_routes <= 0 ||
                deadline_restart_index < cfg.portfolio_max_restart_routes)) {
            const bool allow_prephase =
                retain_prephase_diversification_checkpoint;
            const int profile_count = allow_prephase ? 8 : 6;
            const int profile = deadline_restart_index % profile_count;
            const int generation = deadline_restart_index / profile_count;
            const unsigned int seed =
                cfg.portfolio_aggressive_perturb_seed + 1000U +
                static_cast<unsigned int>(deadline_restart_index) * 7919U;
            const bool prephase = allow_prephase && profile >= 6;
            const bool score_based = profile == 0 || profile == 2 ||
                                     profile == 4 || profile == 6;
            const bool scheduled = profile == 3 || profile == 5 ||
                                   profile == 7;
            double fraction = cfg.portfolio_aggressive_perturb_fraction_small;
            if (profile == 1 || profile == 3 || profile == 6) {
                fraction = cfg.portfolio_aggressive_perturb_fraction_large;
            } else if (profile == 2 || profile == 4 || profile == 5 ||
                       profile == 7) {
                fraction = cfg.portfolio_aggressive_perturb_fraction_extra;
            }

            std::ostringstream route_name;
            route_name << "restart_g" << generation << "_p" << profile
                       << "_" << std::lround(100.0 * fraction) << "pct_s"
                       << seed;
            std::ostringstream route_policy;
            route_policy
                << "deadline-filling " << (prephase ? "pre-Phase0" : "post-Phase0")
                << " aggressive restart; "
                << (score_based ? "score-based" : "heuristic")
                << " Repair/Reclaim"
                << (scheduled ? "; cosine objective schedule" : "");
            run_branch(route_name.str(),
                       route_policy.str(),
                       score_based,
                       false,
                       false,
                       false,
                       PortfolioInitialStateProfile::AggressiveRandomResize,
                       prephase,
                       fraction,
                       seed,
                       scheduled);
            ++deadline_restart_index;
        }

        auto branch_is_better =
            [&](size_t lhs, size_t rhs) {
                const OptimizationSummary& a = branch_runs[lhs].summary;
                const OptimizationSummary& b = branch_runs[rhs].summary;
                return summary_is_better(a, b);
            };

        size_t full_fork_winner = 0;
        for (size_t i = 1; i < branch_runs.size(); ++i) {
            if (branch_runs[i].alternating || branch_runs[i].diversified ||
                branch_runs[i].objective_weight_scheduler) {
                continue;
            }
            if (branch_is_better(i, full_fork_winner)) {
                full_fork_winner = i;
            }
        }
        const size_t overall_winner = portfolio_best_tree_index;

        PostPhase0PortfolioSummary portfolio;
        portfolio.enabled = true;
        portfolio.time_limits_disabled =
            cfg.portfolio_experiment_disable_time_limits;
        portfolio.phase0_timing = phase0_timing;
        portfolio.phase0_area = phase0_area;
        portfolio.phase0_score = phase0_score;
        portfolio.target_alpha = cfg.portfolio_target_alpha;
        portfolio.target_beta = cfg.portfolio_target_beta;
        portfolio.target_gamma = cfg.portfolio_target_gamma;
        portfolio.full_fork_winner =
            branch_runs[full_fork_winner].name;
        portfolio.overall_winner =
            branch_runs[overall_winner].name;
        for (const PortfolioBranchRun& run : branch_runs) {
            PostPhase0PortfolioBranchRecord record;
            record.name = run.name;
            record.policy = run.policy;
            record.initial_state_policy = run.initial_state_policy;
            record.initial_state_resizes = run.initial_state_resizes;
            record.initial_state_seed = run.initial_state_seed;
            record.initial_state_perturb_fraction =
                run.initial_state_perturb_fraction;
            record.initial_state_reruns_phase0 = run.reruns_phase0;
            record.initial_state_timing = run.initial_state_timing;
            record.initial_state_area = run.initial_state_area;
            record.repair_reclaim_score_based = run.score_based;
            record.alternating_repair_reclaim = run.alternating;
            record.objective_weight_scheduler =
                run.objective_weight_scheduler;
            record.success = run.summary.success;
            record.legal = run.summary.final_legality.ok;
            record.runtime_seconds = run.summary.runtime_seconds;
            record.final_timing = run.summary.final_timing;
            record.final_area = run.summary.final_area;
            record.final_score = run.summary.final_score;
            record.target_score =
                portfolio_target_score(run.summary.final_timing,
                                       run.summary.final_area);
            record.repair_reclaim_stop_reason =
                run.summary.repair_reclaim.stop_reason;
            record.final_alt_stop_reason =
                run.summary.final_alt.stop_reason;
            record.perturb_recover_stop_reason =
                run.summary.perturb_recover.stop_reason;
            record.repair_reclaim = run.summary.repair_reclaim;
            record.final_alt = run.summary.final_alt;
            record.final_alt_iterations =
                run.summary.final_alt_iterations;
            record.perturb_recover =
                run.summary.perturb_recover;
            portfolio.branches.push_back(std::move(record));
        }
        portfolio.runtime_seconds =
            seconds_since(portfolio_start);

        const bool selected_reruns_phase0 =
            branch_runs[overall_winner].reruns_phase0;
        const double phase0_runtime = summary.phase0.runtime_seconds;
        OptimizationSummary selected = std::move(portfolio_best_summary);
        tree = std::move(portfolio_best_tree);

        // A normal branch starts at the shared Phase0 checkpoint, so its
        // route-local snapshots need the outer Phase0 prefix restored.  A
        // pre-Phase0 diversified branch instead owns a fresh Phase0 run and
        // must retain that independent history unchanged.
        if (!selected_reruns_phase0) {
            auto offset_stage =
                [&](StageSnapshot& stage) {
                    if (stage.valid) {
                        stage.runtime_seconds += phase0_runtime;
                    }
                };
            offset_stage(selected.after_phase1a);
            offset_stage(selected.after_phase1b);
            offset_stage(selected.after_phase2);
            offset_stage(selected.after_final_alt);
            offset_stage(selected.after_perturb_recover);
            selected.phase0_enabled = cfg.enable_phase0;
            selected.phase0_reset_experiment_enabled =
                cfg.enable_phase0_reset_experiment;
            selected.phase0 = summary.phase0;
            selected.runtime_profile.phase0_seconds +=
                summary.runtime_profile.phase0_seconds;
            selected.fast_timing.build_time_seconds +=
                summary.fast_timing.build_time_seconds;
            selected.fast_timing.sync_time_seconds +=
                summary.fast_timing.sync_time_seconds;
            selected.fast_timing.trial_time_seconds +=
                summary.fast_timing.trial_time_seconds;
            selected.applied_moves.insert(
                selected.applied_moves.begin(),
                summary.applied_moves.begin(),
                summary.applied_moves.end());
        }
        selected.post_phase0_portfolio = std::move(portfolio);
        selected.runtime_seconds = elapsed_seconds();
        selected.message =
            "Post-Phase0 portfolio experiment completed; selected " +
            selected.post_phase0_portfolio.overall_winner + ".";
        return selected;
    }

    const int max_iterations_phase1a = cfg.legacy_phase1a_max_iterations;
    const BufSpec* smallest_buffer = choose_smallest_buffer(libs, model);
    if (!smallest_buffer) {
        summary.message = "Optimization skipped: no buffers available in library.";
        return summary;
    }

    const double baseline_total_tns_abs = std::abs(baseline.timing.ss.tns) + std::abs(baseline.timing.ff.tns);
    const double improvement_threshold =
        baseline_total_tns_abs * cfg.legacy_phase1a_improvement_ratio;
    summary.adaptive_repair_reclaim =
        choose_adaptive_repair_reclaim_params(cfg, baseline, summary.phase0);
    const double effective_phase1a_pulse_delay_scale =
        summary.adaptive_repair_reclaim.selected_phase1a_pulse_delay_scale;
    const double effective_reclaim_giveback_ratio =
        summary.adaptive_repair_reclaim.selected_reclaim_giveback_ratio;
    bool active_repair_reclaim_score_acceptance =
        cfg.enable_repair_reclaim_score_based_acceptance;

    std::set<std::string> blacklist;

    auto print_stage_summary = [&](const StageSnapshot& stage) {
        if (!stage.valid) return;
        DEBUG_PRINT("  [" << stage.name << "]"
                    << " added=" << stage.added_buffers
                    << " removed=" << stage.removed_buffers
                    << " downsized=" << stage.downsized_buffers
                    << " runtime=" << stage.runtime_seconds << "s"
                    << " total_violations=" << total_violations(stage.timing)
                    << " | SS TNS=" << stage.timing.ss.tns
                    << " WNS=" << stage.timing.ss.wns
                    << " Violations=" << stage.timing.ss.violating_paths
                    << " | FF TNS=" << stage.timing.ff.tns
                    << " WNS=" << stage.timing.ff.wns
                    << " Violations=" << stage.timing.ff.violating_paths
                    << " | Area=" << stage.area);
    };

    auto capture_stage = [&](StageSnapshot& stage,
                             const std::string& name,
                             int added_buffers,
                             int removed_buffers,
                             int downsized_buffers,
                             const TimingAnalysisResult* cached_timing = nullptr) {
        stage.valid = true;
        stage.name = name;
        stage.added_buffers = added_buffers;
        stage.removed_buffers = removed_buffers;
        stage.downsized_buffers = downsized_buffers;
        stage.runtime_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        TimingAnalysisResult full_timing =
            model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        stage.timing = compact_timing_snapshot(full_timing);
        stage.area = compute_area_full();
        stage.legality = validate_full();
        print_stage_summary(stage);
        verify_cached_timing(name + " snapshot",
                             model,
                             tree,
                             ss_paths,
                             ff_paths,
                             clock_period,
                             cached_timing ? *cached_timing : full_timing);
    };

    auto capture_phase1a_iteration = [&](int iter_index,
                                         const TimingAnalysisResult& timing,
                                         int added_buffers) {
        StageSnapshot snapshot;
        snapshot.valid = true;
        snapshot.name = "Phase 1A iter " + std::to_string(iter_index);
        snapshot.added_buffers = added_buffers;
        snapshot.removed_buffers = 0;
        snapshot.downsized_buffers = 0;
        snapshot.runtime_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        snapshot.timing = compact_timing_snapshot(timing);
        snapshot.area = compute_area_full();
        snapshot.legality = validate_full();
        summary.phase1a_iteration_snapshots.push_back(snapshot);
        print_stage_summary(snapshot);
        verify_cached_timing(snapshot.name + " snapshot",
                             model,
                             tree,
                             ss_paths,
                             ff_paths,
                             clock_period,
                             snapshot.timing);
    };

    auto choose_batch_buffer = [&](double target_delay) -> const BufSpec* {
        const BufSpec* best = nullptr;
        double best_delay = -std::numeric_limits<double>::infinity();
        double best_area = std::numeric_limits<double>::infinity();
        constexpr double eps = 1e-9;

        for (const auto& lib : libs) {
            const double delay = 0.5 * (model.buffer_delay_ss(lib.name, 1) + model.buffer_delay_ff(lib.name, 1));
            if (delay > target_delay + eps) continue;

            const double area = model.estimate_buffer_area(lib);
            if (!best || delay > best_delay + eps || (std::abs(delay - best_delay) <= eps && area + eps < best_area)) {
                best = &lib;
                best_delay = delay;
                best_area = area;
            }
        }

        return best;
    };

    auto choose_repair_buffer_candidates =
        [&](double target_delay,
            int max_types) {
            std::vector<const BufSpec*> candidates;
            const BufSpec* primary = choose_batch_buffer(target_delay);
            if (primary) candidates.push_back(primary);
            if (max_types <= 1) return candidates;

            constexpr double eps = 1e-9;
            std::vector<const BufSpec*> ordered;
            ordered.reserve(libs.size());
            for (const auto& lib : libs) {
                const double delay =
                    0.5 * (model.buffer_delay_ss(lib.name, 1) +
                           model.buffer_delay_ff(lib.name, 1));
                if (delay <= target_delay + eps) {
                    ordered.push_back(&lib);
                }
            }
            std::sort(ordered.begin(),
                      ordered.end(),
                      [&](const BufSpec* lhs, const BufSpec* rhs) {
                          const double lhs_delay =
                              0.5 *
                              (model.buffer_delay_ss(lhs->name, 1) +
                               model.buffer_delay_ff(lhs->name, 1));
                          const double rhs_delay =
                              0.5 *
                              (model.buffer_delay_ss(rhs->name, 1) +
                               model.buffer_delay_ff(rhs->name, 1));
                          if (std::abs(lhs_delay - rhs_delay) > eps) {
                              return lhs_delay > rhs_delay;
                          }
                          const double lhs_area =
                              model.estimate_buffer_area(*lhs);
                          const double rhs_area =
                              model.estimate_buffer_area(*rhs);
                          if (std::abs(lhs_area - rhs_area) > eps) {
                              return lhs_area < rhs_area;
                          }
                          return lhs->name < rhs->name;
                      });
            for (const BufSpec* candidate : ordered) {
                if (!candidate) continue;
                const bool duplicate =
                    std::any_of(candidates.begin(),
                                candidates.end(),
                                [&](const BufSpec* existing) {
                                    return existing &&
                                           existing->name ==
                                               candidate->name;
                                });
                if (duplicate) continue;
                candidates.push_back(candidate);
                if (candidates.size() >=
                    static_cast<size_t>(max_types)) {
                    break;
                }
            }
            if (cfg.final_alt_include_delay_overshoot_types &&
                candidates.size() < static_cast<size_t>(max_types)) {
                std::vector<const BufSpec*> overshoot;
                overshoot.reserve(libs.size());
                for (const auto& lib : libs) {
                    const double delay =
                        0.5 * (model.buffer_delay_ss(lib.name, 1) +
                               model.buffer_delay_ff(lib.name, 1));
                    if (delay > target_delay + eps) {
                        overshoot.push_back(&lib);
                    }
                }
                std::sort(
                    overshoot.begin(),
                    overshoot.end(),
                    [&](const BufSpec* lhs, const BufSpec* rhs) {
                        const double lhs_delay =
                            0.5 *
                            (model.buffer_delay_ss(lhs->name, 1) +
                             model.buffer_delay_ff(lhs->name, 1));
                        const double rhs_delay =
                            0.5 *
                            (model.buffer_delay_ss(rhs->name, 1) +
                             model.buffer_delay_ff(rhs->name, 1));
                        if (std::abs(lhs_delay - rhs_delay) > eps) {
                            return lhs_delay < rhs_delay;
                        }
                        const double lhs_area =
                            model.estimate_buffer_area(*lhs);
                        const double rhs_area =
                            model.estimate_buffer_area(*rhs);
                        if (std::abs(lhs_area - rhs_area) > eps) {
                            return lhs_area < rhs_area;
                        }
                        return lhs->name < rhs->name;
                    });
                for (const BufSpec* candidate : overshoot) {
                    const bool duplicate =
                        std::any_of(candidates.begin(),
                                    candidates.end(),
                                    [&](const BufSpec* existing) {
                                        return existing &&
                                               existing->name ==
                                                   candidate->name;
                                    });
                    if (duplicate) continue;
                    candidates.push_back(candidate);
                    if (candidates.size() >=
                        static_cast<size_t>(max_types)) {
                        break;
                    }
                }
            }
            return candidates;
        };

    auto run_phase1a_pulse = [&](int iter_index,
                                 double delay_scale,
                                 bool record_iteration,
                                 const std::unordered_set<std::string>&
                                     excluded_targets,
                                 int max_targets,
                                 bool shared_path_repair_enabled,
                                 bool endpoint_repair_enabled,
                                 const std::function<bool()>&
                                     bounded_time_up) {
        Phase1APulseResult result;
        result.before_timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        result.after_timing = result.before_timing;
        result.before_area = compute_area_full();
        result.after_area = result.before_area;
        const bool use_fast_insert =
            cfg.enable_fast_timing_engine &&
            fast_timing_engine &&
            sync_fast_timing("Repair pulse insert sync");
        result.fast_timing_current = use_fast_insert;

        if (record_iteration) {
            summary.iterations += 1;
            capture_phase1a_iteration(iter_index, result.before_timing, summary.phase1a_insertions);
        }

        if (result.before_timing.ss.tns == 0.0 && result.before_timing.ff.tns == 0.0) {
            result.stop_reason = "timing_closed";
            result.no_buffers_inserted = true;
            return result;
        }

        std::vector<const TimingPathResult*> ordered_paths;
        ordered_paths.reserve(result.before_timing.path_results.size());
        for (const auto& path : result.before_timing.path_results) {
            ordered_paths.push_back(&path);
        }
        std::sort(ordered_paths.begin(), ordered_paths.end(), [](const TimingPathResult* lhs,
                                                                 const TimingPathResult* rhs) {
            const double lhs_wns = std::min(lhs->setup_slack, lhs->hold_slack);
            const double rhs_wns = std::min(rhs->setup_slack, rhs->hold_slack);
            if (lhs_wns != rhs_wns) return lhs_wns < rhs_wns;
            return lhs->path_name < rhs->path_name;
        });

        std::unordered_map<std::string, double> target_delays;
        std::unordered_set<std::string> used_ffs;
        std::vector<std::string> batch_targets;

        if (endpoint_repair_enabled) {
            for (const TimingPathResult* path_ptr : ordered_paths) {
                if (!path_ptr) continue;
                const TimingPathResult& path = *path_ptr;
                const bool setup_bad = path.setup_slack < 0.0;
                const bool hold_bad = path.hold_slack < 0.0;
                if (!setup_bad && !hold_bad) continue;

                const bool use_setup = setup_bad && (!hold_bad || path.setup_slack <= path.hold_slack);
                const std::string& target_node = use_setup ? path.capture_ff : path.launch_ff;
                const double required_delay = use_setup ? -path.setup_slack : -path.hold_slack;
                if (required_delay <= 0.0) continue;
                if (excluded_targets.count(target_node) != 0) continue;

                auto delay_it = target_delays.find(target_node);
                if (delay_it == target_delays.end()) {
                    target_delays.emplace(target_node, required_delay);
                } else {
                    delay_it->second = std::max(delay_it->second, required_delay);
                }

                if (used_ffs.count(path.launch_ff) == 0 && used_ffs.count(path.capture_ff) == 0) {
                    batch_targets.push_back(target_node);
                    used_ffs.insert(path.launch_ff);
                    used_ffs.insert(path.capture_ff);
                    if (max_targets > 0 &&
                        batch_targets.size() >=
                            static_cast<size_t>(max_targets)) {
                        break;
                    }
                }
            }
        }

        struct SharedRepairCandidate {
            std::string parent_name;
            std::vector<std::string> child_names;
            const BufSpec* buffer = nullptr;
            double estimated_value = 0.0;
        };
        bool any_shared_accepted = false;
        if (shared_path_repair_enabled &&
            !(bounded_time_up && bounded_time_up()) &&
            !is_time_up()) {
            result.shared_path_repair_attempted = true;
            const auto subtree_pressure =
                compute_endpoint_subtree_pressure(tree,
                                                  result.before_timing,
                                                  bounded_time_up);
            std::vector<SharedRepairCandidate> shared_candidates;
            const std::vector<std::string> preorder =
                collect_preorder_names(tree.root.get());
            for (const std::string& parent_name : preorder) {
                if (bounded_time_up && bounded_time_up()) break;
                ClockNode* parent = tree.find_node(parent_name);
                if (!parent || parent->children.size() < 2) continue;
                if (!endpoint_repair_enabled &&
                    (!parent->original || !is_buffer_node(parent))) {
                    continue;
                }
                std::vector<std::pair<double, std::string>> beneficial_children;
                for (const auto& child : parent->children) {
                    if (!child) continue;
                    if (!endpoint_repair_enabled &&
                        (!child->original ||
                         (cfg.existing_shared_children_must_be_buffers &&
                          !is_buffer_node(child.get())))) {
                        continue;
                    }
                    auto pressure_it = subtree_pressure.find(child->name);
                    const double pressure =
                        pressure_it == subtree_pressure.end()
                            ? 0.0
                            : pressure_it->second;
                    if (pressure > cfg.repair_shared_min_edge_pressure) {
                        beneficial_children.emplace_back(pressure,
                                                         child->name);
                    }
                }
                if (beneficial_children.size() < 2) continue;
                std::sort(beneficial_children.begin(),
                          beneficial_children.end(),
                          [](const auto& lhs, const auto& rhs) {
                              if (lhs.first != rhs.first) return lhs.first > rhs.first;
                              return lhs.second < rhs.second;
                          });

                std::vector<SharedRepairCandidate> parent_candidates;
                for (const BufSpec& lib : libs) {
                    const size_t lib_fanout =
                        std::min(lib.ss_delay.size(), lib.ff_delay.size());
                    size_t group_size =
                        std::min(beneficial_children.size(), lib_fanout);
                    if (cfg.repair_shared_max_children > 0) {
                        group_size =
                            std::min(group_size,
                                     static_cast<size_t>(
                                         cfg.repair_shared_max_children));
                    }
                    if (group_size < 2) continue;
                    const size_t parent_new_fanout =
                        parent->children.size() - group_size + 1;
                    if (!parent->type.empty() &&
                        !lib_cache.supports_fanout(parent->buffer_type_id,
                                                   parent_new_fanout)) {
                        continue;
                    }
                    const double inserted_delay =
                        0.5 * (model.buffer_delay_ss(lib.name, group_size) +
                               model.buffer_delay_ff(lib.name, group_size));
                    const double area = model.estimate_buffer_area(lib);
                    double pressure_sum = 0.0;
                    SharedRepairCandidate candidate;
                    candidate.parent_name = parent_name;
                    candidate.buffer = &lib;
                    candidate.child_names.reserve(group_size);
                    for (size_t i = 0; i < group_size; ++i) {
                        pressure_sum += beneficial_children[i].first;
                        candidate.child_names.push_back(
                            beneficial_children[i].second);
                    }
                    candidate.estimated_value =
                        pressure_sum * inserted_delay /
                        std::max(area, cfg.area_comparison_epsilon);
                    parent_candidates.push_back(std::move(candidate));
                }
                std::sort(parent_candidates.begin(),
                          parent_candidates.end(),
                          [](const auto& lhs, const auto& rhs) {
                              if (lhs.estimated_value != rhs.estimated_value) {
                                  return lhs.estimated_value > rhs.estimated_value;
                              }
                              return lhs.buffer->name < rhs.buffer->name;
                          });
                const size_t type_limit =
                    std::min(
                        parent_candidates.size(),
                        static_cast<size_t>(
                            std::max(1,
                                     cfg.repair_shared_types_per_parent)));
                for (size_t i = 0; i < type_limit; ++i) {
                    shared_candidates.push_back(
                        std::move(parent_candidates[i]));
                }
            }
            std::sort(shared_candidates.begin(),
                      shared_candidates.end(),
                      [](const auto& lhs, const auto& rhs) {
                          if (lhs.estimated_value != rhs.estimated_value) {
                              return lhs.estimated_value > rhs.estimated_value;
                          }
                          if (lhs.parent_name != rhs.parent_name) {
                              return lhs.parent_name < rhs.parent_name;
                          }
                          return lhs.buffer->name < rhs.buffer->name;
                      });
            if (cfg.repair_shared_max_candidates_per_pulse > 0 &&
                shared_candidates.size() >
                    static_cast<size_t>(
                        cfg.repair_shared_max_candidates_per_pulse)) {
                shared_candidates.resize(
                    static_cast<size_t>(
                        cfg.repair_shared_max_candidates_per_pulse));
            }

            for (const SharedRepairCandidate& candidate : shared_candidates) {
                if (is_time_up() ||
                    (bounded_time_up && bounded_time_up())) {
                    result.time_limit = true;
                    result.stop_reason =
                        is_time_up() ? "time_limit" : "cycle_time_window";
                    break;
                }
                ClockNode* parent = tree.find_node(candidate.parent_name);
                if (!parent || !candidate.buffer) continue;
                ++result.shared_candidates_tried;
                bool children_still_direct = true;
                for (const std::string& child_name : candidate.child_names) {
                    ClockNode* child = tree.find_node(child_name);
                    children_still_direct =
                        children_still_direct && child && child->parent == parent;
                }
                if (!children_still_direct) continue;

                const TimingAnalysisResult before_shared =
                    use_fast_insert
                        ? fast_timing_engine->timing()
                        : model.analyze_timing(tree,
                                               ss_paths,
                                               ff_paths,
                                               clock_period);
                const double before_shared_area =
                    use_fast_insert
                        ? fast_timing_engine->area()
                        : compute_area_full();
                const std::string buffer_name =
                    tree.generate_unique_name("SHARED_BUF");
                if (!tree.insert_buffer_above_children(
                        candidate.parent_name,
                        candidate.child_names,
                        buffer_name,
                        candidate.buffer->name)) {
                    continue;
                }
                bool timing_ok = true;
                if (use_fast_insert) {
                    timing_ok = sync_fast_timing(
                        "Repair shared-path insertion trial");
                    if (!timing_ok) result.fast_timing_current = false;
                }
                const TimingAnalysisResult after_shared =
                    timing_ok && use_fast_insert
                        ? fast_timing_engine->timing()
                        : model.analyze_timing(tree,
                                               ss_paths,
                                               ff_paths,
                                               clock_period);
                const double after_shared_area =
                    timing_ok && use_fast_insert
                        ? fast_timing_engine->area()
                        : compute_area_full();
                const double before_score =
                    weighted_objective_score(model,
                                             before_shared,
                                             before_shared_area,
                                             baseline,
                                             cfg);
                const double after_score =
                    weighted_objective_score(model,
                                             after_shared,
                                             after_shared_area,
                                             baseline,
                                             cfg);
                const bool ss_not_worse =
                    after_shared.ss.tns >= before_shared.ss.tns;
                const bool ff_not_worse =
                    after_shared.ff.tns >= before_shared.ff.tns;
                const bool ff_closed =
                    std::abs(before_shared.ff.tns) <=
                    cfg.repair_pulse_closed_tns_epsilon;
                const bool ss_strictly_better =
                    after_shared.ss.tns >
                    before_shared.ss.tns +
                        cfg.repair_pulse_required_tns_improvement;
                const bool heuristic_accept =
                    cfg.repair_pulse_require_both_tns_not_worse
                        ? (ss_not_worse && ff_not_worse)
                        : (ff_closed
                               ? (ff_not_worse && ss_strictly_better)
                               : (ss_not_worse || ff_not_worse));
                const bool accepted =
                    timing_ok &&
                    (active_repair_reclaim_score_acceptance
                         ? score_not_worse(after_score,
                                           before_score,
                                           cfg)
                         : heuristic_accept);
                if (!accepted) {
                    tree.delete_buffer(buffer_name);
                    if (use_fast_insert) {
                        if (!sync_fast_timing(
                                "Repair shared-path insertion rollback")) {
                            result.fast_timing_current = false;
                        }
                    }
                    continue;
                }
                any_shared_accepted = true;
                ++result.shared_insertions;
                result.inserted_buffers.push_back(buffer_name);
                result.moves.push_back(
                    "Phase1A shared insert " + buffer_name +
                    " under " + candidate.parent_name +
                    " driving " +
                    std::to_string(candidate.child_names.size()) +
                    " children");
                result.after_timing = after_shared;
                result.after_area = after_shared_area;
            }
        }

        if (batch_targets.empty() && !any_shared_accepted) {
            result.stop_reason = "no_batch_targets";
            result.no_buffers_inserted = true;
            return result;
        }
        result.selected_targets = batch_targets;

        struct PulseBatchAttempt {
            bool accepted = false;
            bool time_limit = false;
            bool had_insertions = false;
            bool worsened = false;
            std::string stop_reason;
            TimingAnalysisResult before_timing;
            TimingAnalysisResult after_timing;
            double before_area = 0.0;
            double after_area = 0.0;
            std::vector<std::string> inserted_buffers;
            std::vector<std::string> moves;
        };

        auto rollback_pulse_buffers =
            [&](const std::vector<std::string>& inserted_buffers,
                const std::string& sync_context) {
                for (auto it = inserted_buffers.rbegin();
                     it != inserted_buffers.rend();
                     ++it) {
                    RemovedBufferState removed;
                    remove_buffer_node(tree, *it, removed);
                }
                if (use_fast_insert && !inserted_buffers.empty()) {
                    if (!sync_fast_timing(sync_context)) {
                        result.fast_timing_current = false;
                    }
                }
            };

        auto attempt_pulse_batch =
            [&](const std::vector<std::string>& targets) {
                PulseBatchAttempt attempt;
                attempt.before_timing =
                    use_fast_insert
                        ? fast_timing_engine->timing()
                        : model.analyze_timing(tree,
                                               ss_paths,
                                               ff_paths,
                                               clock_period);
                attempt.after_timing = attempt.before_timing;
                attempt.before_area =
                    use_fast_insert
                        ? fast_timing_engine->area()
                        : compute_area_full();
                attempt.after_area = attempt.before_area;
                double inserted_area = 0.0;
                FastTimingEngine::Transaction pulse_insert_txn;

                for (const std::string& target_name : targets) {
                    if (is_time_up() ||
                        (bounded_time_up && bounded_time_up())) {
                        attempt.time_limit = true;
                        attempt.stop_reason =
                            is_time_up() ? "time_limit"
                                         : "cycle_time_window";
                        break;
                    }

                    ClockNode* target = tree.find_node(target_name);
                    if (!target || !target->parent) continue;
                    const auto delay_it = target_delays.find(target_name);
                    if (delay_it == target_delays.end()) continue;

                    const double scaled_required_delay =
                        std::max(0.0, delay_scale) * delay_it->second;
                    const BufSpec* chosen_buffer =
                        choose_batch_buffer(scaled_required_delay);
                    if (!chosen_buffer) continue;

                    const std::string parent_name = target->parent->name;
                    const std::string buffer_name =
                        tree.generate_unique_name("NEW_BUF");
                    if (use_fast_insert) {
                        FastTimingTrialSummary trial =
                            fast_timing_engine->trial_insert_between(
                                tree,
                                parent_name,
                                target_name,
                                buffer_name,
                                chosen_buffer->name,
                                pulse_insert_txn);
                        if (!trial.ok ||
                            !fast_timing_engine->commit(tree,
                                                        pulse_insert_txn)) {
                            continue;
                        }
                        attempt.after_timing = trial.timing;
                        attempt.after_area = trial.area;
                    } else {
                        if (!tree.insert_buffer_between(parent_name,
                                                        target_name,
                                                        buffer_name,
                                                        chosen_buffer->name)) {
                            continue;
                        }
                        inserted_area +=
                            model.estimate_buffer_area(*chosen_buffer);
                    }

                    attempt.inserted_buffers.push_back(buffer_name);
                    attempt.moves.push_back(
                        "Phase1A insert " + buffer_name +
                        " above " + target_name);
                }

                attempt.had_insertions =
                    !attempt.inserted_buffers.empty();
                if (attempt.time_limit) {
                    rollback_pulse_buffers(
                        attempt.inserted_buffers,
                        "Repair pulse split time rollback");
                    attempt.after_timing = attempt.before_timing;
                    attempt.after_area = attempt.before_area;
                    return attempt;
                }
                if (!attempt.had_insertions) {
                    attempt.stop_reason = "no_inserted_buffers";
                    return attempt;
                }

                if (!use_fast_insert) {
                    attempt.after_timing =
                        model.analyze_timing(tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period);
                    attempt.after_area =
                        attempt.before_area + inserted_area;
                }
                const double before_score =
                    weighted_objective_score(model,
                                             attempt.before_timing,
                                             attempt.before_area,
                                             baseline,
                                             cfg);
                const double after_score =
                    weighted_objective_score(model,
                                             attempt.after_timing,
                                             attempt.after_area,
                                             baseline,
                                             cfg);
                const bool ss_tns_not_worse =
                    attempt.after_timing.ss.tns >=
                    attempt.before_timing.ss.tns;
                const bool ff_tns_not_worse =
                    attempt.after_timing.ff.tns >=
                    attempt.before_timing.ff.tns;
                const bool ff_tns_closed =
                    std::abs(attempt.before_timing.ff.tns) <=
                    cfg.repair_pulse_closed_tns_epsilon;
                const bool ss_tns_strictly_better =
                    attempt.after_timing.ss.tns >
                    attempt.before_timing.ss.tns +
                        cfg.repair_pulse_required_tns_improvement;
                const bool guarded_or_acceptable =
                    ff_tns_closed
                        ? (ff_tns_not_worse &&
                           ss_tns_strictly_better)
                        : (ss_tns_not_worse ||
                           ff_tns_not_worse);
                const bool tns_acceptable =
                    cfg.repair_pulse_require_both_tns_not_worse
                        ? (ss_tns_not_worse && ff_tns_not_worse)
                        : guarded_or_acceptable;
                attempt.accepted =
                    active_repair_reclaim_score_acceptance
                        ? score_not_worse(after_score,
                                          before_score,
                                          cfg)
                        : tns_acceptable;
                if (!attempt.accepted) {
                    attempt.worsened = true;
                    attempt.stop_reason =
                        active_repair_reclaim_score_acceptance
                            ? "batch_worsened_score"
                            : "batch_worsened_timing";
                    rollback_pulse_buffers(
                        attempt.inserted_buffers,
                        "Repair pulse rejected batch rollback");
                    attempt.after_timing = attempt.before_timing;
                    attempt.after_area = attempt.before_area;
                    attempt.inserted_buffers.clear();
                    attempt.moves.clear();
                }
                return attempt;
            };

        result.inserted_buffers.reserve(batch_targets.size());
        result.moves.reserve(batch_targets.size());
        bool saw_worsened_batch = false;
        std::function<bool(const std::vector<std::string>&, int)>
            try_pulse_batch;
        try_pulse_batch =
            [&](const std::vector<std::string>& targets,
                int split_depth) -> bool {
                if (targets.empty() || result.time_limit) return false;
                ++result.batch_attempts;
                result.max_split_depth_reached =
                    std::max(result.max_split_depth_reached,
                             split_depth);

                PulseBatchAttempt attempt =
                    attempt_pulse_batch(targets);
                if (attempt.time_limit) {
                    result.time_limit = true;
                    result.stop_reason = attempt.stop_reason;
                    return false;
                }
                if (attempt.accepted) {
                    result.inserted_buffers.insert(
                        result.inserted_buffers.end(),
                        attempt.inserted_buffers.begin(),
                        attempt.inserted_buffers.end());
                    result.moves.insert(result.moves.end(),
                                        attempt.moves.begin(),
                                        attempt.moves.end());
                    return true;
                }
                if (!attempt.worsened) return false;

                saw_worsened_batch = true;
                ++result.rejected_batches;
                if (cfg.repair_pulse_split_on_fail &&
                    targets.size() > 1 &&
                    split_depth <
                        cfg.repair_pulse_max_split_depth) {
                    ++result.batch_splits;
                    const size_t mid = targets.size() / 2;
                    const bool accepted_left =
                        try_pulse_batch(
                            std::vector<std::string>(
                                targets.begin(),
                                targets.begin() +
                                    static_cast<std::ptrdiff_t>(mid)),
                            split_depth + 1);
                    const bool accepted_right =
                        !result.time_limit &&
                        try_pulse_batch(
                            std::vector<std::string>(
                                targets.begin() +
                                    static_cast<std::ptrdiff_t>(mid),
                                targets.end()),
                            split_depth + 1);
                    return accepted_left || accepted_right;
                }
                if (cfg.repair_pulse_split_on_fail &&
                    targets.size() > 1 &&
                    split_depth >=
                        cfg.repair_pulse_max_split_depth) {
                    ++result.split_depth_limit_hits;
                }
                return false;
            };

        const bool any_batch_accepted =
            any_shared_accepted ||
            (!batch_targets.empty() &&
             try_pulse_batch(batch_targets, 0));
        if (result.time_limit) {
            rollback_pulse_buffers(
                result.inserted_buffers,
                "Repair pulse complete time rollback");
            result.inserted_buffers.clear();
            result.moves.clear();
            result.inserted_count = 0;
            result.after_timing = result.before_timing;
            result.after_area = result.before_area;
            result.no_buffers_inserted = true;
            return result;
        }

        result.inserted_count =
            static_cast<int>(result.inserted_buffers.size());
        if (!any_batch_accepted || result.inserted_buffers.empty()) {
            result.stop_reason =
                saw_worsened_batch
                    ? (cfg.repair_pulse_split_on_fail
                           ? (active_repair_reclaim_score_acceptance
                                  ? "split_depth_exhausted_score"
                                  : "split_depth_exhausted_timing")
                           : (active_repair_reclaim_score_acceptance
                                  ? "batch_worsened_score"
                                  : "batch_worsened_timing"))
                    : "no_inserted_buffers";
            result.no_buffers_inserted = true;
            return result;
        }

        result.after_timing =
            use_fast_insert
                ? fast_timing_engine->timing()
                : model.analyze_timing(tree,
                                       ss_paths,
                                       ff_paths,
                                       clock_period);
        result.after_area =
            use_fast_insert
                ? fast_timing_engine->area()
                : compute_area_full();
        ++summary.diagnostics.incremental_area_updates;

        verify_incremental_area("accepted Phase1A pulse", result.after_area);
        if (use_fast_insert &&
            !verify_fast_timing_after_commit("accepted Phase1A pulse insertions")) {
            std::abort();
        }
        result.accepted = true;
        return result;
    };

    auto run_pressure_guided_reclaim = [&](RepairReclaimCycleRecord& cycle,
                                           const TimingAnalysisResult& before_pulse_timing,
                                           const TimingAnalysisResult& post_pulse_timing,
                                           double post_pulse_area,
                                           int max_trials,
                                           double local_time_budget_seconds,
                                           const std::unordered_set<std::string>*
                                               protected_deletes,
                                           const std::function<bool()>&
                                               bounded_time_up,
                                           bool fast_timing_current) {
        const auto reclaim_start = std::chrono::steady_clock::now();
        auto reclaim_elapsed = [&]() {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - reclaim_start).count();
        };
        auto reclaim_time_up = [&]() {
            return (local_time_budget_seconds > 0.0 &&
                    reclaim_elapsed() >= local_time_budget_seconds) ||
                   (bounded_time_up && bounded_time_up()) ||
                   is_time_up();
        };

        const double ss_tns_gain =
            std::max(0.0, std::abs(before_pulse_timing.ss.tns) -
                          std::abs(post_pulse_timing.ss.tns));
        const double ff_tns_gain =
            std::max(0.0, std::abs(before_pulse_timing.ff.tns) -
                          std::abs(post_pulse_timing.ff.tns));
        const double allowed_ss_giveback = effective_reclaim_giveback_ratio * ss_tns_gain;
        const double allowed_ff_giveback = effective_reclaim_giveback_ratio * ff_tns_gain;

        const auto pressure_start = std::chrono::steady_clock::now();
        const TimingAnalysisResult pressure_timing =
            post_pulse_timing.path_results.empty()
                ? model.analyze_timing(tree, ss_paths, ff_paths, clock_period)
                : post_pulse_timing;
        bool pressure_timed_out = false;
        auto pressure = compute_endpoint_subtree_pressure(tree,
                                                          pressure_timing,
                                                          reclaim_time_up,
                                                          &pressure_timed_out);
        if (summary.runtime_profile.enabled) {
            summary.runtime_profile.pressure_seconds += seconds_since(pressure_start);
        }
        if (pressure_timed_out) {
            cycle.reclaim_runtime_seconds = reclaim_elapsed();
            return;
        }
        const auto candidate_generation_start = std::chrono::steady_clock::now();
        bool candidate_generation_timed_out = false;
        auto candidates = build_pressure_guided_reclaim_candidates(tree,
                                                                   lib_cache,
                                                                   pressure,
                                                                   cfg,
                                                                   summary.runtime_profile.enabled
                                                                       ? &summary.runtime_profile.reclaim_sorting_seconds
                                                                       : nullptr,
                                                                   reclaim_time_up,
                                                                   &candidate_generation_timed_out);
        if (summary.runtime_profile.enabled) {
            summary.runtime_profile.reclaim_candidate_generation_seconds +=
                seconds_since(candidate_generation_start);
        }
        if (candidate_generation_timed_out) {
            cycle.reclaim_runtime_seconds = reclaim_elapsed();
            return;
        }
        summary.second_round.reclaim_candidates_before_topk +=
            static_cast<int>(candidates.size());
        if (cfg.enable_reclaim_candidate_top_k &&
            cfg.reclaim_candidate_top_k > 0 &&
            candidates.size() >
                static_cast<size_t>(cfg.reclaim_candidate_top_k)) {
            candidates.resize(static_cast<size_t>(cfg.reclaim_candidate_top_k));
        }
        summary.second_round.reclaim_candidates_after_topk +=
            static_cast<int>(candidates.size());
        cycle.reclaim_candidates_built = static_cast<int>(candidates.size());
        cycle.after_reclaim_timing = post_pulse_timing;
        cycle.after_reclaim_area = post_pulse_area;
        if (!cfg.enable_pressure_guided_full_tree_reclaim) {
            cycle.reclaim_runtime_seconds = reclaim_elapsed();
            return;
        }

        if (cfg.enable_fast_timing_engine &&
            fast_timing_engine &&
            (fast_timing_current ||
             sync_fast_timing("Repair/Reclaim sync"))) {
            double current_area = fast_timing_engine->area();
            int consecutive_rejects = 0;
            const auto trial_loop_start = std::chrono::steady_clock::now();
            FastTimingEngine::Transaction reclaim_txn;

            for (const ReclaimCandidate& candidate : candidates) {
                if (reclaim_time_up()) break;
                if (max_trials > 0 &&
                    cycle.reclaim_candidates_tried >= max_trials) {
                    break;
                }
                ++cycle.reclaim_candidates_tried;

                ClockNode* node = tree.find_node(candidate.node_name);
                if (!node || node->is_sink) {
                    ++cycle.reclaim_candidates_rejected;
                    ++consecutive_rejects;
                    continue;
                }

                const TimingAnalysisResult previous_timing =
                    fast_timing_engine->timing();
                FastTimingTrialSummary trial;
                if (candidate.kind == ReclaimCandidateKind::DeleteNewBuffer) {
                    if (protected_deletes &&
                        protected_deletes->count(candidate.node_name) != 0) {
                        ++cycle.reclaim_candidates_rejected;
                        ++consecutive_rejects;
                        continue;
                    }
                    if (!cached_reclaim_delete_ok(node)) {
                        ++cycle.reclaim_candidates_rejected;
                        ++consecutive_rejects;
                        continue;
                    }
                    trial = fast_timing_engine->trial_delete(tree,
                                                             candidate.node_name,
                                                             reclaim_txn);
                } else {
                    trial = fast_timing_engine->trial_resize(tree,
                                                             candidate.node_name,
                                                             candidate.new_type,
                                                             reclaim_txn);
                }

                if (!trial.ok) {
                    ++cycle.reclaim_candidates_rejected;
                    ++consecutive_rejects;
                    continue;
                }

                const double previous_area = current_area;
                const bool area_ok =
                    cfg.reclaim_allow_area_increasing_moves
                        ? trial.area <= previous_area + cfg.area_comparison_epsilon
                        : trial.area < previous_area - cfg.area_comparison_epsilon;
                const bool timing_ok =
                    active_repair_reclaim_score_acceptance
                        ? score_not_worse(
                              weighted_objective_score(model,
                                                       trial.timing,
                                                       trial.area,
                                                       baseline,
                                                       cfg),
                              weighted_objective_score(model,
                                                       previous_timing,
                                                       previous_area,
                                                       baseline,
                                                       cfg),
                              cfg)
                        : timing_within_reclaim_budget(trial.timing,
                                                      post_pulse_timing,
                                                      allowed_ss_giveback,
                                                      allowed_ff_giveback,
                                                      cfg);

                if (area_ok && timing_ok &&
                    fast_timing_engine->commit(tree, reclaim_txn)) {
                    const double area_saved = previous_area - trial.area;
                    current_area = trial.area;
                    ++summary.diagnostics.incremental_area_updates;
                    verify_incremental_area("accepted fast Repair/Reclaim move",
                                            current_area);
                    ++cycle.reclaim_candidates_accepted;
                    cycle.reclaim_area_saved += area_saved;
                    cycle.ss_tns_giveback_used =
                        std::max(cycle.ss_tns_giveback_used,
                                 tns_abs_degradation(trial.timing.ss,
                                                     post_pulse_timing.ss));
                    cycle.ff_tns_giveback_used =
                        std::max(cycle.ff_tns_giveback_used,
                                 tns_abs_degradation(trial.timing.ff,
                                                     post_pulse_timing.ff));
                    consecutive_rejects = 0;

                    std::ostringstream move;
                    if (candidate.kind == ReclaimCandidateKind::DeleteNewBuffer) {
                        ++cycle.newbuf_deletes_accepted;
                        cycle.newbuf_delete_area_saved += area_saved;
                        move << "CycleReclaim delete " << candidate.node_name;
                    } else if (candidate.kind == ReclaimCandidateKind::ResizeOriginal) {
                        ++cycle.original_resizes_accepted;
                        cycle.original_resize_area_saved += area_saved;
                        move << "CycleReclaim resize original " << candidate.node_name
                             << " " << candidate.old_type << " -> " << candidate.new_type;
                    } else {
                        ++cycle.newbuf_resizes_accepted;
                        cycle.newbuf_resize_area_saved += area_saved;
                        move << "CycleReclaim resize NEW_BUF " << candidate.node_name
                             << " " << candidate.old_type << " -> " << candidate.new_type;
                    }
                    move << " area_saved=" << area_saved
                         << " pressure_effect=" << candidate.pressure_effect
                         << " [fast_timing]";
                    summary.applied_moves.push_back(move.str());
                    verify_cached_timing(move.str(),
                                         model,
                                         tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period,
                                         trial.timing);
                    if (!verify_fast_timing_after_commit(move.str())) {
                        summary.early_stopped = true;
                        summary.message =
                            "FastTimingEngine verification failed during Repair/Reclaim.";
                        break;
                    }
                    continue;
                }

                fast_timing_engine->rollback(reclaim_txn);
                ++cycle.reclaim_candidates_rejected;
                ++consecutive_rejects;
                if (cfg.reclaim_max_consecutive_rejects > 0 &&
                    consecutive_rejects >= cfg.reclaim_max_consecutive_rejects) {
                    cycle.reclaim_consecutive_reject_stop = true;
                    break;
                }
            }
            if (summary.runtime_profile.enabled) {
                summary.runtime_profile.reclaim_trial_loop_seconds +=
                    seconds_since(trial_loop_start);
            }

            cycle.after_reclaim_timing = fast_timing_engine->timing();
            cycle.after_reclaim_area = current_area;
            cycle.reclaim_runtime_seconds = reclaim_elapsed();
            return;
        }

        auto ss_arrival = model.compute_clock_arrivals(tree, true);
        auto ff_arrival = model.compute_clock_arrivals(tree, false);
        Phase1bTimingCache reclaim_timing = build_phase1b_timing_cache(ss_paths,
                                                                       ff_paths,
                                                                       ss_arrival,
                                                                       ff_arrival,
                                                                       clock_period);
        double current_area = post_pulse_area;
        int consecutive_rejects = 0;
        const auto trial_loop_start = std::chrono::steady_clock::now();

        for (const ReclaimCandidate& candidate : candidates) {
            if (reclaim_time_up()) break;
            if (max_trials > 0 &&
                cycle.reclaim_candidates_tried >= max_trials) {
                break;
            }
            ++cycle.reclaim_candidates_tried;

            ClockNode* node = tree.find_node(candidate.node_name);
            if (!node || node->is_sink) {
                ++cycle.reclaim_candidates_rejected;
                ++consecutive_rejects;
                continue;
            }

            const double previous_area = current_area;
            const TimingAnalysisResult previous_timing = reclaim_timing.timing;
            ArrivalRollback ss_rollback;
            ArrivalRollback ff_rollback;
            RemovedBufferState removed_state;
            std::string old_type = node->type;
            ClockNode* update_root = node;
            bool applied = false;
            double trial_area = previous_area;

            if (candidate.kind == ReclaimCandidateKind::DeleteNewBuffer) {
                if (protected_deletes &&
                    protected_deletes->count(candidate.node_name) != 0) {
                    ++cycle.reclaim_candidates_rejected;
                    ++consecutive_rejects;
                    continue;
                }
                if (!cached_reclaim_delete_ok(node)) {
                    ++cycle.reclaim_candidates_rejected;
                    ++consecutive_rejects;
                    continue;
                }
                const BufSpec* old_lib = cached_lib(node->type);
                if (!old_lib) {
                    ++cycle.reclaim_candidates_rejected;
                    ++consecutive_rejects;
                    continue;
                }
                trial_area = previous_area - model.estimate_buffer_area(*old_lib);
                update_root = node->parent;
                applied = remove_buffer_node(tree, candidate.node_name, removed_state);
            } else {
                const BufSpec* old_lib = cached_lib(node->type);
                const BufSpec* new_lib = cached_lib(candidate.new_type);
                if (!old_lib || !locally_legal_reclaim_resize(node, new_lib)) {
                    ++cycle.reclaim_candidates_rejected;
                    ++consecutive_rejects;
                    continue;
                }
                trial_area =
                    previous_area - model.estimate_buffer_area(*old_lib) +
                    model.estimate_buffer_area(*new_lib);
                applied = tree.set_buffer_type(candidate.node_name, candidate.new_type);
            }

            if (!applied || !update_root) {
                ++cycle.reclaim_candidates_rejected;
                ++consecutive_rejects;
                continue;
            }

            update_subtree_arrivals(model, update_root, true, ss_arrival, ss_rollback);
            update_subtree_arrivals(model, update_root, false, ff_arrival, ff_rollback);
            std::vector<size_t> affected_groups = affected_path_groups(reclaim_timing, ss_rollback);
            PathTimingRollback path_rollback = update_affected_path_groups(reclaim_timing,
                                                                           affected_groups,
                                                                           ss_arrival,
                                                                           ff_arrival);
            const bool area_ok =
                cfg.reclaim_allow_area_increasing_moves
                    ? trial_area <= previous_area + cfg.area_comparison_epsilon
                    : trial_area < previous_area - cfg.area_comparison_epsilon;
            const bool timing_ok =
                active_repair_reclaim_score_acceptance
                    ? score_not_worse(
                          weighted_objective_score(model,
                                                   reclaim_timing.timing,
                                                   trial_area,
                                                   baseline,
                                                   cfg),
                          weighted_objective_score(model,
                                                   previous_timing,
                                                   previous_area,
                                                   baseline,
                                                   cfg),
                          cfg)
                    : timing_within_reclaim_budget(reclaim_timing.timing,
                                                  post_pulse_timing,
                                                  allowed_ss_giveback,
                                                  allowed_ff_giveback,
                                                  cfg);

            if (area_ok && timing_ok) {
                const double area_saved = previous_area - trial_area;
                current_area = trial_area;
                ++summary.diagnostics.incremental_area_updates;
                verify_incremental_area("accepted Repair/Reclaim move", current_area);
                ++cycle.reclaim_candidates_accepted;
                cycle.reclaim_area_saved += area_saved;
                cycle.ss_tns_giveback_used =
                    std::max(cycle.ss_tns_giveback_used,
                             tns_abs_degradation(reclaim_timing.timing.ss,
                                                 post_pulse_timing.ss));
                cycle.ff_tns_giveback_used =
                    std::max(cycle.ff_tns_giveback_used,
                             tns_abs_degradation(reclaim_timing.timing.ff,
                                                 post_pulse_timing.ff));
                consecutive_rejects = 0;

                std::ostringstream move;
                if (candidate.kind == ReclaimCandidateKind::DeleteNewBuffer) {
                    ++cycle.newbuf_deletes_accepted;
                    cycle.newbuf_delete_area_saved += area_saved;
                    move << "CycleReclaim delete " << candidate.node_name;
                } else if (candidate.kind == ReclaimCandidateKind::ResizeOriginal) {
                    ++cycle.original_resizes_accepted;
                    cycle.original_resize_area_saved += area_saved;
                    move << "CycleReclaim resize original " << candidate.node_name
                         << " " << candidate.old_type << " -> " << candidate.new_type;
                } else {
                    ++cycle.newbuf_resizes_accepted;
                    cycle.newbuf_resize_area_saved += area_saved;
                    move << "CycleReclaim resize NEW_BUF " << candidate.node_name
                         << " " << candidate.old_type << " -> " << candidate.new_type;
                }
                move << " area_saved=" << area_saved
                     << " pressure_effect=" << candidate.pressure_effect;
                summary.applied_moves.push_back(move.str());
                verify_cached_timing(move.str(),
                                     model,
                                     tree,
                                     ss_paths,
                                     ff_paths,
                                     clock_period,
                                     reclaim_timing.timing);
                continue;
            }

            rollback_path_groups(reclaim_timing, path_rollback);
            rollback_arrivals(ss_arrival, ss_rollback);
            rollback_arrivals(ff_arrival, ff_rollback);
            if (candidate.kind == ReclaimCandidateKind::DeleteNewBuffer) {
                restore_buffer_node(tree, removed_state);
            } else {
                tree.set_buffer_type(candidate.node_name, old_type);
            }
            reclaim_timing.timing = previous_timing;
            ++cycle.reclaim_candidates_rejected;
            ++consecutive_rejects;
            if (cfg.reclaim_max_consecutive_rejects > 0 &&
                consecutive_rejects >= cfg.reclaim_max_consecutive_rejects) {
                cycle.reclaim_consecutive_reject_stop = true;
                break;
            }
        }
        if (summary.runtime_profile.enabled) {
            summary.runtime_profile.reclaim_trial_loop_seconds +=
                seconds_since(trial_loop_start);
        }

        cycle.after_reclaim_timing = reclaim_timing.timing;
        cycle.after_reclaim_area = current_area;
        cycle.reclaim_runtime_seconds = reclaim_elapsed();
    };

    // Keep topology-changing sibling grouping separate from endpoint repair.
    // Each cycle may only insert between original buffer nodes, then gives its
    // remaining per-cycle budget to reclaim before endpoint repair resumes.
    auto run_existing_buffer_shared_reclaim_cycle = [&](int cycle_index) {
        RepairReclaimCycleRecord cycle;
        cycle.cycle_index = cycle_index;
        cycle.branch_mode = "existing_buffer_shared";
        cycle.start_time_seconds = elapsed_seconds();
        const auto existing_cycle_start = std::chrono::steady_clock::now();
        auto existing_cycle_time_up = [&]() {
            return is_time_up() ||
                   elapsed_seconds() >= cfg.repair_reclaim_cycle_end_time_seconds ||
                   (cfg.cycle_reclaim_time_budget_seconds > 0.0 &&
                    seconds_since(existing_cycle_start) >=
                        cfg.cycle_reclaim_time_budget_seconds);
        };

        Phase1APulseResult pulse =
            run_phase1a_pulse(-1,
                               effective_phase1a_pulse_delay_scale,
                               false,
                               {},
                               0,
                               true,
                               false,
                               existing_cycle_time_up);
        cycle.before_pulse_timing = pulse.before_timing;
        cycle.before_pulse_area = pulse.before_area;
        cycle.after_pulse_timing = pulse.after_timing;
        cycle.after_pulse_area = pulse.after_area;
        cycle.pulse_inserted_buffers = pulse.inserted_count;
        cycle.pulse_shared_candidates_tried = pulse.shared_candidates_tried;
        cycle.pulse_shared_insertions = pulse.shared_insertions;
        cycle.shared_path_repair_active = true;
        cycle.pulse_batch_attempts = pulse.batch_attempts;
        cycle.pulse_rejected_batches = pulse.rejected_batches;
        cycle.pulse_batch_splits = pulse.batch_splits;
        cycle.pulse_split_depth_limit_hits = pulse.split_depth_limit_hits;
        cycle.pulse_max_split_depth_reached = pulse.max_split_depth_reached;
        cycle.pulse_stop_reason = pulse.stop_reason;
        cycle.after_reclaim_timing = pulse.after_timing;
        cycle.after_reclaim_area = pulse.after_area;

        if (!pulse.time_limit && pulse.accepted &&
            pulse.shared_insertions > 0 &&
            cfg.enable_pressure_guided_full_tree_reclaim &&
            !existing_cycle_time_up()) {
            const double remaining_reclaim_budget =
                cfg.cycle_reclaim_time_budget_seconds > 0.0
                    ? std::max(0.0,
                               cfg.cycle_reclaim_time_budget_seconds -
                                   seconds_since(existing_cycle_start))
                    : 0.0;
            run_pressure_guided_reclaim(cycle,
                                        pulse.before_timing,
                                        pulse.after_timing,
                                        pulse.after_area,
                                        cfg.reclaim_max_trials_per_cycle,
                                        remaining_reclaim_budget,
                                        nullptr,
                                        existing_cycle_time_up,
                                        pulse.fast_timing_current);
        }
        cycle.end_time_seconds = elapsed_seconds();
        finalize_repair_reclaim_cycle_report_fields(
            cycle, effective_reclaim_giveback_ratio);
        if (pulse.accepted) {
            summary.applied_moves.insert(summary.applied_moves.end(),
                                         pulse.moves.begin(),
                                         pulse.moves.end());
        }
        update_best_checkpoint(cycle.after_reclaim_timing,
                               cycle.after_reclaim_area,
                               "existing_buffer_shared_reclaim_" +
                                   std::to_string(cycle_index));
        compact_cycle_record(cycle);
        return cycle;
    };

    int existing_shared_no_insertion_streak = 0;
    bool existing_shared_alternation_active =
        cfg.enable_existing_buffer_shared_reclaim_alternation &&
        !existing_shared_disabled_for_large_case;
    auto observe_existing_shared_result =
        [&](const RepairReclaimCycleRecord& cycle, bool initial_cycle) {
            if (cycle.pulse_shared_insertions > 0) {
                existing_shared_no_insertion_streak = 0;
            } else {
                ++existing_shared_no_insertion_streak;
            }
            const bool initial_failure_stop =
                initial_cycle &&
                cfg.existing_shared_disable_alternation_if_initial_no_insertion &&
                cycle.pulse_shared_insertions == 0;
            const bool streak_stop =
                cfg.existing_shared_max_consecutive_no_insertion_cycles > 0 &&
                existing_shared_no_insertion_streak >=
                    cfg.existing_shared_max_consecutive_no_insertion_cycles;
            if (initial_failure_stop || streak_stop) {
                existing_shared_alternation_active = false;
                summary.existing_buffer_shared_reclaim_disabled_by_no_insertion = true;
                summary.existing_buffer_shared_reclaim_disabled_after_cycle =
                    cycle.cycle_index;
            }
            summary.existing_buffer_shared_final_no_insertion_streak =
                existing_shared_no_insertion_streak;
        };

    if (cfg.enable_phase0_existing_buffer_shared_reclaim_cycle &&
        !existing_shared_disabled_for_large_case &&
        !is_time_up() &&
        elapsed_seconds() < cfg.repair_reclaim_cycle_end_time_seconds) {
        summary.existing_buffer_shared_reclaim_enabled = true;
        RepairReclaimCycleRecord cycle =
            run_existing_buffer_shared_reclaim_cycle(0);
        cycle.objective_alpha = cfg.alpha;
        cycle.objective_beta = cfg.beta;
        cycle.objective_gamma = cfg.gamma;
        observe_existing_shared_result(cycle, true);
        capture_stage(summary.after_existing_buffer_shared_reclaim,
                      "ExistingBufferSharedReclaim",
                      cycle.pulse_inserted_buffers,
                      cycle.newbuf_deletes_accepted,
                      cycle.original_resizes_accepted +
                          cycle.newbuf_resizes_accepted,
                      &cycle.after_reclaim_timing);
        summary.existing_buffer_shared_reclaim_cycle = cycle;
        summary.existing_buffer_shared_reclaim_cycles.push_back(
            std::move(cycle));
    }

    const auto repair_reclaim_profile_start = std::chrono::steady_clock::now();
    if (cfg.enable_repair_reclaim_cycles) {
        RepairReclaimSummary& rr = summary.repair_reclaim;
        rr.enabled = true;
        rr.reclaim_enabled = cfg.enable_pressure_guided_full_tree_reclaim;
        rr.cycle_end_time_seconds = cfg.repair_reclaim_cycle_end_time_seconds;
        rr.max_cycles = cfg.repair_reclaim_max_cycles;
        rr.phase1a_pulse_delay_scale = effective_phase1a_pulse_delay_scale;
        rr.shared_path_repair_enabled = cfg.enable_shared_path_repair;
        rr.shared_max_consecutive_no_insertion_pulses =
            cfg.repair_shared_max_consecutive_no_insertion_pulses;
        rr.pulse_split_on_fail = cfg.repair_pulse_split_on_fail;
        rr.pulse_max_split_depth = cfg.repair_pulse_max_split_depth;
        rr.pulse_failure_recovery_enabled =
            cfg.enable_repair_pulse_failure_recovery;
        rr.pulse_max_consecutive_failures =
            cfg.repair_pulse_max_consecutive_failures;
        rr.pulse_recovery_scale_multiplier =
            cfg.repair_pulse_recovery_scale_multiplier;
        rr.pulse_recovery_min_delay_scale =
            cfg.repair_pulse_recovery_min_delay_scale;
        rr.pulse_recovery_blacklist_targets =
            cfg.repair_pulse_recovery_blacklist_targets;
        rr.reclaim_giveback_ratio = effective_reclaim_giveback_ratio;
        rr.cycle_reclaim_time_budget_seconds = cfg.cycle_reclaim_time_budget_seconds;
        rr.stop_reason = "max_cycles";
        rr.no_progress_stop_enabled = cfg.enable_repair_reclaim_no_progress_stop;
        rr.no_progress_streak_limit = cfg.repair_reclaim_no_progress_streak_limit;

        int no_progress_streak = 0;
        int consecutive_pulse_failures = 0;
        int consecutive_shared_no_insertion_pulses = 0;
        bool shared_path_repair_active = cfg.enable_shared_path_repair;
        double current_pulse_delay_scale =
            effective_phase1a_pulse_delay_scale;
        std::unordered_set<std::string> pulse_recovery_blacklist;
        std::vector<std::string> recently_inserted_buffers;
        for (int cycle_index = 0; cycle_index < cfg.repair_reclaim_max_cycles; ++cycle_index) {
            apply_score_weight_schedule(cycle_index, false);
            if (is_time_up()) {
                summary.early_stopped = true;
                rr.stop_reason = "global_time_budget";
                summary.message = "Optimization stopped early due to time limit in Repair/Reclaim cycles.";
                break;
            }
            if (elapsed_seconds() >= cfg.repair_reclaim_cycle_end_time_seconds) {
                rr.stop_reason = "cycle_time_window";
                break;
            }

            if (cfg.enable_phase0_existing_buffer_shared_reclaim_cycle &&
                existing_shared_alternation_active &&
                cycle_index > 0) {
                RepairReclaimCycleRecord existing_cycle =
                    run_existing_buffer_shared_reclaim_cycle(cycle_index);
                existing_cycle.objective_alpha = cfg.alpha;
                existing_cycle.objective_beta = cfg.beta;
                existing_cycle.objective_gamma = cfg.gamma;
                observe_existing_shared_result(existing_cycle, false);
                summary.existing_buffer_shared_reclaim_cycle = existing_cycle;
                summary.existing_buffer_shared_reclaim_cycles.push_back(
                    std::move(existing_cycle));
                if (is_time_up()) {
                    summary.early_stopped = true;
                    rr.stop_reason = "global_time_budget";
                    break;
                }
                if (elapsed_seconds() >=
                    cfg.repair_reclaim_cycle_end_time_seconds) {
                    rr.stop_reason = "cycle_time_window";
                    break;
                }
            }

            active_repair_reclaim_score_acceptance =
                cfg.enable_repair_reclaim_alternating_experiment
                    ? (cycle_index % 2 == 1)
                    : cfg.enable_repair_reclaim_score_based_acceptance;

            RepairReclaimCycleRecord cycle;
            cycle.cycle_index = cycle_index;
            cycle.objective_alpha = cfg.alpha;
            cycle.objective_beta = cfg.beta;
            cycle.objective_gamma = cfg.gamma;
            cycle.start_time_seconds = elapsed_seconds();
            cycle.branch_mode =
                active_repair_reclaim_score_acceptance
                    ? (cfg.enable_repair_reclaim_alternating_experiment
                           ? "score_delete"
                           : "score")
                    : "heuristic";
            if (!cfg.portfolio_experiment_branch_label.empty() ||
                cfg.enable_repair_reclaim_alternating_experiment) {
                const TimingAnalysisResult pre_cycle_entry_timing =
                    model.analyze_timing(tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period);
                const double pre_cycle_entry_area = compute_area_full();
                cycle.pre_cycle_score_before =
                    weighted_objective_score(model,
                                             pre_cycle_entry_timing,
                                             pre_cycle_entry_area,
                                             baseline,
                                             cfg);
                cycle.pre_cycle_score_after =
                    cycle.pre_cycle_score_before;
            }

            if (cfg.enable_repair_reclaim_alternating_experiment &&
                active_repair_reclaim_score_acceptance) {
                struct RecentDeleteCandidate {
                    std::string name;
                    double area = 0.0;
                };
                std::vector<RecentDeleteCandidate> delete_candidates;
                delete_candidates.reserve(
                    recently_inserted_buffers.size());
                for (const std::string& buffer_name :
                     recently_inserted_buffers) {
                    ++cycle.pre_cycle_delete_attempts;
                    ClockNode* node = tree.find_node(buffer_name);
                    if (!cached_reclaim_delete_ok(node)) continue;
                    const BufSpec* lib =
                        node ? cached_lib(node->type) : nullptr;
                    if (!lib) continue;
                    delete_candidates.push_back(
                        {buffer_name,
                         model.estimate_buffer_area(*lib)});
                }
                std::sort(
                    delete_candidates.begin(),
                    delete_candidates.end(),
                    [](const RecentDeleteCandidate& lhs,
                       const RecentDeleteCandidate& rhs) {
                        if (std::abs(lhs.area - rhs.area) > 1e-12) {
                            return lhs.area > rhs.area;
                        }
                        return lhs.name < rhs.name;
                    });

                const int delete_limit =
                    std::max(
                        0,
                        cfg.repair_reclaim_alternating_delete_recent_limit);
                for (const RecentDeleteCandidate& candidate :
                     delete_candidates) {
                    if (delete_limit > 0 &&
                        cycle.pre_cycle_deletes >= delete_limit) {
                        break;
                    }
                    if (!tree.delete_buffer(candidate.name)) continue;
                    ++cycle.pre_cycle_deletes;
                    cycle.pre_cycle_deleted_area += candidate.area;
                    ++rr.total_pre_cycle_deletes;
                    rr.total_pre_cycle_deleted_area += candidate.area;
                    summary.applied_moves.push_back(
                        "Alternating score-cycle pre-delete " +
                        candidate.name +
                        " area=" + std::to_string(candidate.area));
                }
                if (cycle.pre_cycle_deletes > 0 &&
                    fast_timing_engine) {
                    sync_fast_timing(
                        "alternating score-cycle pre-delete");
                }
                const TimingAnalysisResult perturb_after_timing =
                    model.analyze_timing(tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period);
                const double perturb_after_area = compute_area_full();
                cycle.pre_cycle_score_after =
                    weighted_objective_score(model,
                                             perturb_after_timing,
                                             perturb_after_area,
                                             baseline,
                                             cfg);
                recently_inserted_buffers.clear();
            } else if (
                cfg.enable_repair_reclaim_alternating_experiment) {
                recently_inserted_buffers.clear();
            }

            TimingAnalysisResult cycle_timing =
                model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
            if (cycle_timing.ss.tns == 0.0 && cycle_timing.ff.tns == 0.0) {
                rr.stop_reason = "timing_closed";
                break;
            }

            cycle.before_pulse_timing = cycle_timing;

            const auto pulse_profile_start = std::chrono::steady_clock::now();
            Phase1APulseResult pulse =
                run_phase1a_pulse(cycle_index,
                                   current_pulse_delay_scale,
                                   false,
                                   pulse_recovery_blacklist,
                                   0,
                                   shared_path_repair_active,
                                   true,
                                   [&]() {
                                       return elapsed_seconds() >=
                                              cfg.repair_reclaim_cycle_end_time_seconds;
                                   });
            if (summary.runtime_profile.enabled) {
                summary.runtime_profile.phase1a_pulse_seconds +=
                    seconds_since(pulse_profile_start);
            }
            cycle.before_pulse_timing = pulse.before_timing;
            cycle.before_pulse_area = pulse.before_area;
            cycle.after_pulse_timing = pulse.after_timing;
            cycle.after_pulse_area = pulse.after_area;
            cycle.pulse_inserted_buffers = pulse.inserted_count;
            cycle.pulse_shared_candidates_tried =
                pulse.shared_candidates_tried;
            cycle.pulse_shared_insertions = pulse.shared_insertions;
            cycle.shared_path_repair_active = shared_path_repair_active;
            cycle.pulse_batch_attempts = pulse.batch_attempts;
            cycle.pulse_rejected_batches = pulse.rejected_batches;
            cycle.pulse_batch_splits = pulse.batch_splits;
            cycle.pulse_split_depth_limit_hits =
                pulse.split_depth_limit_hits;
            cycle.pulse_max_split_depth_reached =
                pulse.max_split_depth_reached;
            cycle.phase1a_pulse_delay_scale =
                current_pulse_delay_scale;
            cycle.pulse_recovery_attempt =
                consecutive_pulse_failures > 0;
            cycle.pulse_failure_streak =
                consecutive_pulse_failures;
            cycle.pulse_recovery_blacklist_size =
                static_cast<int>(pulse_recovery_blacklist.size());
            cycle.pulse_stop_reason = pulse.stop_reason;
            cycle.pulse_ss_tns_gain =
                std::max(0.0, std::abs(pulse.before_timing.ss.tns) -
                              std::abs(pulse.after_timing.ss.tns));
            cycle.pulse_ff_tns_gain =
                std::max(0.0, std::abs(pulse.before_timing.ff.tns) -
                              std::abs(pulse.after_timing.ff.tns));
            cycle.after_reclaim_timing = cycle.after_pulse_timing;
            cycle.after_reclaim_area = cycle.after_pulse_area;
            rr.total_pulse_batch_attempts += pulse.batch_attempts;
            rr.total_pulse_rejected_batches += pulse.rejected_batches;
            rr.total_pulse_batch_splits += pulse.batch_splits;
            rr.total_pulse_split_depth_limit_hits +=
                pulse.split_depth_limit_hits;
            rr.max_pulse_split_depth_reached =
                std::max(rr.max_pulse_split_depth_reached,
                         pulse.max_split_depth_reached);

            // Shared sibling grouping has diminishing returns on many trees.
            // Once it repeatedly fails to accept a move, stop paying its
            // full-tree candidate-generation cost for the rest of this stage.
            if (shared_path_repair_active &&
                pulse.shared_path_repair_attempted) {
                if (pulse.shared_insertions > 0) {
                    consecutive_shared_no_insertion_pulses = 0;
                } else {
                    ++consecutive_shared_no_insertion_pulses;
                    if (cfg.repair_shared_max_consecutive_no_insertion_pulses >
                            0 &&
                        consecutive_shared_no_insertion_pulses >=
                            cfg.repair_shared_max_consecutive_no_insertion_pulses) {
                        shared_path_repair_active = false;
                        rr.shared_path_repair_disabled_by_streak = true;
                        rr.shared_path_repair_disabled_after_cycle = cycle_index;
                    }
                }
            }
            cycle.shared_no_insertion_streak =
                consecutive_shared_no_insertion_pulses;
            rr.final_shared_no_insertion_streak =
                consecutive_shared_no_insertion_pulses;

            if (pulse.time_limit) {
                if (pulse.stop_reason == "cycle_time_window") {
                    rr.stop_reason = "cycle_time_window";
                } else {
                    summary.early_stopped = true;
                    rr.stop_reason = "global_time_budget";
                    summary.message = "Optimization stopped early due to time limit in Phase1A pulse.";
                }
                cycle.end_time_seconds = elapsed_seconds();
                finalize_repair_reclaim_cycle_report_fields(cycle,
                                                             effective_reclaim_giveback_ratio);
                compact_cycle_record(cycle);
                rr.cycles.push_back(std::move(cycle));
                break;
            }
            if (!pulse.accepted || pulse.inserted_count == 0) {
                const std::string pulse_failure_reason =
                    pulse.stop_reason.empty()
                        ? "phase1a_pulse_no_insertions"
                        : pulse.stop_reason;
                const bool recoverable_failure =
                    cfg.enable_repair_pulse_failure_recovery &&
                    (pulse_failure_reason ==
                         "split_depth_exhausted_timing" ||
                     pulse_failure_reason ==
                         "split_depth_exhausted_score" ||
                     pulse_failure_reason ==
                         "batch_worsened_timing" ||
                     pulse_failure_reason ==
                         "batch_worsened_score");
                if (recoverable_failure) {
                    ++consecutive_pulse_failures;
                    cycle.pulse_failure_streak =
                        consecutive_pulse_failures;
                    rr.max_consecutive_pulse_failures =
                        std::max(rr.max_consecutive_pulse_failures,
                                 consecutive_pulse_failures);
                }
                cycle.end_time_seconds = elapsed_seconds();
                finalize_repair_reclaim_cycle_report_fields(cycle,
                                                             effective_reclaim_giveback_ratio);
                compact_cycle_record(cycle);
                rr.cycles.push_back(std::move(cycle));

                const bool retry_available =
                    recoverable_failure &&
                    cfg.repair_pulse_max_consecutive_failures > 0 &&
                    consecutive_pulse_failures <
                        cfg.repair_pulse_max_consecutive_failures;
                if (retry_available) {
                    ++rr.total_pulse_recovery_retries;
                    current_pulse_delay_scale =
                        std::max(
                            cfg.repair_pulse_recovery_min_delay_scale,
                            current_pulse_delay_scale *
                                cfg.repair_pulse_recovery_scale_multiplier);
                    int targets_added = 0;
                    for (const std::string& target :
                         pulse.selected_targets) {
                        if (cfg.repair_pulse_recovery_blacklist_targets >
                                0 &&
                            targets_added >=
                                cfg.repair_pulse_recovery_blacklist_targets) {
                            break;
                        }
                        if (pulse_recovery_blacklist.insert(target).second) {
                            ++targets_added;
                        }
                    }
                    continue;
                }
                rr.stop_reason = pulse_failure_reason;
                break;
            }

            if (consecutive_pulse_failures > 0) {
                ++rr.successful_pulse_recoveries;
            }
            consecutive_pulse_failures = 0;
            current_pulse_delay_scale =
                effective_phase1a_pulse_delay_scale;
            pulse_recovery_blacklist.clear();

            summary.phase1a_insertions += pulse.inserted_count;
            summary.applied_moves.insert(summary.applied_moves.end(),
                                         pulse.moves.begin(),
                                         pulse.moves.end());
            rr.total_pulse_inserted_buffers += pulse.inserted_count;
            rr.total_shared_candidates_tried +=
                pulse.shared_candidates_tried;
            rr.total_shared_insertions += pulse.shared_insertions;
            summary.iterations += 1;
            if (cfg.enable_repair_reclaim_alternating_experiment &&
                !active_repair_reclaim_score_acceptance) {
                recently_inserted_buffers =
                    pulse.inserted_buffers;
            }

            if (cfg.enable_pressure_guided_full_tree_reclaim) {
                run_pressure_guided_reclaim(cycle,
                                            pulse.before_timing,
                                            pulse.after_timing,
                                            pulse.after_area,
                                            cfg.reclaim_max_trials_per_cycle,
                                            cfg.cycle_reclaim_time_budget_seconds,
                                            nullptr,
                                            [&]() {
                                                return elapsed_seconds() >=
                                                       cfg.repair_reclaim_cycle_end_time_seconds;
                                            },
                                            pulse.fast_timing_current);
            }

            rr.total_original_resizes += cycle.original_resizes_accepted;
            rr.total_newbuf_resizes += cycle.newbuf_resizes_accepted;
            rr.total_newbuf_deletes += cycle.newbuf_deletes_accepted;
            rr.total_reclaim_area_saved += cycle.reclaim_area_saved;
            cycle.end_time_seconds = elapsed_seconds();
            finalize_repair_reclaim_cycle_report_fields(cycle,
                                                         effective_reclaim_giveback_ratio);
            update_best_checkpoint(cycle.after_reclaim_timing,
                                   cycle.after_reclaim_area,
                                   "repair_reclaim_cycle_" +
                                       std::to_string(cycle_index));
            const double timing_improvement =
                repair_reclaim_progress_cost(cycle.before_pulse_timing) -
                repair_reclaim_progress_cost(cycle.after_reclaim_timing);
            const double area_improvement =
                cycle.before_pulse_area - cycle.after_reclaim_area;
            const bool no_progress =
                cfg.enable_repair_reclaim_no_progress_stop &&
                cycle.pulse_inserted_buffers <=
                    cfg.repair_reclaim_no_progress_max_insertions &&
                area_improvement < cfg.repair_reclaim_min_area_improvement &&
                timing_improvement <
                    cfg.repair_reclaim_min_timing_cost_improvement &&
                cycle.net_cycle_area_delta >=
                    -cfg.repair_reclaim_min_area_improvement;
            if (no_progress) {
                ++no_progress_streak;
            } else {
                no_progress_streak = 0;
            }
            rr.final_no_progress_streak = no_progress_streak;
            compact_cycle_record(cycle);
            rr.cycles.push_back(std::move(cycle));
            if (cfg.enable_repair_reclaim_no_progress_stop &&
                cfg.repair_reclaim_no_progress_streak_limit > 0 &&
                no_progress_streak >=
                    cfg.repair_reclaim_no_progress_streak_limit) {
                rr.stop_reason = "no_progress_streak";
                rr.no_progress_stop_triggered = true;
                ++summary.diagnostics.repair_reclaim_no_progress_stops;
                break;
            }
        }

        finalize_repair_reclaim_summary_report_fields(rr);
        if (rr.stop_reason == "max_cycles" &&
            elapsed_seconds() >= cfg.repair_reclaim_cycle_end_time_seconds) {
            rr.stop_reason = "cycle_time_window";
        }
        if (rr.cycles.empty() && rr.stop_reason == "max_cycles") {
            rr.stop_reason = "not_run";
        }
        summary.phase1a_stop_reason = "repair_reclaim_cycles_" + rr.stop_reason;
        capture_stage(summary.after_phase1a,
                      "RepairReclaimCycles",
                      summary.phase1a_insertions,
                      rr.total_newbuf_deletes +
                          rr.total_pre_cycle_deletes,
                      rr.total_original_resizes + rr.total_newbuf_resizes);
    } else {

    // Phase 1A: Iterative Target-Driven Independent Batching (Coarse Optimization)
    summary.phase1a_stop_reason = "max_iterations";
    for (int iter1a = 0; iter1a < max_iterations_phase1a; ++iter1a) {
        if (is_time_up()) {
            summary.early_stopped = true;
            summary.phase1a_stop_reason = "time_limit";
            summary.message = "Optimization stopped early due to time limit in Phase 1A.";
            break;
        }

        TimingAnalysisResult current_timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        summary.iterations += 1;
        capture_phase1a_iteration(iter1a, current_timing, summary.phase1a_insertions);

        if (current_timing.ss.tns == 0.0 && current_timing.ff.tns == 0.0) {
            summary.phase1a_stop_reason = "timing_closed";
            break;
        }

        std::vector<const TimingPathResult*> ordered_paths;
        ordered_paths.reserve(current_timing.path_results.size());
        for (const auto& path : current_timing.path_results) {
            ordered_paths.push_back(&path);
        }
        std::sort(ordered_paths.begin(), ordered_paths.end(), [](const TimingPathResult* lhs, const TimingPathResult* rhs) {
            const double lhs_wns = std::min(lhs->setup_slack, lhs->hold_slack);
            const double rhs_wns = std::min(rhs->setup_slack, rhs->hold_slack);
            if (lhs_wns != rhs_wns) return lhs_wns < rhs_wns;
            return lhs->path_name < rhs->path_name;
        });

        std::unordered_map<std::string, double> target_delays;
        std::unordered_set<std::string> used_ffs;
        std::vector<std::string> batch_targets;

        for (const TimingPathResult* path_ptr : ordered_paths) {
            if (!path_ptr) continue;
            const TimingPathResult& path = *path_ptr;
            const bool setup_bad = path.setup_slack < 0.0;
            const bool hold_bad = path.hold_slack < 0.0;
            if (!setup_bad && !hold_bad) continue;

            const bool use_setup = setup_bad && (!hold_bad || path.setup_slack <= path.hold_slack);
            const std::string& target_node = use_setup ? path.capture_ff : path.launch_ff;
            const double required_delay = use_setup ? -path.setup_slack : -path.hold_slack;
            if (required_delay <= 0.0) continue;

            auto delay_it = target_delays.find(target_node);
            if (delay_it == target_delays.end()) {
                target_delays.emplace(target_node, required_delay);
            } else {
                delay_it->second = std::max(delay_it->second, required_delay);
            }

            if (used_ffs.count(path.launch_ff) == 0 && used_ffs.count(path.capture_ff) == 0) {
                batch_targets.push_back(target_node);
                used_ffs.insert(path.launch_ff);
                used_ffs.insert(path.capture_ff);
            }
        }

        if (batch_targets.empty()) {
            summary.phase1a_stop_reason = "no_batch_targets";
            break;
        }

        std::vector<std::string> inserted_buffers;
        inserted_buffers.reserve(batch_targets.size());
        std::vector<std::string> batch_moves;
        batch_moves.reserve(batch_targets.size());
        int batch_insertion_count = 0;

        for (const std::string& target_name : batch_targets) {
            if (is_time_up()) {
                summary.early_stopped = true;
                summary.phase1a_stop_reason = "time_limit";
                summary.message = "Optimization stopped early due to time limit in Phase 1A batch insertion.";
                break;
            }

            ClockNode* target = tree.find_node(target_name);
            if (!target || !target->parent) continue;

            const auto delay_it = target_delays.find(target_name);
            if (delay_it == target_delays.end()) continue;

            const BufSpec* chosen_buffer = choose_batch_buffer(delay_it->second);
            if (!chosen_buffer) continue;

            const std::string parent_name = target->parent->name;
            const std::string buffer_name = tree.generate_unique_name("NEW_BUF");
            if (!tree.insert_buffer_between(parent_name, target_name, buffer_name, chosen_buffer->name)) {
                continue;
            }

            inserted_buffers.push_back(buffer_name);
            batch_moves.push_back("Phase1A insert " + buffer_name + " above " + target_name);
            batch_insertion_count += 1;
        }

        if (inserted_buffers.empty()) {
            if (summary.phase1a_stop_reason != "time_limit") {
                summary.phase1a_stop_reason = "no_inserted_buffers";
            }
            break;
        }

        TimingAnalysisResult batch_timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);

        if (batch_timing.ss.tns < current_timing.ss.tns || batch_timing.ff.tns < current_timing.ff.tns) {
            for (const std::string& buffer_name : inserted_buffers) {
                RemovedBufferState removed;
                remove_buffer_node(tree, buffer_name, removed);
            }
            summary.phase1a_stop_reason = "batch_worsened_timing";
            summary.message = "Phase 1A batch worsened a corner TNS; reverting and handing off to Phase 1B.";
            break;
        }

        summary.phase1a_insertions += batch_insertion_count;
        summary.applied_moves.insert(summary.applied_moves.end(), batch_moves.begin(), batch_moves.end());

        double tns_improvement = (batch_timing.ss.tns - current_timing.ss.tns) +
                                 (batch_timing.ff.tns - current_timing.ff.tns);
        if (tns_improvement > 0.0 && tns_improvement < improvement_threshold) {
            summary.phase1a_stop_reason = "plateau";
            summary.message = "Phase 1A plateaued (TNS improvement < 0.1% of baseline); handing off to Phase 1B.";
            break;
        }

        if (current_timing.ss.tns == 0.0 && current_timing.ff.tns == 0.0) {
            summary.phase1a_stop_reason = "timing_closed";
            break;
        }
    }

    capture_stage(summary.after_phase1a, "Phase1A", summary.phase1a_insertions, 0, 0);
    }
    if (summary.runtime_profile.enabled) {
        summary.runtime_profile.repair_reclaim_seconds +=
            cfg.enable_repair_reclaim_cycles
                ? seconds_since(repair_reclaim_profile_start)
                : 0.0;
    }

    const bool use_final_alt =
        cfg.enable_final_alternating_greedy &&
        cfg.final_alt_replace_phase1b_and_phase2;

    // FinalAlt is the closure-oriented tail of the Pipeline.  If a scheduled
    // score is enabled, finish its ramp before entering this stage so its
    // target checkpoint and any optional score acceptance use the timing-heavy
    // endpoint rather than an arbitrary last Repair/Reclaim-cycle weight.
    apply_score_weight_schedule(0, true);

    if (use_final_alt) {
        const auto final_alt_profile_start = std::chrono::steady_clock::now();
        FinalAlternatingGreedySummary& final_alt = summary.final_alt;
        final_alt.stop_reason = "not_started";
        final_alt.area_before = compute_area_full();
        TimingAnalysisResult final_alt_before_timing =
            model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        final_alt.ss_tns_before = final_alt_before_timing.ss.tns;
        final_alt.ss_wns_before = final_alt_before_timing.ss.wns;
        final_alt.ss_violations_before = final_alt_before_timing.ss.violating_paths;
        final_alt.ff_tns_before = final_alt_before_timing.ff.tns;
        final_alt.ff_violations_before = final_alt_before_timing.ff.violating_paths;

        auto final_alt_ss_arrival = model.compute_clock_arrivals(tree, true);
        auto final_alt_ff_arrival = model.compute_clock_arrivals(tree, false);
        Phase1bTimingCache final_alt_timing =
            build_phase1b_timing_cache(ss_paths,
                                       ff_paths,
                                       final_alt_ss_arrival,
                                       final_alt_ff_arrival,
                                       clock_period);
        double final_alt_area = final_alt.area_before;
        auto final_alt_target_score =
            [&](const TimingAnalysisResult& timing, double area) {
                return model.compute_score_metrics(
                                timing,
                                baseline.timing,
                                area,
                                baseline.area,
                                cfg.perturb_recover_checkpoint_alpha,
                                cfg.perturb_recover_checkpoint_beta,
                                cfg.perturb_recover_checkpoint_gamma)
                    .total_score;
            };
        ClockTree final_alt_target_best_tree;
        TimingAnalysisResult final_alt_target_best_timing =
            final_alt_timing.timing;
        double final_alt_target_best_area = final_alt_area;
        double final_alt_target_best_score =
            final_alt_target_score(final_alt_timing.timing,
                                   final_alt_area);
        size_t final_alt_target_best_move_log_size =
            summary.applied_moves.size();
        int final_alt_target_best_insertions = 0;
        int final_alt_target_best_reclaim = 0;
        int final_alt_target_best_deletes = 0;
        int final_alt_target_best_resizes = 0;
        int final_alt_target_best_phase1b_insertions =
            summary.phase1b_insertions;
        int final_alt_target_best_phase2_removals =
            summary.phase2_removals;
        int final_alt_target_best_phase2_downsizes =
            summary.phase2_downsizes;
        final_alt.target_score_before =
            final_alt_target_best_score;
        if (cfg.enable_final_alt_target_checkpoint) {
            final_alt_target_best_tree = tree.clone();
        }
        auto update_final_alt_target_checkpoint =
            [&](int iteration) {
                if (!cfg.enable_final_alt_target_checkpoint) return;
                const double score =
                    final_alt_target_score(final_alt_timing.timing,
                                           final_alt_area);
                if (!score_strictly_better(
                        score,
                        final_alt_target_best_score,
                        cfg) &&
                    !(std::abs(score - final_alt_target_best_score) <=
                          cfg.score_acceptance_epsilon &&
                      final_alt_area <
                          final_alt_target_best_area -
                              cfg.area_comparison_epsilon)) {
                    return;
                }
                final_alt_target_best_tree = tree.clone();
                final_alt_target_best_timing = final_alt_timing.timing;
                final_alt_target_best_area = final_alt_area;
                final_alt_target_best_score = score;
                final_alt_target_best_move_log_size =
                    summary.applied_moves.size();
                final_alt_target_best_insertions =
                    final_alt.repair_insertions;
                final_alt_target_best_reclaim =
                    final_alt.reclaim_moves_accepted;
                final_alt_target_best_deletes =
                    final_alt.newbuf_deletes;
                final_alt_target_best_resizes = final_alt.resizes;
                final_alt_target_best_phase1b_insertions =
                    summary.phase1b_insertions;
                final_alt_target_best_phase2_removals =
                    summary.phase2_removals;
                final_alt_target_best_phase2_downsizes =
                    summary.phase2_downsizes;
                final_alt.target_checkpoint_best_iteration = iteration;
            };
        std::set<std::string> final_alt_blacklist;
        std::unordered_map<std::string, int>
            final_alt_blacklist_expiry;
        std::unordered_map<std::string, int>
            final_alt_target_failures;
        int current_final_alt_iter = 0;
        auto expire_final_alt_blacklist = [&]() {
            if (cfg.final_alt_blacklist_cooldown_iters <= 0) return;
            for (auto it = final_alt_blacklist_expiry.begin();
                 it != final_alt_blacklist_expiry.end();) {
                if (it->second <= current_final_alt_iter) {
                    final_alt_blacklist.erase(it->first);
                    it = final_alt_blacklist_expiry.erase(it);
                } else {
                    ++it;
                }
            }
        };
        auto blacklist_final_alt_target =
            [&](const std::string& target_name) {
                if (target_name.empty()) return;
                final_alt_blacklist.insert(target_name);
                const int failures =
                    ++final_alt_target_failures[target_name];
                if (cfg.final_alt_blacklist_cooldown_iters > 0) {
                    const bool permanently_blacklisted =
                        cfg.final_alt_max_failures_per_target > 0 &&
                        failures >=
                            cfg.final_alt_max_failures_per_target;
                    if (permanently_blacklisted) {
                        final_alt_blacklist_expiry.erase(target_name);
                    } else {
                        final_alt_blacklist_expiry[target_name] =
                            current_final_alt_iter +
                            cfg.final_alt_blacklist_cooldown_iters + 1;
                    }
                }
            };
        auto clear_final_alt_target_failures =
            [&](const std::string& target_name) {
                final_alt_blacklist.erase(target_name);
                final_alt_blacklist_expiry.erase(target_name);
                final_alt_target_failures.erase(target_name);
            };
        bool final_alt_fast_current =
            cfg.enable_fast_timing_engine &&
            fast_timing_engine &&
            sync_fast_timing("FinalAlt initial sync");
        bool perturb_recover_active = false;
        bool perturb_recover_started = false;
        std::chrono::steady_clock::time_point perturb_recover_start;
        auto verify_final_alt_full_validation = [&](const std::string& context) {
            if (!cfg.enable_final_alt_trial_full_validation_verify) return;
            ++summary.diagnostics.full_legality_validations_final_alt_trials;
            const LegalityReport legality = validate_full();
            if (!legality.ok) {
                std::cerr << "FinalAlt full validation failed after "
                          << context << '\n';
                for (const auto& issue : legality.issues) {
                    std::cerr << "  - " << issue << '\n';
                }
                std::abort();
            }
        };

        auto final_alt_time_up = [&]() {
            if (is_time_up()) return true;
            if (perturb_recover_enabled) {
                const double validation_reserve =
                    effective_finalization_reserve_seconds;
                if (perturb_recover_active) {
                    if (elapsed_seconds() + validation_reserve >=
                        cfg.time_limit_seconds) {
                        return true;
                    }
                    if (perturb_recover_started &&
                        cfg.perturb_recover_time_budget_seconds > 0.0 &&
                        seconds_since(perturb_recover_start) >=
                            cfg.perturb_recover_time_budget_seconds) {
                        return true;
                    }
                } else {
                    const double stage_reserve =
                        validation_reserve +
                        std::max(
                            0.0,
                            cfg.perturb_recover_time_budget_seconds);
                    if (elapsed_seconds() + stage_reserve >=
                        cfg.time_limit_seconds) {
                        return true;
                    }
                }
            } else if (effective_finalization_reserve_seconds > 0.0 &&
                       elapsed_seconds() +
                               effective_finalization_reserve_seconds >=
                           cfg.time_limit_seconds) {
                return true;
            }
            const double safety_margin =
                std::max(0.0, cfg.final_alt_safety_margin_seconds);
            if (elapsed_seconds() + safety_margin >= cfg.time_limit_seconds) {
                return true;
            }
            if (cfg.final_alt_time_budget_seconds > 0.0 &&
                seconds_since(final_alt_profile_start) >=
                    cfg.final_alt_time_budget_seconds) {
                return true;
            }
            return false;
        };

        auto final_alt_insert_repair = [&](int max_insertions,
                                           FinalAltIterationRecord* iter_record) {
            int inserted_count = 0;
            const bool use_fast_insert = final_alt_fast_current;
            FastTimingEngine::Transaction repair_insert_txn;
            for (int local_iter = 0; local_iter < max_insertions; ++local_iter) {
                if (final_alt_time_up()) break;
                if (!final_alt_needs_repair(final_alt_timing.timing, cfg)) break;

                WorstPathChoice choice;
                if (use_fast_insert) {
                    const FastTimingWorstViolation violation =
                        fast_timing_engine->worst_violation(final_alt_blacklist);
                    choice.valid = violation.valid;
                    choice.target_node = violation.target_node;
                    choice.path_name =
                        violation.launch_ff + "->" + violation.capture_ff;
                    choice.violation_slack = violation.slack;
                    choice.is_setup = violation.is_setup;
                } else {
                    choice = choose_worst_target(final_alt_timing.timing,
                                                 final_alt_blacklist);
                }
                if (!choice.valid) break;
                if (iter_record) ++iter_record->repair_attempts;

                ClockNode* target = tree.find_node(choice.target_node);
                if (!target || !target->parent) {
                    blacklist_final_alt_target(choice.target_node);
                    if (iter_record) ++iter_record->repair_rejections;
                    continue;
                }

                const std::string parent_name = target->parent->name;
                const std::string buffer_name = tree.generate_unique_name("NEW_BUF");
                const double required_delay = -choice.violation_slack;
                const BufSpec* adaptive_buffer = choose_batch_buffer(required_delay);
                if (!adaptive_buffer) adaptive_buffer = smallest_buffer;
                if (use_fast_insert &&
                    cfg.final_alt_repair_types_per_target > 1) {
                    std::vector<const BufSpec*> repair_types =
                        choose_repair_buffer_candidates(
                            required_delay,
                            cfg.final_alt_repair_types_per_target);
                    if (repair_types.empty() && smallest_buffer) {
                        repair_types.push_back(smallest_buffer);
                    }

                    const TimingAnalysisResult selection_baseline_timing =
                        final_alt_timing.timing;
                    const double selection_baseline_score =
                        weighted_objective_score(
                            model,
                            selection_baseline_timing,
                            final_alt_area,
                            baseline,
                            cfg);
                    const BufSpec* best_buffer = nullptr;
                    double best_objective =
                        -std::numeric_limits<double>::infinity();
                    double best_area =
                        std::numeric_limits<double>::infinity();
                    constexpr double timing_eps = 1e-9;

                    for (const BufSpec* candidate_buffer :
                         repair_types) {
                        if (!candidate_buffer) continue;
                        FastTimingTrialSummary candidate_trial =
                            fast_timing_engine->trial_insert_between(
                                tree,
                                parent_name,
                                choice.target_node,
                                buffer_name,
                                candidate_buffer->name,
                                repair_insert_txn);
                        if (!candidate_trial.ok) continue;

                        const bool all_not_worse =
                            candidate_trial.timing.ss.wns >=
                                selection_baseline_timing.ss.wns -
                                    timing_eps &&
                            candidate_trial.timing.ff.wns >=
                                selection_baseline_timing.ff.wns -
                                    timing_eps &&
                            candidate_trial.timing.ss.tns >=
                                selection_baseline_timing.ss.tns -
                                    timing_eps &&
                            candidate_trial.timing.ff.tns >=
                                selection_baseline_timing.ff.tns -
                                    timing_eps;
                        const bool any_improved =
                            candidate_trial.timing.ss.wns >
                                selection_baseline_timing.ss.wns +
                                    timing_eps ||
                            candidate_trial.timing.ff.wns >
                                selection_baseline_timing.ff.wns +
                                    timing_eps ||
                            candidate_trial.timing.ss.tns >
                                selection_baseline_timing.ss.tns +
                                    timing_eps ||
                            candidate_trial.timing.ff.tns >
                                selection_baseline_timing.ff.tns +
                                    timing_eps;
                        const double candidate_score =
                            weighted_objective_score(
                                model,
                                candidate_trial.timing,
                                candidate_trial.area,
                                baseline,
                                cfg);
                        const bool acceptable =
                            cfg.enable_final_alt_score_based_acceptance
                                ? score_strictly_better(
                                      candidate_score,
                                      selection_baseline_score,
                                      cfg)
                                : (all_not_worse && any_improved);
                        const double objective =
                            cfg.enable_final_alt_score_based_acceptance
                                ? candidate_score
                                : -final_alt_timing_cost(
                                      candidate_trial.timing);
                        if (acceptable &&
                            (objective > best_objective + timing_eps ||
                             (std::abs(objective - best_objective) <=
                                  timing_eps &&
                              candidate_trial.area <
                                  best_area -
                                      cfg.area_comparison_epsilon))) {
                            best_buffer = candidate_buffer;
                            best_objective = objective;
                            best_area = candidate_trial.area;
                        }
                        fast_timing_engine->rollback(
                            repair_insert_txn);
                    }
                    if (!best_buffer) {
                        blacklist_final_alt_target(
                            choice.target_node);
                        if (iter_record) {
                            ++iter_record->repair_rejections;
                        }
                        continue;
                    }
                    adaptive_buffer = best_buffer;
                }
                if (use_fast_insert) {
                    const TimingAnalysisResult previous_timing =
                        final_alt_timing.timing;
                    FastTimingTrialSummary trial =
                        fast_timing_engine->trial_insert_between(tree,
                                                                 parent_name,
                                                                 choice.target_node,
                                                                 buffer_name,
                                                                 adaptive_buffer->name,
                                                                 repair_insert_txn);
                    if (!trial.ok) {
                        blacklist_final_alt_target(choice.target_node);
                        if (iter_record) ++iter_record->repair_rejections;
                        continue;
                    }

                    constexpr double timing_eps = 1e-9;
                    const bool all_not_worse =
                        trial.timing.ss.wns >= previous_timing.ss.wns - timing_eps &&
                        trial.timing.ff.wns >= previous_timing.ff.wns - timing_eps &&
                        trial.timing.ss.tns >= previous_timing.ss.tns - timing_eps &&
                        trial.timing.ff.tns >= previous_timing.ff.tns - timing_eps;
                    const bool any_improved =
                        trial.timing.ss.wns > previous_timing.ss.wns + timing_eps ||
                        trial.timing.ff.wns > previous_timing.ff.wns + timing_eps ||
                        trial.timing.ss.tns > previous_timing.ss.tns + timing_eps ||
                        trial.timing.ff.tns > previous_timing.ff.tns + timing_eps;
                    const bool repair_acceptable =
                        cfg.enable_final_alt_score_based_acceptance
                            ? score_strictly_better(
                                  weighted_objective_score(model,
                                                           trial.timing,
                                                           trial.area,
                                                           baseline,
                                                           cfg),
                                  weighted_objective_score(model,
                                                           previous_timing,
                                                           final_alt_area,
                                                           baseline,
                                                           cfg),
                                  cfg)
                            : (all_not_worse && any_improved);

                    if (repair_acceptable &&
                        fast_timing_engine->commit(tree, repair_insert_txn)) {
                        clear_final_alt_target_failures(
                            choice.target_node);
                        summary.phase1b_insertions += 1;
                        final_alt.repair_insertions += 1;
                        inserted_count += 1;
                        if (iter_record) ++iter_record->repair_insertions;
                        final_alt_area = trial.area;
                        final_alt_timing.timing = trial.timing;
                        ++summary.diagnostics.incremental_area_updates;
                        verify_incremental_area("accepted fast FinalAlt repair insert",
                                                final_alt_area);
                        verify_final_alt_full_validation("accepted fast FinalAlt repair insert");
                        const std::string move =
                            "FinalAlt repair insert " + buffer_name +
                            " above " + choice.target_node +
                            " from path " + choice.path_name +
                            (choice.is_setup ? " [setup]" : " [hold]") +
                            " [fast_timing]";
                        summary.applied_moves.push_back(move);
                        verify_cached_timing(move,
                                             model,
                                             tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period,
                                             final_alt_timing.timing);
                        if (!verify_fast_timing_after_commit(move)) {
                            std::abort();
                        }
                        continue;
                    }

                    fast_timing_engine->rollback(repair_insert_txn);
                    blacklist_final_alt_target(choice.target_node);
                    if (iter_record) ++iter_record->repair_rejections;
                    continue;
                }
                if (!tree.insert_buffer_between(parent_name,
                                                choice.target_node,
                                                buffer_name,
                                                adaptive_buffer->name)) {
                    blacklist_final_alt_target(choice.target_node);
                    if (iter_record) ++iter_record->repair_rejections;
                    continue;
                }

                ClockNode* inserted_buffer = tree.find_node(buffer_name);
                ArrivalRollback ss_rollback;
                ArrivalRollback ff_rollback;
                update_subtree_arrivals(model,
                                        inserted_buffer,
                                        true,
                                        final_alt_ss_arrival,
                                        ss_rollback);
                update_subtree_arrivals(model,
                                        inserted_buffer,
                                        false,
                                        final_alt_ff_arrival,
                                        ff_rollback);

                std::vector<size_t> affected_groups =
                    affected_path_groups(final_alt_timing, ss_rollback);
                const TimingAnalysisResult previous_timing =
                    final_alt_timing.timing;
                const TimingCornerResult previous_ss =
                    final_alt_timing.timing.ss;
                const TimingCornerResult previous_ff =
                    final_alt_timing.timing.ff;
                PathTimingRollback path_rollback =
                    update_affected_path_groups(final_alt_timing,
                                                affected_groups,
                                                final_alt_ss_arrival,
                                                final_alt_ff_arrival);

                constexpr double timing_eps = 1e-9;
                const bool all_not_worse =
                    final_alt_timing.timing.ss.wns >= previous_ss.wns - timing_eps &&
                    final_alt_timing.timing.ff.wns >= previous_ff.wns - timing_eps &&
                    final_alt_timing.timing.ss.tns >= previous_ss.tns - timing_eps &&
                    final_alt_timing.timing.ff.tns >= previous_ff.tns - timing_eps;
                const bool any_improved =
                    final_alt_timing.timing.ss.wns > previous_ss.wns + timing_eps ||
                    final_alt_timing.timing.ff.wns > previous_ff.wns + timing_eps ||
                    final_alt_timing.timing.ss.tns > previous_ss.tns + timing_eps ||
                    final_alt_timing.timing.ff.tns > previous_ff.tns + timing_eps;
                const double trial_area =
                    final_alt_area +
                    model.estimate_buffer_area(*adaptive_buffer);
                const bool repair_acceptable =
                    cfg.enable_final_alt_score_based_acceptance
                        ? score_strictly_better(
                              weighted_objective_score(model,
                                                       final_alt_timing.timing,
                                                       trial_area,
                                                       baseline,
                                                       cfg),
                              weighted_objective_score(model,
                                                       previous_timing,
                                                       final_alt_area,
                                                       baseline,
                                                       cfg),
                              cfg)
                        : (all_not_worse && any_improved);

                if (repair_acceptable) {
                    clear_final_alt_target_failures(
                        choice.target_node);
                    summary.phase1b_insertions += 1;
                    final_alt.repair_insertions += 1;
                    inserted_count += 1;
                    if (iter_record) ++iter_record->repair_insertions;
                    final_alt_area = trial_area;
                    ++summary.diagnostics.incremental_area_updates;
                    verify_incremental_area("accepted FinalAlt repair insert",
                                            final_alt_area);
                    verify_final_alt_full_validation("accepted FinalAlt repair insert");
                    const std::string move =
                        "FinalAlt repair insert " + buffer_name +
                        " above " + choice.target_node +
                        " from path " + choice.path_name +
                        (choice.is_setup ? " [setup]" : " [hold]");
                    summary.applied_moves.push_back(move);
                    verify_cached_timing(move,
                                         model,
                                         tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period,
                                         final_alt_timing.timing);
                    continue;
                }

                rollback_path_groups(final_alt_timing, path_rollback);
                rollback_arrivals(final_alt_ss_arrival, ss_rollback);
                rollback_arrivals(final_alt_ff_arrival, ff_rollback);
                RemovedBufferState removed;
                if (remove_buffer_node(tree, buffer_name, removed)) {
                    blacklist_final_alt_target(choice.target_node);
                }
                if (iter_record) ++iter_record->repair_rejections;
            }
            if (use_fast_insert && inserted_count > 0) {
                final_alt_ss_arrival = model.compute_clock_arrivals(tree, true);
                final_alt_ff_arrival = model.compute_clock_arrivals(tree, false);
                final_alt_timing =
                    build_phase1b_timing_cache(ss_paths,
                                               ff_paths,
                                               final_alt_ss_arrival,
                                               final_alt_ff_arrival,
                                               clock_period);
                final_alt_area = fast_timing_engine->area();
            }
            if (!use_fast_insert && inserted_count > 0) {
                final_alt_fast_current = false;
            }
            return inserted_count;
        };

        bool final_alt_use_fast_reclaim = false;
        FastTimingEngine::Transaction final_alt_reclaim_txn;

        auto final_alt_try_downsize_to = [&](const std::string& node_name,
                                             const std::string& requested_new_type,
                                             const TimingAnalysisResult& reclaim_baseline,
                                             double allowed_cost_giveback) {
            ClockNode* node = tree.find_node(node_name);
            if (!is_buffer_node(node)) return false;

            const std::string old_type = node->type;
            const BufSpec* old_lib = cached_lib(old_type);
            if (!old_lib) return false;
            const BufSpec* smaller = nullptr;
            if (!requested_new_type.empty()) {
                if (requested_new_type == old_type) return false;
                smaller = cached_lib(requested_new_type);
                if (!smaller ||
                    !is_physically_smaller(*smaller, *old_lib, model)) {
                    return false;
                }
            } else {
                smaller =
                    choose_next_smaller_buffer(libs,
                                               model,
                                               old_type,
                                               node->children.size());
            }
            if (!smaller) return false;
            ++summary.diagnostics.local_legality_checks_final_alt;
            if (!local_resize_legality(node, smaller)) return false;

            if (final_alt_use_fast_reclaim &&
                fast_timing_engine) {
                FastTimingTrialSummary trial =
                    fast_timing_engine->trial_resize(tree,
                                                     node_name,
                                                     smaller->name,
                                                     final_alt_reclaim_txn);
                if (!trial.ok) return false;

                const TimingAnalysisResult previous_timing =
                    final_alt_timing.timing;
                const double previous_area = final_alt_area;
                const bool area_ok =
                    !cfg.final_alt_reclaim_area_decrease_only ||
                    trial.area < previous_area - cfg.area_comparison_epsilon;
                const bool timing_ok =
                    cfg.enable_final_alt_score_based_acceptance
                        ? score_not_worse(
                              weighted_objective_score(model,
                                                       trial.timing,
                                                       trial.area,
                                                       baseline,
                                                       cfg),
                              weighted_objective_score(model,
                                                       previous_timing,
                                                       previous_area,
                                                       baseline,
                                                       cfg),
                              cfg)
                        : final_alt_within_reclaim_budget(
                              trial.timing,
                              reclaim_baseline,
                              allowed_cost_giveback,
                              cfg.final_alt_reclaim_hard_wns_guard);
                if (area_ok && timing_ok &&
                    fast_timing_engine->commit(tree,
                                               final_alt_reclaim_txn)) {
                    final_alt_area = trial.area;
                    final_alt_timing.timing = trial.timing;
                    ++summary.diagnostics.incremental_area_updates;
                    verify_incremental_area("accepted fast FinalAlt downsize",
                                            final_alt_area);
                    verify_final_alt_full_validation("accepted fast FinalAlt downsize");
                    summary.phase2_downsizes += 1;
                    final_alt.reclaim_moves_accepted += 1;
                    final_alt.resizes += 1;
                    const std::string move =
                        "FinalAlt greedy-downsize " + node_name +
                        " to " + smaller->name + " [fast_timing]";
                    summary.applied_moves.push_back(move);
                    verify_cached_timing(move,
                                         model,
                                         tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period,
                                         final_alt_timing.timing);
                    if (!verify_fast_timing_after_commit(move)) {
                        std::abort();
                    }
                    return true;
                }

                fast_timing_engine->rollback(final_alt_reclaim_txn);
                return false;
            }

            const TimingAnalysisResult previous_timing = final_alt_timing.timing;
            const double previous_area = final_alt_area;
            const double trial_area =
                previous_area - model.estimate_buffer_area(*old_lib) +
                model.estimate_buffer_area(*smaller);
            if (!tree.set_buffer_type(node_name, smaller->name)) return false;

            ArrivalRollback ss_rollback;
            ArrivalRollback ff_rollback;
            update_subtree_arrivals(model,
                                    node,
                                    true,
                                    final_alt_ss_arrival,
                                    ss_rollback);
            update_subtree_arrivals(model,
                                    node,
                                    false,
                                    final_alt_ff_arrival,
                                    ff_rollback);

            ArrivalRollback affected_rollback;
            affected_rollback.old_values = ss_rollback.old_values;
            affected_rollback.old_values.insert(affected_rollback.old_values.end(),
                                                ff_rollback.old_values.begin(),
                                                ff_rollback.old_values.end());
            std::vector<size_t> affected_groups =
                affected_path_groups(final_alt_timing, affected_rollback);
            PathTimingRollback path_rollback =
                update_affected_path_groups(final_alt_timing,
                                            affected_groups,
                                            final_alt_ss_arrival,
                                            final_alt_ff_arrival);

            const bool area_ok =
                !cfg.final_alt_reclaim_area_decrease_only ||
                trial_area < previous_area - cfg.area_comparison_epsilon;
            const bool timing_ok =
                cfg.enable_final_alt_score_based_acceptance
                    ? score_not_worse(
                          weighted_objective_score(model,
                                                   final_alt_timing.timing,
                                                   trial_area,
                                                   baseline,
                                                   cfg),
                          weighted_objective_score(model,
                                                   previous_timing,
                                                   previous_area,
                                                   baseline,
                                                   cfg),
                          cfg)
                    : final_alt_within_reclaim_budget(
                          final_alt_timing.timing,
                          reclaim_baseline,
                          allowed_cost_giveback,
                          cfg.final_alt_reclaim_hard_wns_guard);

            if (area_ok && timing_ok) {
                final_alt_area = trial_area;
                ++summary.diagnostics.incremental_area_updates;
                verify_incremental_area("accepted FinalAlt downsize",
                                        final_alt_area);
                verify_final_alt_full_validation("accepted FinalAlt downsize");
                summary.phase2_downsizes += 1;
                final_alt.reclaim_moves_accepted += 1;
                final_alt.resizes += 1;
                const std::string move =
                    "FinalAlt greedy-downsize " + node_name +
                    " to " + smaller->name;
                summary.applied_moves.push_back(move);
                verify_cached_timing(move,
                                     model,
                                     tree,
                                     ss_paths,
                                     ff_paths,
                                     clock_period,
                                     final_alt_timing.timing);
                return true;
            }

            rollback_path_groups(final_alt_timing, path_rollback);
            rollback_arrivals(final_alt_ss_arrival, ss_rollback);
            rollback_arrivals(final_alt_ff_arrival, ff_rollback);
            tree.set_buffer_type(node_name, old_type);
            final_alt_timing.timing = previous_timing;
            final_alt_area = previous_area;
            return false;
        };

        auto final_alt_try_downsize = [&](const std::string& node_name,
                                          const TimingAnalysisResult& reclaim_baseline,
                                          double allowed_cost_giveback) {
            return final_alt_try_downsize_to(node_name,
                                             std::string(),
                                             reclaim_baseline,
                                             allowed_cost_giveback);
        };

        auto final_alt_try_delete = [&](const std::string& node_name,
                                        const TimingAnalysisResult& reclaim_baseline,
                                        double allowed_cost_giveback) {
            ClockNode* node = tree.find_node(node_name);
            if (!node || node->original || !node->parent) return false;
            ++summary.diagnostics.local_legality_checks_final_alt;
            if (!cached_reclaim_delete_ok(node)) return false;
            const BufSpec* old_lib = cached_lib(node->type);
            if (!old_lib) return false;

            if (final_alt_use_fast_reclaim &&
                fast_timing_engine) {
                FastTimingTrialSummary trial =
                    fast_timing_engine->trial_delete(tree,
                                                     node_name,
                                                     final_alt_reclaim_txn);
                if (!trial.ok) return false;

                const TimingAnalysisResult previous_timing =
                    final_alt_timing.timing;
                const double previous_area = final_alt_area;
                const bool area_ok =
                    !cfg.final_alt_reclaim_area_decrease_only ||
                    trial.area < previous_area - cfg.area_comparison_epsilon;
                const bool timing_ok =
                    cfg.enable_final_alt_score_based_acceptance
                        ? score_not_worse(
                              weighted_objective_score(model,
                                                       trial.timing,
                                                       trial.area,
                                                       baseline,
                                                       cfg),
                              weighted_objective_score(model,
                                                       previous_timing,
                                                       previous_area,
                                                       baseline,
                                                       cfg),
                              cfg)
                        : final_alt_within_reclaim_budget(
                              trial.timing,
                              reclaim_baseline,
                              allowed_cost_giveback,
                              cfg.final_alt_reclaim_hard_wns_guard);
                if (area_ok && timing_ok &&
                    fast_timing_engine->commit(tree,
                                               final_alt_reclaim_txn)) {
                    final_alt_area = trial.area;
                    final_alt_timing.timing = trial.timing;
                    ++summary.diagnostics.incremental_area_updates;
                    verify_incremental_area("accepted fast FinalAlt delete",
                                            final_alt_area);
                    verify_final_alt_full_validation("accepted fast FinalAlt delete");
                    summary.phase2_removals += 1;
                    final_alt.reclaim_moves_accepted += 1;
                    final_alt.newbuf_deletes += 1;
                    const std::string move =
                        "FinalAlt remove " + node_name + " [fast_timing]";
                    summary.applied_moves.push_back(move);
                    verify_cached_timing(move,
                                         model,
                                         tree,
                                         ss_paths,
                                         ff_paths,
                                         clock_period,
                                         final_alt_timing.timing);
                    if (!verify_fast_timing_after_commit(move)) {
                        std::abort();
                    }
                    return true;
                }

                fast_timing_engine->rollback(final_alt_reclaim_txn);
                return false;
            }

            ClockNode* parent = node->parent;
            const TimingAnalysisResult previous_timing = final_alt_timing.timing;
            const double previous_area = final_alt_area;
            const double trial_area =
                previous_area - model.estimate_buffer_area(*old_lib);

            RemovedBufferState removed;
            if (!remove_buffer_node(tree, node_name, removed)) return false;

            ArrivalRollback ss_rollback;
            ArrivalRollback ff_rollback;
            update_subtree_arrivals(model,
                                    parent,
                                    true,
                                    final_alt_ss_arrival,
                                    ss_rollback);
            update_subtree_arrivals(model,
                                    parent,
                                    false,
                                    final_alt_ff_arrival,
                                    ff_rollback);

            ArrivalRollback affected_rollback;
            affected_rollback.old_values = ss_rollback.old_values;
            affected_rollback.old_values.insert(affected_rollback.old_values.end(),
                                                ff_rollback.old_values.begin(),
                                                ff_rollback.old_values.end());
            std::vector<size_t> affected_groups =
                affected_path_groups(final_alt_timing, affected_rollback);
            PathTimingRollback path_rollback =
                update_affected_path_groups(final_alt_timing,
                                            affected_groups,
                                            final_alt_ss_arrival,
                                            final_alt_ff_arrival);

            const bool area_ok =
                !cfg.final_alt_reclaim_area_decrease_only ||
                trial_area < previous_area - cfg.area_comparison_epsilon;
            const bool timing_ok =
                cfg.enable_final_alt_score_based_acceptance
                    ? score_not_worse(
                          weighted_objective_score(model,
                                                   final_alt_timing.timing,
                                                   trial_area,
                                                   baseline,
                                                   cfg),
                          weighted_objective_score(model,
                                                   previous_timing,
                                                   previous_area,
                                                   baseline,
                                                   cfg),
                          cfg)
                    : final_alt_within_reclaim_budget(
                          final_alt_timing.timing,
                          reclaim_baseline,
                          allowed_cost_giveback,
                          cfg.final_alt_reclaim_hard_wns_guard);

            if (area_ok && timing_ok) {
                final_alt_area = trial_area;
                ++summary.diagnostics.incremental_area_updates;
                verify_incremental_area("accepted FinalAlt delete",
                                        final_alt_area);
                verify_final_alt_full_validation("accepted FinalAlt delete");
                summary.phase2_removals += 1;
                final_alt.reclaim_moves_accepted += 1;
                final_alt.newbuf_deletes += 1;
                const std::string move = "FinalAlt remove " + node_name;
                summary.applied_moves.push_back(move);
                verify_cached_timing(move,
                                     model,
                                     tree,
                                     ss_paths,
                                     ff_paths,
                                     clock_period,
                                     final_alt_timing.timing);
                return true;
            }

            rollback_path_groups(final_alt_timing, path_rollback);
            rollback_arrivals(final_alt_ss_arrival, ss_rollback);
            rollback_arrivals(final_alt_ff_arrival, ff_rollback);
            restore_buffer_node(tree, removed);
            final_alt_timing.timing = previous_timing;
            final_alt_area = previous_area;
            return false;
        };

        auto build_final_alt_reclaim_candidates = [&]() {
            constexpr double eps = 1e-9;
            const double area_weight = cfg.final_alt_rank_area_weight;
            const double timing_help_weight =
                cfg.final_alt_rank_timing_help_weight;
            const double timing_harm_weight =
                cfg.final_alt_rank_timing_harm_weight;
            const double delete_bonus = cfg.final_alt_rank_delete_bonus;
            if (final_alt_time_up()) {
                return std::vector<FinalAltReclaimCandidate>{};
            }

            std::unordered_map<std::string, double> pressure;
            if (cfg.enable_indexed_timing_paths) {
                const auto index_start = std::chrono::steady_clock::now();
                TreeIndexCache tree_index = build_tree_index_cache(tree);
                summary.second_round.tree_index_rebuilds += 1;
                summary.second_round.tree_index_rebuild_time_seconds +=
                    seconds_since(index_start);
                pressure =
                    compute_endpoint_delta_pressure_indexed(tree_index,
                                                            final_alt_timing.timing,
                                                            1.0,
                                                            1.0,
                                                            final_alt_time_up);
            } else {
                pressure =
                    compute_endpoint_subtree_pressure(tree,
                                                      final_alt_timing.timing,
                                                      final_alt_time_up);
            }
            if (final_alt_time_up()) {
                return std::vector<FinalAltReclaimCandidate>{};
            }

            std::vector<std::string> preorder =
                collect_preorder_names(tree.root.get());
            std::unordered_map<std::string, int> node_order;
            node_order.reserve(preorder.size());
            for (size_t i = 0; i < preorder.size(); ++i) {
                node_order.emplace(preorder[i], static_cast<int>(i));
            }

            std::vector<FinalAltReclaimCandidate> candidates;
            candidates.reserve(preorder.size() * 2);

            size_t visited_reclaim_nodes = 0;
            for (const std::string& node_name : preorder) {
                if ((visited_reclaim_nodes++ & 255U) == 0U &&
                    final_alt_time_up()) {
                    return std::vector<FinalAltReclaimCandidate>{};
                }
                ClockNode* node = tree.find_node(node_name);
                if (!is_buffer_node(node)) continue;
                const BufSpec* current_lib = cached_lib(node->type);
                if (!current_lib) continue;

                const size_t fanout = node->children.size();
                const double current_area =
                    model.estimate_buffer_area(*current_lib);
                const double current_delay =
                    buffer_nominal_delay_from_lib(*current_lib, fanout);
                const double node_pressure = [&]() {
                    auto it = pressure.find(node->name);
                    return it == pressure.end() ? 0.0 : it->second;
                }();
                const int order = node_order.count(node->name)
                    ? node_order[node->name]
                    : std::numeric_limits<int>::max();

                if (!node->original) {
                    FinalAltReclaimCandidate candidate;
                    candidate.kind = FinalAltReclaimCandidateKind::DeleteNewBuffer;
                    candidate.node_name = node->name;
                    candidate.node_order = order;
                    candidate.old_type_id = lib_cache.get_type_id(node->type);
                    candidate.area_save = current_area;
                    candidate.pressure = node_pressure;
                    candidate.delay_delta = -current_delay;
                    candidate.pressure_effect =
                        candidate.pressure * candidate.delay_delta;
                    candidate.score =
                        candidate.area_save * area_weight +
                        std::max(0.0, candidate.pressure_effect) *
                            timing_help_weight -
                        std::max(0.0, -candidate.pressure_effect) *
                            timing_harm_weight +
                        delete_bonus;
                    candidates.push_back(candidate);
                }

                const std::vector<const BufSpec*> smaller_types =
                    choose_smaller_buffers(
                        libs,
                        model,
                        node->type,
                        fanout,
                        std::max(
                            1,
                            cfg.final_alt_reclaim_types_per_node));
                for (const BufSpec* smaller : smaller_types) {
                    if (!smaller) continue;
                    const double new_area = model.estimate_buffer_area(*smaller);
                    const double area_save = current_area - new_area;
                    if (area_save > eps) {
                        const double new_delay =
                            buffer_nominal_delay_from_lib(*smaller, fanout);
                        FinalAltReclaimCandidate candidate;
                        candidate.kind =
                            node->original
                                ? FinalAltReclaimCandidateKind::ResizeOriginal
                                : FinalAltReclaimCandidateKind::ResizeNewBuffer;
                        candidate.node_name = node->name;
                        candidate.node_order = order;
                        candidate.old_type_id =
                            lib_cache.get_type_id(node->type);
                        candidate.new_type_id =
                            lib_cache.get_type_id(smaller->name);
                        candidate.area_save = area_save;
                        candidate.pressure = node_pressure;
                        candidate.delay_delta = new_delay - current_delay;
                        candidate.pressure_effect =
                            candidate.pressure * candidate.delay_delta;
                        candidate.score =
                            candidate.area_save * area_weight +
                            std::max(0.0, candidate.pressure_effect) *
                                timing_help_weight -
                            std::max(0.0, -candidate.pressure_effect) *
                                timing_harm_weight;
                        candidates.push_back(candidate);
                    }
                }
            }

            summary.second_round.final_alt_candidates_built +=
                static_cast<int>(candidates.size());

            if (final_alt_time_up()) {
                return std::vector<FinalAltReclaimCandidate>{};
            }

            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const FinalAltReclaimCandidate& lhs,
                         const FinalAltReclaimCandidate& rhs) {
                          constexpr double sort_eps = 1e-9;
                          if (std::abs(lhs.score - rhs.score) > sort_eps) {
                              return lhs.score > rhs.score;
                          }
                          if (std::abs(lhs.area_save - rhs.area_save) > sort_eps) {
                              return lhs.area_save > rhs.area_save;
                          }
                          const bool lhs_helpful = lhs.pressure_effect >= 0.0;
                          const bool rhs_helpful = rhs.pressure_effect >= 0.0;
                          if (lhs_helpful != rhs_helpful) return lhs_helpful;
                          if (std::abs(lhs.pressure_effect -
                                       rhs.pressure_effect) > sort_eps) {
                              return lhs.pressure_effect > rhs.pressure_effect;
                          }
                          if (lhs.kind != rhs.kind) {
                              return lhs.kind ==
                                  FinalAltReclaimCandidateKind::DeleteNewBuffer;
                          }
                          if (lhs.node_order != rhs.node_order) {
                              return lhs.node_order < rhs.node_order;
                          }
                          return lhs.new_type_id < rhs.new_type_id;
                      });
            if (final_alt_time_up()) {
                return std::vector<FinalAltReclaimCandidate>{};
            }

            if (effective_final_alt_reclaim_top_k_per_kind > 0) {
                std::array<int, 3> kept_by_kind = {0, 0, 0};
                std::vector<FinalAltReclaimCandidate> filtered;
                filtered.reserve(candidates.size());
                for (const auto& candidate : candidates) {
                    const int kind_id =
                        static_cast<int>(candidate.kind);
                    if (kind_id < 0 ||
                        kind_id >= static_cast<int>(kept_by_kind.size())) {
                        continue;
                    }
                    if (kept_by_kind[static_cast<size_t>(kind_id)] >=
                        effective_final_alt_reclaim_top_k_per_kind) {
                        continue;
                    }
                    kept_by_kind[static_cast<size_t>(kind_id)] += 1;
                    filtered.push_back(candidate);
                }
                candidates = std::move(filtered);
            }

            if (effective_final_alt_reclaim_top_k > 0 &&
                candidates.size() >
                    static_cast<size_t>(effective_final_alt_reclaim_top_k)) {
                candidates.resize(
                    static_cast<size_t>(effective_final_alt_reclaim_top_k));
            }

            summary.second_round.final_alt_candidates_kept_after_topk +=
                static_cast<int>(candidates.size());
            return candidates;
        };

        auto final_alt_reclaim = [&](double repair_cost_gain,
                                     int max_candidates) {
            int accepted = 0;
            int candidates_tried = 0;
            const bool fast_reclaim_available =
                cfg.enable_fast_timing_engine && fast_timing_engine;
            final_alt_use_fast_reclaim =
                fast_reclaim_available &&
                (final_alt_fast_current ||
                 sync_fast_timing("FinalAlt reclaim sync"));
            final_alt_fast_current = final_alt_use_fast_reclaim;
            const TimingAnalysisResult reclaim_baseline = final_alt_timing.timing;
            const double allowed_cost_giveback =
                std::max(0.0, repair_cost_gain) *
                std::max(0.0, cfg.final_alt_reclaim_giveback_ratio);
            if (cfg.enable_final_alt_ranked_reclaim_candidates) {
                std::vector<FinalAltReclaimCandidate> candidates =
                    build_final_alt_reclaim_candidates();
                for (const FinalAltReclaimCandidate& candidate : candidates) {
                    if (final_alt_time_up()) break;
                    if (max_candidates > 0 &&
                        candidates_tried >= max_candidates) {
                        break;
                    }
                    ++candidates_tried;
                    ++summary.iterations;
                    ++summary.second_round.final_alt_candidates_tried;

                    bool accepted_move = false;
                    if (candidate.kind ==
                        FinalAltReclaimCandidateKind::DeleteNewBuffer) {
                        accepted_move =
                            final_alt_try_delete(candidate.node_name,
                                                 reclaim_baseline,
                                                 allowed_cost_giveback);
                    } else {
                        std::string requested_new_type;
                        if (candidate.new_type_id >= 0 &&
                            static_cast<size_t>(candidate.new_type_id) <
                                lib_cache.type_name.size()) {
                            requested_new_type =
                                lib_cache.type_name[
                                    static_cast<size_t>(candidate.new_type_id)];
                        }
                        accepted_move =
                            final_alt_try_downsize_to(candidate.node_name,
                                                      requested_new_type,
                                                      reclaim_baseline,
                                                      allowed_cost_giveback);
                    }

                    if (accepted_move) {
                        accepted += 1;
                        ++summary.second_round.final_alt_candidates_accepted;
                    }
                }
                if (final_alt_use_fast_reclaim && accepted > 0) {
                    final_alt_ss_arrival = model.compute_clock_arrivals(tree, true);
                    final_alt_ff_arrival = model.compute_clock_arrivals(tree, false);
                    final_alt_timing =
                        build_phase1b_timing_cache(ss_paths,
                                                   ff_paths,
                                                   final_alt_ss_arrival,
                                                   final_alt_ff_arrival,
                                                   clock_period);
                    final_alt_area = fast_timing_engine->area();
                }
                if (!final_alt_use_fast_reclaim && accepted > 0) {
                    final_alt_fast_current = false;
                }
                final_alt_use_fast_reclaim = false;
                return accepted;
            }

            std::vector<std::string> preorder =
                collect_preorder_names(tree.root.get());

            for (const std::string& node_name : preorder) {
                if (final_alt_time_up()) break;
                if (max_candidates > 0 &&
                    candidates_tried >= max_candidates) {
                    break;
                }
                ClockNode* node = tree.find_node(node_name);
                if (!is_buffer_node(node)) continue;

                ++candidates_tried;
                summary.iterations += 1;
                ++summary.second_round.final_alt_candidates_tried;

                if (!node->original &&
                    final_alt_try_delete(node_name,
                                         reclaim_baseline,
                                         allowed_cost_giveback)) {
                    accepted += 1;
                    ++summary.second_round.final_alt_candidates_accepted;
                    continue;
                }

                if (final_alt_try_downsize(node_name,
                                           reclaim_baseline,
                                           allowed_cost_giveback)) {
                    accepted += 1;
                    ++summary.second_round.final_alt_candidates_accepted;
                }
            }
            if (final_alt_use_fast_reclaim && accepted > 0) {
                final_alt_ss_arrival = model.compute_clock_arrivals(tree, true);
                final_alt_ff_arrival = model.compute_clock_arrivals(tree, false);
                final_alt_timing =
                    build_phase1b_timing_cache(ss_paths,
                                               ff_paths,
                                               final_alt_ss_arrival,
                                               final_alt_ff_arrival,
                                               clock_period);
                final_alt_area = fast_timing_engine->area();
            }
            if (!final_alt_use_fast_reclaim && accepted > 0) {
                final_alt_fast_current = false;
            }
            final_alt_use_fast_reclaim = false;
            return accepted;
        };

        const int max_final_alt_iters = std::max(0, cfg.final_alt_max_iters);
        const int repair_per_iter =
            std::max(1, cfg.final_alt_repair_insertions_per_iter);
        for (int iter = 0; iter < max_final_alt_iters; ++iter) {
            current_final_alt_iter = iter;
            expire_final_alt_blacklist();
            if (final_alt_time_up()) {
                const double strategy8_reserve =
                    std::max(
                        0.0,
                        cfg.perturb_recover_time_budget_seconds) +
                    effective_finalization_reserve_seconds;
                const bool stopped_for_strategy8 =
                    perturb_recover_enabled &&
                    elapsed_seconds() + strategy8_reserve >=
                        cfg.time_limit_seconds &&
                    !is_time_up();
                if (stopped_for_strategy8) {
                    final_alt.stop_reason = "strategy8_time_reserve";
                } else if (!perturb_recover_enabled &&
                           effective_finalization_reserve_seconds > 0.0 &&
                           elapsed_seconds() +
                                   effective_finalization_reserve_seconds >=
                               cfg.time_limit_seconds &&
                           !is_time_up()) {
                    final_alt.stop_reason =
                        "final_validation_time_reserve";
                } else {
                    summary.early_stopped = true;
                    final_alt.stop_reason = "time_limit";
                    summary.message =
                        "Optimization stopped early due to time limit in Final Alternating Greedy.";
                }
                break;
            }

            final_alt.iterations += 1;
            FinalAltIterationRecord iter_record;
            iter_record.iteration = iter;
            iter_record.start_time_seconds = seconds_since(final_alt_profile_start);
            const TimingAnalysisResult before_iter_timing =
                final_alt_timing.timing;
            const double before_iter_area = final_alt_area;
            iter_record.before_timing = before_iter_timing;
            iter_record.after_repair_timing = before_iter_timing;
            iter_record.after_timing = before_iter_timing;
            iter_record.area_before = before_iter_area;
            iter_record.area_after_repair = before_iter_area;
            iter_record.area_after = before_iter_area;
            const bool needs_repair =
                final_alt_needs_repair(final_alt_timing.timing, cfg);
            iter_record.repair_needed = needs_repair;
            const int reclaim_tried_before =
                summary.second_round.final_alt_candidates_tried;
            const int reclaim_accepted_before =
                summary.second_round.final_alt_candidates_accepted;
            const int resizes_before = final_alt.resizes;
            const int deletes_before = final_alt.newbuf_deletes;

            if (!needs_repair) {
                if (cfg.final_alt_run_reclaim_when_no_repair_needed) {
                    final_alt_reclaim(0.0, 0);
                }
                iter_record.reclaim_candidates_tried =
                    summary.second_round.final_alt_candidates_tried -
                    reclaim_tried_before;
                iter_record.reclaim_moves_accepted =
                    summary.second_round.final_alt_candidates_accepted -
                    reclaim_accepted_before;
                iter_record.reclaim_moves_rejected =
                    iter_record.reclaim_candidates_tried -
                    iter_record.reclaim_moves_accepted;
                iter_record.reclaim_resizes = final_alt.resizes - resizes_before;
                iter_record.reclaim_deletes =
                    final_alt.newbuf_deletes - deletes_before;
                iter_record.after_repair_timing = final_alt_timing.timing;
                iter_record.after_timing = final_alt_timing.timing;
                iter_record.area_after_repair = final_alt_area;
                iter_record.area_after = final_alt_area;
                update_best_checkpoint(final_alt_timing.timing,
                                       final_alt_area,
                                       "final_alt_" + std::to_string(iter));
                iter_record.end_time_seconds = seconds_since(final_alt_profile_start);
                iter_record.stop_reason = "timing_good";
                compact_iteration_record(iter_record);
                summary.final_alt_iterations.push_back(std::move(iter_record));
                final_alt.stop_reason = "timing_good";
                break;
            }

            const int inserted = final_alt_insert_repair(repair_per_iter,
                                                         &iter_record);
            iter_record.after_repair_timing = final_alt_timing.timing;
            iter_record.area_after_repair = final_alt_area;
            update_final_alt_target_checkpoint(iter);
            if (inserted == 0) {
                const bool cooldown_wait =
                    cfg.final_alt_blacklist_cooldown_iters > 0 &&
                    !final_alt_blacklist_expiry.empty();
                if (!cooldown_wait &&
                    cfg.final_alt_run_reclaim_when_no_repair_needed) {
                    final_alt_reclaim(0.0, 0);
                }
                iter_record.reclaim_candidates_tried =
                    summary.second_round.final_alt_candidates_tried -
                    reclaim_tried_before;
                iter_record.reclaim_moves_accepted =
                    summary.second_round.final_alt_candidates_accepted -
                    reclaim_accepted_before;
                iter_record.reclaim_moves_rejected =
                    iter_record.reclaim_candidates_tried -
                    iter_record.reclaim_moves_accepted;
                iter_record.reclaim_resizes = final_alt.resizes - resizes_before;
                iter_record.reclaim_deletes =
                    final_alt.newbuf_deletes - deletes_before;
                iter_record.after_timing = final_alt_timing.timing;
                iter_record.area_after = final_alt_area;
                update_best_checkpoint(final_alt_timing.timing,
                                       final_alt_area,
                                       "final_alt_" + std::to_string(iter));
                update_final_alt_target_checkpoint(iter);
                iter_record.end_time_seconds = seconds_since(final_alt_profile_start);
                iter_record.stop_reason =
                    cooldown_wait
                        ? "blacklist_cooldown_wait"
                        : "no_repair_insertion";
                compact_iteration_record(iter_record);
                summary.final_alt_iterations.push_back(std::move(iter_record));
                if (cooldown_wait) continue;
                final_alt.stop_reason = "no_repair_insertion";
                break;
            }

            const double repair_cost_gain =
                final_alt_timing_cost(before_iter_timing) -
                final_alt_timing_cost(final_alt_timing.timing);
            final_alt_reclaim(repair_cost_gain, 0);
            iter_record.reclaim_candidates_tried =
                summary.second_round.final_alt_candidates_tried -
                reclaim_tried_before;
            iter_record.reclaim_moves_accepted =
                summary.second_round.final_alt_candidates_accepted -
                reclaim_accepted_before;
            iter_record.reclaim_moves_rejected =
                iter_record.reclaim_candidates_tried -
                iter_record.reclaim_moves_accepted;
            iter_record.reclaim_resizes = final_alt.resizes - resizes_before;
            iter_record.reclaim_deletes =
                final_alt.newbuf_deletes - deletes_before;
            iter_record.after_timing = final_alt_timing.timing;
            iter_record.area_after = final_alt_area;
            update_best_checkpoint(final_alt_timing.timing,
                                   final_alt_area,
                                   "final_alt_" + std::to_string(iter));
            update_final_alt_target_checkpoint(iter);
            iter_record.end_time_seconds = seconds_since(final_alt_profile_start);

            const double timing_progress =
                final_alt_timing_cost(before_iter_timing) -
                final_alt_timing_cost(final_alt_timing.timing);
            const double area_progress = before_iter_area - final_alt_area;
            if (timing_progress <= cfg.final_alt_progress_epsilon &&
                area_progress <= cfg.final_alt_progress_epsilon) {
                final_alt.stop_reason = "no_meaningful_progress";
                iter_record.stop_reason = "no_meaningful_progress";
                compact_iteration_record(iter_record);
                summary.final_alt_iterations.push_back(std::move(iter_record));
                break;
            }
            iter_record.stop_reason = "continue";
            compact_iteration_record(iter_record);
            summary.final_alt_iterations.push_back(std::move(iter_record));
        }

        if (final_alt.stop_reason == "not_started") {
            final_alt.stop_reason =
                max_final_alt_iters == 0 ? "max_iters_zero" : "max_iters";
        }

        if (cfg.enable_final_alt_target_checkpoint) {
            const double explored_target_score =
                final_alt_target_score(final_alt_timing.timing,
                                       final_alt_area);
            final_alt.target_checkpoint_restored =
                score_strictly_better(final_alt_target_best_score,
                                      explored_target_score,
                                      cfg);
            tree = final_alt_target_best_tree.clone();
            summary.applied_moves.resize(
                final_alt_target_best_move_log_size);
            final_alt.repair_insertions =
                final_alt_target_best_insertions;
            final_alt.reclaim_moves_accepted =
                final_alt_target_best_reclaim;
            final_alt.newbuf_deletes =
                final_alt_target_best_deletes;
            final_alt.resizes = final_alt_target_best_resizes;
            summary.phase1b_insertions =
                final_alt_target_best_phase1b_insertions;
            summary.phase2_removals =
                final_alt_target_best_phase2_removals;
            summary.phase2_downsizes =
                final_alt_target_best_phase2_downsizes;
            final_alt_ss_arrival =
                model.compute_clock_arrivals(tree, true);
            final_alt_ff_arrival =
                model.compute_clock_arrivals(tree, false);
            final_alt_timing =
                build_phase1b_timing_cache(ss_paths,
                                           ff_paths,
                                           final_alt_ss_arrival,
                                           final_alt_ff_arrival,
                                           clock_period);
            final_alt_area = compute_area_full();
            final_alt_fast_current =
                cfg.enable_fast_timing_engine &&
                fast_timing_engine &&
                sync_fast_timing(
                    "FinalAlt target checkpoint restore sync");
        }
        final_alt.target_score_after =
            final_alt_target_score(final_alt_timing.timing,
                                   final_alt_area);

        final_alt.runtime_seconds = seconds_since(final_alt_profile_start);
        final_alt.area_after = final_alt_area;
        final_alt.area_saved = final_alt.area_before - final_alt.area_after;
        final_alt.ss_tns_after = final_alt_timing.timing.ss.tns;
        final_alt.ss_wns_after = final_alt_timing.timing.ss.wns;
        final_alt.ss_violations_after =
            final_alt_timing.timing.ss.violating_paths;
        final_alt.ff_tns_after = final_alt_timing.timing.ff.tns;
        final_alt.ff_violations_after =
            final_alt_timing.timing.ff.violating_paths;

        capture_stage(summary.after_final_alt,
                      "FinalAlternatingGreedy",
                      final_alt.repair_insertions,
                      final_alt.newbuf_deletes,
                      final_alt.resizes,
                      &final_alt_timing.timing);

        PerturbRecoverSummary& perturb = summary.perturb_recover;
        if (perturb_recover_enabled) {
            const bool saved_repair_reclaim_score_acceptance =
                active_repair_reclaim_score_acceptance;
            active_repair_reclaim_score_acceptance =
                cfg.enable_perturb_recover_score_based_repair_reclaim;
            const auto strategy8_profile_start =
                std::chrono::steady_clock::now();
            perturb_recover_start = strategy8_profile_start;
            perturb_recover_started = true;
            perturb_recover_active = true;
            perturb.stop_reason = "max_cycles";

            const FinalAlternatingGreedySummary final_alt_stage_summary =
                final_alt;
            const int phase1b_insertions_before_strategy8 =
                summary.phase1b_insertions;
            const int phase2_removals_before_strategy8 =
                summary.phase2_removals;
            const int phase2_downsizes_before_strategy8 =
                summary.phase2_downsizes;

            auto refresh_final_alt_state =
                [&](const std::string& context) {
                    final_alt_ss_arrival =
                        model.compute_clock_arrivals(tree, true);
                    final_alt_ff_arrival =
                        model.compute_clock_arrivals(tree, false);
                    final_alt_timing =
                        build_phase1b_timing_cache(
                            ss_paths,
                            ff_paths,
                            final_alt_ss_arrival,
                            final_alt_ff_arrival,
                            clock_period);
                    final_alt_area = compute_area_full();
                    final_alt_fast_current =
                        cfg.enable_fast_timing_engine &&
                        fast_timing_engine &&
                        sync_fast_timing(context);
                };

            refresh_final_alt_state("Strategy8 initial sync");
            perturb.timing_before = final_alt_timing.timing;
            perturb.area_before = final_alt_area;
            perturb.score_before =
                weighted_objective_score(
                    model,
                    perturb.timing_before,
                    perturb.area_before,
                    baseline,
                    cfg);

            ClockTree current_tree = tree.clone();
            TimingAnalysisResult current_timing = perturb.timing_before;
            double current_area = perturb.area_before;
            double current_score = perturb.score_before;

            auto strategy8_target_score =
                [&](const TimingAnalysisResult& timing, double area) {
                    if (!cfg.enable_perturb_recover_target_checkpoint) {
                        return weighted_objective_score(
                            model, timing, area, baseline, cfg);
                    }
                    return model.compute_score_metrics(
                                    timing,
                                    baseline.timing,
                                    area,
                                    baseline.area,
                                    cfg.perturb_recover_checkpoint_alpha,
                                    cfg.perturb_recover_checkpoint_beta,
                                    cfg.perturb_recover_checkpoint_gamma)
                        .total_score;
                };
            const double initial_target_score =
                strategy8_target_score(current_timing, current_area);
            double current_target_score = initial_target_score;
            perturb.target_score_before = initial_target_score;

            ClockTree best_tree = current_tree.clone();
            TimingAnalysisResult strategy8_best_timing = current_timing;
            double strategy8_best_area = current_area;
            double strategy8_best_score = current_score;
            double strategy8_best_target_score = initial_target_score;
            size_t strategy8_best_move_log_size =
                summary.applied_moves.size();

            std::mt19937 rng(cfg.perturb_recover_random_seed);
            int no_improvement_streak = 0;
            int target_checkpoint_no_improvement_streak = 0;
            int empty_perturb_streak = 0;
            std::unordered_set<std::string>
                brutal_attempted_path_signatures;

            auto strategy8_time_up = [&]() {
                if (cfg.perturb_recover_time_budget_seconds > 0.0 &&
                    seconds_since(strategy8_profile_start) >=
                        cfg.perturb_recover_time_budget_seconds) {
                    return true;
                }
                return elapsed_seconds() +
                           effective_finalization_reserve_seconds >=
                       cfg.time_limit_seconds;
            };
            double estimated_recovery_seconds =
                std::max(
                    0.0,
                    cfg.perturb_recover_initial_recovery_estimate_seconds);
            auto recovery_time_available = [&]() {
                const double safety_multiplier =
                    std::max(
                        1.0,
                        cfg.perturb_recover_recovery_estimate_safety_multiplier);
                const double required_seconds =
                    estimated_recovery_seconds * safety_multiplier;
                const double global_remaining =
                    cfg.time_limit_seconds -
                    effective_finalization_reserve_seconds -
                    elapsed_seconds();
                double local_remaining =
                    std::numeric_limits<double>::infinity();
                if (cfg.perturb_recover_time_budget_seconds > 0.0) {
                    local_remaining =
                        cfg.perturb_recover_time_budget_seconds -
                        seconds_since(strategy8_profile_start);
                }
                return global_remaining >= required_seconds &&
                       local_remaining >= required_seconds;
            };
            auto observe_recovery_runtime = [&](double runtime_seconds) {
                if (runtime_seconds <= 0.0) return;
                estimated_recovery_seconds =
                    std::max(
                        runtime_seconds,
                        0.5 * estimated_recovery_seconds +
                            0.5 * runtime_seconds);
            };

            auto pick_index = [&](size_t count) {
                std::uniform_int_distribution<size_t> distribution(
                    0,
                    count - 1);
                return distribution(rng);
            };

            const int min_perturb_moves =
                std::max(1, cfg.perturb_recover_min_moves);
            const int max_perturb_moves =
                std::max(
                    min_perturb_moves,
                    cfg.perturb_recover_max_moves);
            const int intensity_streak =
                std::max(
                    1,
                    cfg.perturb_recover_intensity_step_streak);
            const int move_attempt_multiplier =
                std::max(
                    1,
                    cfg.perturb_recover_move_attempt_multiplier);

            const int candidates_per_cycle =
                cfg.perturb_recover_use_brutal_area_path
                    ? 1
                    : std::max(
                          1,
                          cfg.perturb_recover_candidates_per_cycle);
            const double guided_target_ratio =
                std::clamp(
                    cfg.perturb_recover_guided_target_ratio,
                    0.0,
                    1.0);
            std::uniform_real_distribution<double> unit_random(0.0, 1.0);

            auto reset_strategy8_trial_accounting =
                [&](size_t move_log_start,
                    int base_insertions,
                    int base_reclaim,
                    int base_deletes,
                    int base_resizes) {
                    summary.applied_moves.resize(move_log_start);
                    final_alt.repair_insertions = base_insertions;
                    final_alt.reclaim_moves_accepted = base_reclaim;
                    final_alt.newbuf_deletes = base_deletes;
                    final_alt.resizes = base_resizes;
                    summary.phase1b_insertions =
                        phase1b_insertions_before_strategy8;
                    summary.phase2_removals =
                        phase2_removals_before_strategy8;
                    summary.phase2_downsizes =
                        phase2_downsizes_before_strategy8;
                };

            for (int cycle_index = 0;
                 cycle_index <
                 std::max(0, cfg.perturb_recover_max_cycles);
                 ++cycle_index) {
                if (strategy8_time_up()) {
                    perturb.stop_reason = "time_budget";
                    break;
                }

                PerturbRecoverCycleRecord cycle;
                bool target_checkpoint_improved_this_cycle = false;
                cycle.cycle_index = cycle_index;
                cycle.score_before = current_score;
                cycle.area_before = current_area;
                cycle.timing_before = current_timing;
                cycle.requested_moves =
                    std::min(
                        max_perturb_moves,
                        min_perturb_moves +
                            no_improvement_streak / intensity_streak);

                const size_t applied_move_log_start =
                    summary.applied_moves.size();
                const int cycle_final_alt_insertions =
                    final_alt.repair_insertions;
                const int cycle_final_alt_reclaim =
                    final_alt.reclaim_moves_accepted;
                const int cycle_final_alt_deletes =
                    final_alt.newbuf_deletes;
                const int cycle_final_alt_resizes =
                    final_alt.resizes;

                bool have_selected_candidate = false;
                ClockTree selected_tree;
                TimingAnalysisResult selected_timing;
                double selected_area = 0.0;
                double selected_score =
                    -std::numeric_limits<double>::infinity();
                int selected_perturb_inserts = 0;
                int selected_perturb_deletes = 0;
                int selected_perturb_resizes = 0;
                int selected_quick_insertions = 0;
                int selected_quick_deletes = 0;
                int selected_quick_resizes = 0;
                std::unordered_set<std::string>
                    selected_protected_buffers;
                std::vector<std::string> selected_move_logs;

                for (int candidate_index = 0;
                     candidate_index < candidates_per_cycle;
                     ++candidate_index) {
                    if (strategy8_time_up()) break;

                    enum class CandidateProfile {
                        BrutalAreaPath,
                        Legacy,
                        Timing,
                        Area,
                        Balanced,
                        Random
                    };
                    const CandidateProfile candidate_profile =
                        cfg.perturb_recover_use_brutal_area_path
                            ? CandidateProfile::BrutalAreaPath
                        : !cfg.perturb_recover_enable_candidate_profiles
                            ? CandidateProfile::Legacy
                            : [&]() {
                                  switch (candidate_index % 4) {
                                      case 0: return CandidateProfile::Timing;
                                      case 1: return CandidateProfile::Area;
                                      case 2: return CandidateProfile::Balanced;
                                      default: return CandidateProfile::Random;
                                  }
                              }();
                    const auto profile_name = [&]() -> const char* {
                        switch (candidate_profile) {
                            case CandidateProfile::BrutalAreaPath:
                                return "brutal";
                            case CandidateProfile::Timing: return "timing";
                            case CandidateProfile::Area: return "area";
                            case CandidateProfile::Balanced: return "balanced";
                            case CandidateProfile::Random: return "random";
                            default: return "legacy";
                        }
                    };

                    tree = current_tree.clone();
                    refresh_final_alt_state(
                        "Strategy8 beam candidate restore sync");
                    final_alt_blacklist.clear();
                    final_alt_blacklist_expiry.clear();
                    final_alt_target_failures.clear();
                    current_final_alt_iter = 0;
                    reset_strategy8_trial_accounting(
                        applied_move_log_start,
                        cycle_final_alt_insertions,
                        cycle_final_alt_reclaim,
                        cycle_final_alt_deletes,
                        cycle_final_alt_resizes);

                    std::vector<std::string> nodes =
                        collect_preorder_names(tree.root.get());
                    if (nodes.empty()) break;

                    std::vector<std::string> timing_targets;
                    std::vector<std::string> timing_buffers;
                    std::vector<std::string> area_resize_targets;
                    std::vector<std::string> deletable_buffers;
                    std::vector<std::pair<double, std::string>>
                        ranked_area_resize_targets;
                    std::vector<std::pair<double, std::string>>
                        ranked_deletable_buffers;
                    std::unordered_set<std::string> timing_target_seen;
                    std::unordered_set<std::string> timing_buffer_seen;
                    size_t guided_paths_scanned = 0;
                    for (const TimingPathResult& path :
                         current_timing.path_results) {
                        if ((guided_paths_scanned++ & 255U) == 0U &&
                            strategy8_time_up()) {
                            break;
                        }
                        std::vector<std::string> endpoint_names;
                        if (path.setup_slack < 0.0) {
                            endpoint_names.push_back(path.capture_ff);
                        }
                        if (path.hold_slack < 0.0) {
                            endpoint_names.push_back(path.launch_ff);
                        }
                        for (const std::string& endpoint_name :
                             endpoint_names) {
                            ClockNode* guided_node =
                                tree.find_node(endpoint_name);
                            while (guided_node) {
                                if (guided_node->parent &&
                                    timing_target_seen.insert(
                                        guided_node->name).second) {
                                    timing_targets.push_back(
                                        guided_node->name);
                                }
                                if (is_buffer_node(guided_node) &&
                                    timing_buffer_seen.insert(
                                        guided_node->name).second) {
                                    timing_buffers.push_back(
                                        guided_node->name);
                                }
                                guided_node = guided_node->parent;
                            }
                        }
                    }
                    if (strategy8_time_up()) break;
                    size_t area_nodes_scanned = 0;
                    for (const std::string& node_name : nodes) {
                        if ((area_nodes_scanned++ & 255U) == 0U &&
                            strategy8_time_up()) {
                            break;
                        }
                        ClockNode* candidate_node =
                            tree.find_node(node_name);
                        if (is_buffer_node(candidate_node)) {
                            const BufSpec* current_lib =
                                cached_lib(candidate_node->type);
                            if (current_lib) {
                                const double current_buffer_area =
                                    model.estimate_buffer_area(*current_lib);
                                double best_area_saving = 0.0;
                                for (const auto& lib : libs) {
                                    if (lib.name == candidate_node->type ||
                                        !local_resize_legality(
                                            candidate_node,
                                            &lib)) {
                                        continue;
                                    }
                                    best_area_saving =
                                        std::max(
                                            best_area_saving,
                                            current_buffer_area -
                                                model.estimate_buffer_area(lib));
                                }
                                if (best_area_saving >
                                    cfg.area_comparison_epsilon) {
                                    ranked_area_resize_targets.emplace_back(
                                        best_area_saving,
                                        node_name);
                                }
                            }
                        }
                        if (cached_reclaim_delete_ok(candidate_node)) {
                            const BufSpec* current_lib =
                                cached_lib(candidate_node->type);
                            ranked_deletable_buffers.emplace_back(
                                current_lib
                                    ? model.estimate_buffer_area(*current_lib)
                                    : 0.0,
                                node_name);
                        }
                    }
                    if (strategy8_time_up()) break;
                    auto rank_larger_first =
                        [](const auto& lhs, const auto& rhs) {
                            if (lhs.first != rhs.first) {
                                return lhs.first > rhs.first;
                            }
                            return lhs.second < rhs.second;
                        };
                    std::sort(
                        ranked_area_resize_targets.begin(),
                        ranked_area_resize_targets.end(),
                        rank_larger_first);
                    std::sort(
                        ranked_deletable_buffers.begin(),
                        ranked_deletable_buffers.end(),
                        rank_larger_first);
                    if (strategy8_time_up()) break;
                    for (const auto& ranked :
                         ranked_area_resize_targets) {
                        area_resize_targets.push_back(ranked.second);
                    }
                    for (const auto& ranked :
                         ranked_deletable_buffers) {
                        deletable_buffers.push_back(ranked.second);
                    }

                    auto pick_target_name =
                        [&](const std::vector<std::string>& guided_pool,
                            bool& used_guided,
                            bool prefer_front = false,
                            double profile_guided_ratio = -1.0) {
                            const double effective_guided_ratio =
                                profile_guided_ratio >= 0.0
                                    ? profile_guided_ratio
                                    : guided_target_ratio;
                            used_guided =
                                !guided_pool.empty() &&
                                unit_random(rng) <
                                    effective_guided_ratio;
                            const std::vector<std::string>& pool =
                                used_guided ? guided_pool : nodes;
                            size_t eligible_count = pool.size();
                            if (used_guided && prefer_front) {
                                eligible_count =
                                    std::max<size_t>(
                                        1,
                                        (pool.size() + 3) / 4);
                            }
                            return pool[pick_index(eligible_count)];
                        };

                    std::vector<std::string> candidate_perturb_logs;
                    std::unordered_set<std::string>
                        candidate_protected_buffers;
                    int candidate_guided_moves = 0;
                    int candidate_random_moves = 0;
                    int candidate_resizes = 0;
                    int candidate_inserts = 0;
                    int candidate_deletes = 0;
                    int applied_perturb_moves = 0;
                    const int max_move_attempts =
                        cycle.requested_moves *
                        move_attempt_multiplier;

                    if (cfg.perturb_recover_use_brutal_area_path) {
                        std::vector<std::string> brutal_path;
                        std::string brutal_leaf;
                        std::string brutal_path_signature;
                        double brutal_area = -1.0;
                        bool brutal_scan_timed_out = false;
                        size_t brutal_leaves_scanned = 0;
                        for (const std::string& node_name : nodes) {
                            if ((brutal_leaves_scanned++ & 255U) == 0U &&
                                strategy8_time_up()) {
                                brutal_scan_timed_out = true;
                                break;
                            }
                            ClockNode* leaf = tree.find_node(node_name);
                            if (!leaf || !leaf->children.empty()) {
                                continue;
                            }
                            std::vector<std::string> path;
                            std::string path_signature;
                            double path_area = 0.0;
                            bool path_has_deletable_buffer = false;
                            size_t path_nodes_scanned = 0;
                            for (ClockNode* path_node = leaf;
                                 path_node;
                                 path_node = path_node->parent) {
                                if ((path_nodes_scanned++ & 255U) == 0U &&
                                    strategy8_time_up()) {
                                    brutal_scan_timed_out = true;
                                    break;
                                }
                                path.push_back(path_node->name);
                                if (!is_buffer_node(path_node)) continue;
                                path_signature += path_node->name;
                                path_signature.push_back('\n');
                                const BufSpec* path_lib =
                                    cached_lib(path_node->type);
                                if (path_lib) {
                                    path_area +=
                                        model.estimate_buffer_area(
                                            *path_lib);
                                }
                                path_has_deletable_buffer =
                                    path_has_deletable_buffer ||
                                    cached_reclaim_delete_ok(path_node);
                            }
                            if (brutal_scan_timed_out) break;
                            if (path_signature.empty() ||
                                !path_has_deletable_buffer ||
                                brutal_attempted_path_signatures.count(
                                    path_signature) != 0) {
                                continue;
                            }
                            if (brutal_leaf.empty() ||
                                path_area >
                                    brutal_area +
                                        cfg.area_comparison_epsilon ||
                                (std::abs(path_area - brutal_area) <=
                                     cfg.area_comparison_epsilon &&
                                 leaf->name < brutal_leaf)) {
                                brutal_leaf = leaf->name;
                                brutal_area = path_area;
                                brutal_path = std::move(path);
                                brutal_path_signature =
                                    std::move(path_signature);
                            }
                        }

                        if (brutal_scan_timed_out || strategy8_time_up()) break;

                        if (!brutal_leaf.empty()) {
                            brutal_attempted_path_signatures.insert(
                                brutal_path_signature);
                            cycle.brutal_path_leaf = brutal_leaf;
                            cycle.brutal_path_nodes =
                                static_cast<int>(brutal_path.size());
                            cycle.brutal_path_area = brutal_area;
                            cycle.requested_moves =
                                static_cast<int>(brutal_path.size());
                        }

                        for (const std::string& node_name :
                             brutal_path) {
                            if (strategy8_time_up()) break;
                            ++cycle.perturb_attempts;
                            ++perturb.perturb_attempts;
                            ClockNode* node =
                                tree.find_node(node_name);
                            if (!is_buffer_node(node)) continue;

                            const bool changed =
                                cached_reclaim_delete_ok(node) &&
                                tree.delete_buffer(node_name);
                            if (changed) {
                                ++candidate_deletes;
                                candidate_perturb_logs.push_back(
                                    "Strategy8 brutal-path delete " +
                                    node_name + " leaf=" +
                                    brutal_leaf);
                                ++applied_perturb_moves;
                                ++candidate_guided_moves;
                            }
                        }
                    } else {
                    for (int attempt = 0;
                         attempt < max_move_attempts &&
                         applied_perturb_moves < cycle.requested_moves;
                         ++attempt) {
                        if (strategy8_time_up()) break;
                        ++cycle.perturb_attempts;
                        ++perturb.perturb_attempts;

                        const int operation = [&]() {
                            if (candidate_profile ==
                                    CandidateProfile::Legacy ||
                                candidate_profile ==
                                    CandidateProfile::Random) {
                                return static_cast<int>(rng() % 3U);
                            }
                            const unsigned int draw =
                                static_cast<unsigned int>(rng() % 100U);
                            if (candidate_profile ==
                                CandidateProfile::Timing) {
                                return draw < 45U ? 0 :
                                       draw < 55U ? 1 : 2;
                            }
                            if (candidate_profile ==
                                CandidateProfile::Area) {
                                return draw < 55U ? 0 : 1;
                            }
                            return draw < 40U ? 0 :
                                   draw < 70U ? 1 : 2;
                        }();
                        bool changed = false;
                        bool used_guided = false;

                        if (operation == 0) {
                            const std::vector<std::string>*
                                resize_pool = &timing_buffers;
                            bool prefer_front = false;
                            double profile_guided_ratio = -1.0;
                            if (candidate_profile ==
                                CandidateProfile::Area) {
                                resize_pool = &area_resize_targets;
                                prefer_front = true;
                            } else if (candidate_profile ==
                                       CandidateProfile::Balanced) {
                                resize_pool =
                                    unit_random(rng) < 0.5
                                        ? &timing_buffers
                                        : &area_resize_targets;
                            } else if (candidate_profile ==
                                       CandidateProfile::Random) {
                                resize_pool = &area_resize_targets;
                                profile_guided_ratio = 0.0;
                            }
                            const std::string node_name =
                                pick_target_name(
                                    *resize_pool,
                                    used_guided,
                                    prefer_front,
                                    profile_guided_ratio);
                            ClockNode* node =
                                tree.find_node(node_name);
                            if (is_buffer_node(node)) {
                                std::vector<const BufSpec*> legal_types;
                                for (const auto& lib : libs) {
                                    if (lib.name == node->type) continue;
                                    if (!local_resize_legality(
                                            node,
                                            &lib)) {
                                        continue;
                                    }
                                    legal_types.push_back(&lib);
                                }
                                if (!legal_types.empty()) {
                                    const BufSpec* chosen = nullptr;
                                    if (candidate_profile ==
                                        CandidateProfile::Area) {
                                        chosen = *std::min_element(
                                            legal_types.begin(),
                                            legal_types.end(),
                                            [&](const BufSpec* lhs,
                                                const BufSpec* rhs) {
                                                return model.estimate_buffer_area(
                                                           *lhs) <
                                                       model.estimate_buffer_area(
                                                           *rhs);
                                            });
                                    } else if (candidate_profile ==
                                               CandidateProfile::Timing) {
                                        chosen = *std::min_element(
                                            legal_types.begin(),
                                            legal_types.end(),
                                            [&](const BufSpec* lhs,
                                                const BufSpec* rhs) {
                                                const size_t fanout =
                                                    node->children.size();
                                                const double lhs_delay =
                                                    0.5 *
                                                    (model.buffer_delay_ss(
                                                         lhs->name,
                                                         fanout) +
                                                     model.buffer_delay_ff(
                                                         lhs->name,
                                                         fanout));
                                                const double rhs_delay =
                                                    0.5 *
                                                    (model.buffer_delay_ss(
                                                         rhs->name,
                                                         fanout) +
                                                     model.buffer_delay_ff(
                                                         rhs->name,
                                                         fanout));
                                                return lhs_delay < rhs_delay;
                                            });
                                    } else {
                                        chosen =
                                            legal_types[
                                                pick_index(
                                                    legal_types.size())];
                                    }
                                    const std::string old_type =
                                        node->type;
                                    if (tree.set_buffer_type(
                                            node_name,
                                            chosen->name)) {
                                        changed = true;
                                        ++candidate_resizes;
                                        candidate_perturb_logs.push_back(
                                            "Strategy8 perturb resize " +
                                            node_name + " " + old_type +
                                            " -> " + chosen->name);
                                    }
                                }
                            }
                        } else if (operation == 1) {
                            const bool area_profile =
                                candidate_profile ==
                                CandidateProfile::Area;
                            const double profile_guided_ratio =
                                candidate_profile ==
                                        CandidateProfile::Random
                                    ? 0.0
                                    : -1.0;
                            const std::string node_name =
                                pick_target_name(
                                    deletable_buffers,
                                    used_guided,
                                    area_profile,
                                    profile_guided_ratio);
                            ClockNode* node =
                                tree.find_node(node_name);
                            if (cached_reclaim_delete_ok(node) &&
                                tree.delete_buffer(node_name)) {
                                changed = true;
                                ++candidate_deletes;
                                candidate_perturb_logs.push_back(
                                    "Strategy8 perturb delete " +
                                    node_name);
                            }
                        } else {
                            const double profile_guided_ratio =
                                candidate_profile ==
                                        CandidateProfile::Random
                                    ? 0.0
                                    : -1.0;
                            const std::string target_name =
                                pick_target_name(
                                    timing_targets,
                                    used_guided,
                                    false,
                                    profile_guided_ratio);
                            ClockNode* target =
                                tree.find_node(target_name);
                            if (target && target->parent) {
                                std::vector<const BufSpec*> insert_types;
                                for (const auto& lib : libs) {
                                    if (lib.ss_delay.empty() ||
                                        lib.ff_delay.empty()) {
                                        continue;
                                    }
                                    insert_types.push_back(&lib);
                                }
                                if (!insert_types.empty()) {
                                    const BufSpec* chosen =
                                        insert_types[
                                            pick_index(
                                                insert_types.size())];
                                    const std::string parent_name =
                                        target->parent->name;
                                    const std::string buffer_name =
                                        tree.generate_unique_name(
                                            "STRATEGY8_BUF");
                                    if (tree.insert_buffer_between(
                                            parent_name,
                                            target_name,
                                            buffer_name,
                                            chosen->name)) {
                                        changed = true;
                                        ++candidate_inserts;
                                        candidate_protected_buffers.insert(
                                            buffer_name);
                                        candidate_perturb_logs.push_back(
                                            "Strategy8 perturb insert " +
                                            buffer_name + " above " +
                                            target_name + " type " +
                                            chosen->name);
                                    }
                                }
                            }
                        }

                        if (changed) {
                            ++applied_perturb_moves;
                            if (used_guided) {
                                ++candidate_guided_moves;
                            } else {
                                ++candidate_random_moves;
                            }
                        }
                    }
                    }

                    cycle.perturb_resizes += candidate_resizes;
                    cycle.perturb_inserts += candidate_inserts;
                    cycle.perturb_deletes += candidate_deletes;
                    cycle.guided_moves += candidate_guided_moves;
                    cycle.random_moves += candidate_random_moves;
                    perturb.perturb_resizes += candidate_resizes;
                    perturb.perturb_inserts += candidate_inserts;
                    perturb.perturb_deletes += candidate_deletes;

                    if (applied_perturb_moves == 0) {
                        reset_strategy8_trial_accounting(
                            applied_move_log_start,
                            cycle_final_alt_insertions,
                            cycle_final_alt_reclaim,
                            cycle_final_alt_deletes,
                            cycle_final_alt_resizes);
                        continue;
                    }
                    ++cycle.candidates_generated;

                    refresh_final_alt_state(
                        "Strategy8 quick-recovery sync");
                    final_alt_blacklist.clear();
                    final_alt_blacklist_expiry.clear();
                    final_alt_target_failures.clear();

                    int quick_iterations = 0;
                    for (int recovery_iter = 0;
                         recovery_iter <
                         std::max(
                             0,
                             cfg.perturb_recover_use_brutal_area_path
                                 ? 0
                                 : cfg.perturb_recover_quick_recovery_iters);
                         ++recovery_iter) {
                        if (strategy8_time_up()) break;
                        ++quick_iterations;
                        ++cycle.quick_recovery_iterations;
                        ++cycle.recovery_iterations;
                        ++perturb.recovery_iterations;
                        current_final_alt_iter = recovery_iter;
                        const TimingAnalysisResult before_recovery =
                            final_alt_timing.timing;
                        const int inserted =
                            final_alt_insert_repair(
                                std::max(
                                    1,
                                    cfg.perturb_recover_quick_repair_attempts),
                                nullptr);
                        const double repair_gain =
                            final_alt_timing_cost(before_recovery) -
                            final_alt_timing_cost(
                                final_alt_timing.timing);
                        const int reclaimed =
                            final_alt_reclaim(
                                repair_gain,
                                std::max(
                                    1,
                                    cfg.perturb_recover_quick_reclaim_candidates));
                        if (inserted == 0 && reclaimed == 0) break;
                        final_alt_blacklist.clear();
                        final_alt_blacklist_expiry.clear();
                        final_alt_target_failures.clear();
                    }
                    if (quick_iterations > 0) {
                        ++cycle.candidates_quick_recovered;
                    }

                    const int quick_insertions =
                        final_alt.repair_insertions -
                        cycle_final_alt_insertions;
                    const int quick_reclaim =
                        final_alt.reclaim_moves_accepted -
                        cycle_final_alt_reclaim;
                    const int quick_deletes =
                        final_alt.newbuf_deletes -
                        cycle_final_alt_deletes;
                    const int quick_resizes =
                        final_alt.resizes -
                        cycle_final_alt_resizes;
                    cycle.recovery_insertions += quick_insertions;
                    cycle.recovery_reclaim_moves += quick_reclaim;
                    perturb.recovery_insertions += quick_insertions;
                    perturb.recovery_reclaim_moves += quick_reclaim;

                    const TimingAnalysisResult quick_timing =
                        model.analyze_timing(
                            tree,
                            ss_paths,
                            ff_paths,
                            clock_period);
                    const double quick_area = compute_area_full();
                    const double quick_score =
                        weighted_objective_score(
                            model,
                            quick_timing,
                            quick_area,
                            baseline,
                            cfg);
                    const LegalityReport quick_legality =
                        validate_full();

                    std::vector<std::string> quick_move_logs =
                        candidate_perturb_logs;
                    for (size_t log_index = applied_move_log_start;
                         log_index < summary.applied_moves.size();
                         ++log_index) {
                        quick_move_logs.push_back(
                            "Strategy8 quick recovery: " +
                            summary.applied_moves[log_index]);
                    }

                    const bool better_screened_candidate =
                        quick_legality.ok &&
                        (!have_selected_candidate ||
                         quick_score >
                             selected_score +
                                 cfg.score_acceptance_epsilon ||
                         (std::abs(
                              quick_score - selected_score) <=
                              cfg.score_acceptance_epsilon &&
                          quick_area < selected_area));
                    if (better_screened_candidate) {
                        have_selected_candidate = true;
                        selected_tree = tree.clone();
                        selected_timing = quick_timing;
                        selected_area = quick_area;
                        selected_score = quick_score;
                        cycle.selected_candidate = candidate_index;
                        cycle.selected_profile = profile_name();
                        selected_perturb_inserts = candidate_inserts;
                        selected_perturb_deletes = candidate_deletes;
                        selected_perturb_resizes = candidate_resizes;
                        selected_quick_insertions = quick_insertions;
                        selected_quick_deletes = quick_deletes;
                        selected_quick_resizes = quick_resizes;
                        selected_protected_buffers =
                            candidate_protected_buffers;
                        selected_move_logs =
                            std::move(quick_move_logs);
                    }

                    reset_strategy8_trial_accounting(
                        applied_move_log_start,
                        cycle_final_alt_insertions,
                        cycle_final_alt_reclaim,
                        cycle_final_alt_deletes,
                        cycle_final_alt_resizes);
                }

                if (!have_selected_candidate) {
                    ++empty_perturb_streak;
                    ++no_improvement_streak;
                    ++perturb.rejected_cycles;
                    cycle.score_after = current_score;
                    cycle.area_after = current_area;
                    cycle.timing_after = current_timing;
                    cycle.stop_reason = "no_legal_perturbation";
                    compact_perturb_record(cycle);
                    perturb.cycle_records.push_back(std::move(cycle));
                    ++perturb.cycles;
                    tree = current_tree.clone();
                    refresh_final_alt_state(
                        "Strategy8 empty beam restore sync");
                    if (empty_perturb_streak >= 3) {
                        perturb.stop_reason =
                            "no_legal_perturbation";
                        break;
                    }
                    continue;
                }
                empty_perturb_streak = 0;

                tree = selected_tree.clone();
                refresh_final_alt_state(
                    "Strategy8 selected-candidate deep sync");
                final_alt_blacklist.clear();
                final_alt_blacklist_expiry.clear();
                final_alt_target_failures.clear();
                reset_strategy8_trial_accounting(
                    applied_move_log_start,
                    cycle_final_alt_insertions,
                    cycle_final_alt_reclaim,
                    cycle_final_alt_deletes,
                    cycle_final_alt_resizes);

                int deep_insertions = 0;
                int deep_reclaim = 0;
                int deep_deletes = 0;
                int deep_resizes = 0;
                if (!recovery_time_available()) {
                    perturb.stop_reason = "insufficient_recovery_time";
                    reset_strategy8_trial_accounting(
                        applied_move_log_start,
                        cycle_final_alt_insertions,
                        cycle_final_alt_reclaim,
                        cycle_final_alt_deletes,
                        cycle_final_alt_resizes);
                    tree = current_tree.clone();
                    break;
                }
                if (cfg.perturb_recover_use_cycle_deep_recovery ||
                    cfg.perturb_recover_use_brutal_area_path) {
                    ClockTree recovery_best_tree = tree.clone();
                    double recovery_best_score = selected_score;
                    double recovery_best_area = selected_area;
                    size_t recovery_best_move_log_size =
                        summary.applied_moves.size();
                    int recovery_best_insertions = 0;
                    int recovery_best_reclaim = 0;
                    int recovery_best_deletes = 0;
                    int recovery_best_resizes = 0;
                    std::unordered_set<std::string>
                        deep_pulse_recovery_blacklist;
                    double deep_pulse_delay_scale =
                        effective_phase1a_pulse_delay_scale;
                    int deep_pulse_failure_streak = 0;
                    for (int recovery_iter = 0;
                         recovery_iter <
                         std::max(
                             0,
                             cfg.perturb_recover_use_brutal_area_path
                                 ? cfg.perturb_recover_brutal_recovery_max_cycles
                                 : cfg.perturb_recover_recovery_iters);
                         ++recovery_iter) {
                        if (strategy8_time_up() ||
                            !recovery_time_available()) {
                            if (!strategy8_time_up()) {
                                perturb.stop_reason =
                                    "insufficient_recovery_time";
                            }
                            break;
                        }
                        const auto recovery_iteration_start =
                            std::chrono::steady_clock::now();
                        ++cycle.deep_recovery_iterations;
                        ++cycle.recovery_iterations;
                        ++perturb.recovery_iterations;
                        bool recovery_progress = false;

                        if (recovery_iter == 0 &&
                            (cfg.perturb_recover_use_brutal_area_path ||
                             cfg.perturb_recover_bootstrap_reclaim_candidates !=
                                 0)) {
                            RepairReclaimCycleRecord bootstrap_cycle;
                            bootstrap_cycle.cycle_index = recovery_iter;
                            const TimingAnalysisResult bootstrap_timing =
                                model.analyze_timing(
                                    tree,
                                    ss_paths,
                                    ff_paths,
                                    clock_period);
                            const double bootstrap_area =
                                compute_area_full();
                            const auto* protected_deletes =
                                cfg.perturb_recover_bootstrap_protect_inserted
                                    ? &selected_protected_buffers
                                    : nullptr;
                            run_pressure_guided_reclaim(
                                bootstrap_cycle,
                                bootstrap_timing,
                                bootstrap_timing,
                                bootstrap_area,
                                cfg.perturb_recover_use_brutal_area_path
                                    ? 0
                                    : std::max(
                                          0,
                                          cfg.perturb_recover_bootstrap_reclaim_candidates),
                                cfg.perturb_recover_use_brutal_area_path
                                    ? cfg.cycle_reclaim_time_budget_seconds
                                    : 0.0,
                                protected_deletes,
                                strategy8_time_up,
                                false);
                            cycle.bootstrap_reclaim_tried +=
                                bootstrap_cycle.reclaim_candidates_tried;
                            cycle.bootstrap_reclaim_accepted +=
                                bootstrap_cycle.reclaim_candidates_accepted;
                            perturb.bootstrap_reclaim_tried +=
                                bootstrap_cycle.reclaim_candidates_tried;
                            perturb.bootstrap_reclaim_accepted +=
                                bootstrap_cycle.reclaim_candidates_accepted;
                            deep_reclaim +=
                                bootstrap_cycle.reclaim_candidates_accepted;
                            deep_deletes +=
                                bootstrap_cycle.newbuf_deletes_accepted;
                            deep_resizes +=
                                bootstrap_cycle.original_resizes_accepted +
                                bootstrap_cycle.newbuf_resizes_accepted;
                            recovery_progress =
                                bootstrap_cycle.reclaim_candidates_accepted >
                                0;
                            if (strategy8_time_up()) break;
                        }

                        Phase1APulseResult pulse =
                            run_phase1a_pulse(
                                recovery_iter,
                                deep_pulse_delay_scale,
                                false,
                                deep_pulse_recovery_blacklist,
                                cfg.perturb_recover_use_brutal_area_path
                                    ? 0
                                    : std::max(
                                          1,
                                          cfg.perturb_recover_repair_attempts),
                                cfg.enable_shared_path_repair,
                                true,
                                strategy8_time_up);
                        cycle.cycle_pulse_batch_attempts +=
                            pulse.batch_attempts;
                        cycle.cycle_pulse_insertions +=
                            pulse.inserted_count;
                        perturb.cycle_pulse_batch_attempts +=
                            pulse.batch_attempts;
                        perturb.cycle_pulse_insertions +=
                            pulse.inserted_count;
                        deep_insertions += pulse.inserted_count;
                        bool pulse_retry_available = false;
                        if (pulse.accepted &&
                            pulse.inserted_count > 0) {
                            summary.applied_moves.insert(
                                summary.applied_moves.end(),
                                pulse.moves.begin(),
                                pulse.moves.end());
                            recovery_progress = true;
                            deep_pulse_failure_streak = 0;
                            deep_pulse_delay_scale =
                                effective_phase1a_pulse_delay_scale;
                            deep_pulse_recovery_blacklist.clear();
                        } else {
                            const bool recoverable_failure =
                                cfg.enable_repair_pulse_failure_recovery &&
                                (pulse.stop_reason ==
                                     "split_depth_exhausted_timing" ||
                                 pulse.stop_reason ==
                                     "split_depth_exhausted_score" ||
                                 pulse.stop_reason ==
                                     "batch_worsened_timing" ||
                                 pulse.stop_reason ==
                                     "batch_worsened_score");
                            if (recoverable_failure) {
                                ++deep_pulse_failure_streak;
                                pulse_retry_available =
                                    cfg.repair_pulse_max_consecutive_failures >
                                        0 &&
                                    deep_pulse_failure_streak <
                                        cfg.repair_pulse_max_consecutive_failures;
                                if (pulse_retry_available) {
                                    deep_pulse_delay_scale =
                                        std::max(
                                            cfg.repair_pulse_recovery_min_delay_scale,
                                            deep_pulse_delay_scale *
                                                cfg.repair_pulse_recovery_scale_multiplier);
                                    int targets_added = 0;
                                    for (const std::string& target :
                                         pulse.selected_targets) {
                                        if (cfg.repair_pulse_recovery_blacklist_targets >
                                                0 &&
                                            targets_added >=
                                                cfg.repair_pulse_recovery_blacklist_targets) {
                                            break;
                                        }
                                        if (deep_pulse_recovery_blacklist
                                                .insert(target)
                                                .second) {
                                            ++targets_added;
                                        }
                                    }
                                }
                            }
                        }
                        const bool pulse_hit_time_limit =
                            pulse.time_limit;

                        if (!pulse_hit_time_limit &&
                            pulse.accepted &&
                            pulse.inserted_count > 0 &&
                            cfg.enable_pressure_guided_full_tree_reclaim) {
                            RepairReclaimCycleRecord reclaim_cycle;
                            reclaim_cycle.cycle_index = recovery_iter;
                            run_pressure_guided_reclaim(
                                reclaim_cycle,
                                pulse.before_timing,
                                pulse.after_timing,
                                pulse.after_area,
                                cfg.perturb_recover_use_brutal_area_path
                                    ? 0
                                    : std::max(
                                          0,
                                          cfg.perturb_recover_reclaim_candidates),
                                cfg.perturb_recover_use_brutal_area_path
                                    ? cfg.cycle_reclaim_time_budget_seconds
                                    : 0.0,
                                nullptr,
                                strategy8_time_up,
                                pulse.fast_timing_current);
                            cycle.cycle_reclaim_tried +=
                                reclaim_cycle.reclaim_candidates_tried;
                            cycle.cycle_reclaim_accepted +=
                                reclaim_cycle.reclaim_candidates_accepted;
                            perturb.cycle_reclaim_tried +=
                                reclaim_cycle.reclaim_candidates_tried;
                            perturb.cycle_reclaim_accepted +=
                                reclaim_cycle.reclaim_candidates_accepted;
                            deep_reclaim +=
                                reclaim_cycle.reclaim_candidates_accepted;
                            deep_deletes +=
                                reclaim_cycle.newbuf_deletes_accepted;
                            deep_resizes +=
                                reclaim_cycle.original_resizes_accepted +
                                reclaim_cycle.newbuf_resizes_accepted;
                            recovery_progress =
                                recovery_progress ||
                                reclaim_cycle.reclaim_candidates_accepted > 0;
                        }

                        refresh_final_alt_state(
                            "Strategy8 cycle deep-recovery sync");
                        const double recovery_score =
                            weighted_objective_score(
                                model,
                                final_alt_timing.timing,
                                final_alt_area,
                                baseline,
                                cfg);
                        if (recovery_score >
                                recovery_best_score +
                                    cfg.score_acceptance_epsilon ||
                            (std::abs(
                                 recovery_score -
                                 recovery_best_score) <=
                                 cfg.score_acceptance_epsilon &&
                             final_alt_area < recovery_best_area)) {
                            recovery_best_tree = tree.clone();
                            recovery_best_score = recovery_score;
                            recovery_best_area = final_alt_area;
                            recovery_best_move_log_size =
                                summary.applied_moves.size();
                            recovery_best_insertions =
                                deep_insertions;
                            recovery_best_reclaim =
                                deep_reclaim;
                            recovery_best_deletes =
                                deep_deletes;
                            recovery_best_resizes =
                                deep_resizes;
                        }
                        if (pulse_hit_time_limit) break;
                        if (!recovery_progress &&
                            !pulse_retry_available) {
                            break;
                        }
                        observe_recovery_runtime(
                            seconds_since(recovery_iteration_start));
                    }
                    tree = recovery_best_tree.clone();
                    refresh_final_alt_state(
                        "Strategy8 best cycle-recovery checkpoint sync");
                    summary.applied_moves.resize(
                        recovery_best_move_log_size);
                    deep_insertions =
                        recovery_best_insertions;
                    deep_reclaim = recovery_best_reclaim;
                    deep_deletes = recovery_best_deletes;
                    deep_resizes = recovery_best_resizes;
                } else {
                    for (int recovery_iter = 0;
                         recovery_iter <
                         std::max(
                             0,
                             cfg.perturb_recover_recovery_iters);
                         ++recovery_iter) {
                        if (strategy8_time_up()) break;
                        ++cycle.deep_recovery_iterations;
                        ++cycle.recovery_iterations;
                        ++perturb.recovery_iterations;
                        current_final_alt_iter = recovery_iter;
                        const TimingAnalysisResult before_recovery =
                            final_alt_timing.timing;
                        const int inserted =
                            final_alt_insert_repair(
                                std::max(
                                    1,
                                    cfg.perturb_recover_repair_attempts),
                                nullptr);
                        const double repair_gain =
                            final_alt_timing_cost(before_recovery) -
                            final_alt_timing_cost(
                                final_alt_timing.timing);
                        const int reclaimed =
                            final_alt_reclaim(
                                repair_gain,
                                std::max(
                                    1,
                                    cfg.perturb_recover_reclaim_candidates));
                        if (inserted == 0 && reclaimed == 0) break;
                        final_alt_blacklist.clear();
                        final_alt_blacklist_expiry.clear();
                        final_alt_target_failures.clear();
                    }
                    deep_insertions =
                        final_alt.repair_insertions -
                        cycle_final_alt_insertions;
                    deep_reclaim =
                        final_alt.reclaim_moves_accepted -
                        cycle_final_alt_reclaim;
                    deep_deletes =
                        final_alt.newbuf_deletes -
                        cycle_final_alt_deletes;
                    deep_resizes =
                        final_alt.resizes -
                        cycle_final_alt_resizes;
                }
                cycle.recovery_insertions += deep_insertions;
                cycle.recovery_reclaim_moves += deep_reclaim;
                perturb.recovery_insertions += deep_insertions;
                perturb.recovery_reclaim_moves += deep_reclaim;

                TimingAnalysisResult candidate_timing =
                    model.analyze_timing(
                        tree,
                        ss_paths,
                        ff_paths,
                        clock_period);
                double candidate_area = compute_area_full();
                double candidate_score =
                    weighted_objective_score(
                        model,
                        candidate_timing,
                        candidate_area,
                        baseline,
                        cfg);
                LegalityReport candidate_legality =
                    validate_full();
                bool use_deep_candidate =
                    candidate_legality.ok &&
                    (candidate_score >
                         selected_score +
                             cfg.score_acceptance_epsilon ||
                     (std::abs(
                          candidate_score - selected_score) <=
                          cfg.score_acceptance_epsilon &&
                      candidate_area < selected_area));
                if (!use_deep_candidate) {
                    tree = selected_tree.clone();
                    refresh_final_alt_state(
                        "Strategy8 quick checkpoint restore sync");
                    candidate_timing = selected_timing;
                    candidate_area = selected_area;
                    candidate_score = selected_score;
                    candidate_legality = LegalityReport{};
                }

                cycle.score_after = candidate_score;
                cycle.area_after = candidate_area;
                cycle.timing_after = candidate_timing;
                cycle.legal = candidate_legality.ok;
                cycle.timing_guard_passed =
                    !cfg.perturb_recover_require_timing_not_worse ||
                    timing_not_worse(
                        candidate_timing,
                        current_timing);

                const double violation_growth =
                    std::max(
                        0.0,
                        cfg.perturb_recover_violation_growth_ratio);
                const size_t allowed_ss_violations =
                    current_timing.ss.violating_paths +
                    static_cast<size_t>(
                        std::ceil(
                            violation_growth *
                            static_cast<double>(
                                current_timing.ss.violating_paths)));
                const size_t allowed_ff_violations =
                    current_timing.ff.violating_paths +
                    static_cast<size_t>(
                        std::ceil(
                            violation_growth *
                            static_cast<double>(
                                current_timing.ff.violating_paths)));
                cycle.violation_guard_passed =
                    !cfg.perturb_recover_require_violation_guard ||
                    (candidate_timing.ss.violating_paths <=
                         allowed_ss_violations &&
                     candidate_timing.ff.violating_paths <=
                         allowed_ff_violations);
                cycle.accepted =
                    cycle.legal &&
                    cycle.timing_guard_passed &&
                    cycle.violation_guard_passed &&
                    score_strictly_better(
                        cfg.enable_perturb_recover_target_outer_acceptance
                            ? strategy8_target_score(candidate_timing,
                                                     candidate_area)
                            : candidate_score,
                        cfg.enable_perturb_recover_target_outer_acceptance
                            ? current_target_score
                            : current_score,
                        cfg);

                std::vector<std::string> deep_move_logs;
                for (size_t log_index = applied_move_log_start;
                     log_index < summary.applied_moves.size();
                     ++log_index) {
                    deep_move_logs.push_back(
                        "Strategy8 deep recovery: " +
                        summary.applied_moves[log_index]);
                }
                reset_strategy8_trial_accounting(
                    applied_move_log_start,
                    cycle_final_alt_insertions,
                    cycle_final_alt_reclaim,
                    cycle_final_alt_deletes,
                    cycle_final_alt_resizes);

                if (cycle.accepted) {
                    summary.applied_moves.insert(
                        summary.applied_moves.end(),
                        selected_move_logs.begin(),
                        selected_move_logs.end());
                    if (use_deep_candidate) {
                        summary.applied_moves.insert(
                            summary.applied_moves.end(),
                            deep_move_logs.begin(),
                            deep_move_logs.end());
                    }

                    current_tree = tree.clone();
                    current_timing = candidate_timing;
                    current_area = candidate_area;
                    current_score = candidate_score;
                    current_target_score =
                        strategy8_target_score(current_timing,
                                               current_area);
                    ++perturb.accepted_cycles;
                    no_improvement_streak = 0;
                    if (cfg.perturb_recover_use_brutal_area_path) {
                        brutal_attempted_path_signatures.clear();
                    }
                    perturb.accepted_insertions +=
                        selected_perturb_inserts +
                        selected_quick_insertions +
                        (use_deep_candidate ? deep_insertions : 0);
                    perturb.accepted_deletes +=
                        selected_perturb_deletes +
                        selected_quick_deletes +
                        (use_deep_candidate ? deep_deletes : 0);
                    perturb.accepted_resizes +=
                        selected_perturb_resizes +
                        selected_quick_resizes +
                        (use_deep_candidate ? deep_resizes : 0);

                    const double current_target_score =
                        strategy8_target_score(
                            current_timing,
                            current_area);
                    if (score_strictly_better(
                            current_target_score,
                            strategy8_best_target_score,
                            cfg) ||
                        (std::abs(
                             current_target_score -
                             strategy8_best_target_score) <=
                             cfg.score_acceptance_epsilon &&
                         current_area < strategy8_best_area)) {
                        best_tree = current_tree.clone();
                        strategy8_best_timing =
                            current_timing;
                        strategy8_best_area = current_area;
                        strategy8_best_score = current_score;
                        strategy8_best_target_score =
                            current_target_score;
                        strategy8_best_move_log_size =
                            summary.applied_moves.size();
                        target_checkpoint_improved_this_cycle = true;
                        ++perturb.best_updates;
                    }
                    cycle.stop_reason = "accepted_improvement";
                } else {
                    summary.applied_moves.resize(
                        applied_move_log_start);
                    tree = current_tree.clone();
                    refresh_final_alt_state(
                        "Strategy8 rejected beam restore sync");
                    ++perturb.rejected_cycles;
                    ++no_improvement_streak;
                    if (!cycle.legal) {
                        cycle.stop_reason = "illegal_candidate";
                    } else if (!cycle.timing_guard_passed) {
                        cycle.stop_reason = "timing_guard";
                    } else if (!cycle.violation_guard_passed) {
                        cycle.stop_reason = "violation_guard";
                    } else {
                        cycle.stop_reason = "score_not_better";
                    }
                }

                compact_perturb_record(cycle);
                perturb.cycle_records.push_back(std::move(cycle));
                ++perturb.cycles;
                if (cfg.enable_perturb_recover_target_checkpoint) {
                    if (target_checkpoint_improved_this_cycle) {
                        target_checkpoint_no_improvement_streak = 0;
                    } else {
                        ++target_checkpoint_no_improvement_streak;
                    }
                    const int target_streak_limit =
                        cfg.perturb_recover_max_cycles_without_target_improvement;
                    if (target_streak_limit > 0 &&
                        target_checkpoint_no_improvement_streak >=
                            target_streak_limit) {
                        perturb.stop_reason =
                            "target_checkpoint_no_improvement";
                        break;
                    }
                }
            }

            const double final_explored_target_score =
                strategy8_target_score(current_timing, current_area);
            perturb.target_checkpoint_restored =
                cfg.enable_perturb_recover_target_checkpoint &&
                score_strictly_better(
                    strategy8_best_target_score,
                    final_explored_target_score,
                    cfg);
            tree = best_tree.clone();
            refresh_final_alt_state("Strategy8 best restore sync");
            summary.applied_moves.resize(
                strategy8_best_move_log_size);
            perturb.timing_after = strategy8_best_timing;
            perturb.area_after = strategy8_best_area;
            perturb.score_after = strategy8_best_score;
            perturb.best_score = strategy8_best_score;
            perturb.target_score_after =
                strategy8_best_target_score;
            perturb.runtime_seconds =
                seconds_since(strategy8_profile_start);
            if (perturb.cycles >=
                    std::max(
                        0,
                        cfg.perturb_recover_max_cycles) &&
                perturb.stop_reason == "max_cycles") {
                perturb.stop_reason = "max_cycles";
            }

            final_alt = final_alt_stage_summary;
            summary.phase1b_insertions =
                phase1b_insertions_before_strategy8;
            summary.phase2_removals =
                phase2_removals_before_strategy8;
            summary.phase2_downsizes =
                phase2_downsizes_before_strategy8;
            perturb_recover_active = false;
            active_repair_reclaim_score_acceptance =
                saved_repair_reclaim_score_acceptance;

            capture_stage(
                summary.after_perturb_recover,
                "PerturbAndRecover",
                perturb.accepted_insertions,
                perturb.accepted_deletes,
                perturb.accepted_resizes,
                &perturb.timing_after);
            perturb.timing_before =
                compact_timing_snapshot(perturb.timing_before);
            perturb.timing_after =
                compact_timing_snapshot(perturb.timing_after);
        }
    } else {
    // Phase 1B: Oracle-Driven Iterative Greedy (Fine Cleanup)
    const auto phase1b_profile_start = std::chrono::steady_clock::now();
    const int max_phase1b_attempts = cfg.legacy_phase1b_max_attempts;
    auto phase1b_ss_arrival = model.compute_clock_arrivals(tree, true);
    auto phase1b_ff_arrival = model.compute_clock_arrivals(tree, false);
    Phase1bTimingCache phase1b_timing = build_phase1b_timing_cache(ss_paths,
                                                                   ff_paths,
                                                                   phase1b_ss_arrival,
                                                                   phase1b_ff_arrival,
                                                                   clock_period);
    for (int iter1b = 0; iter1b < max_phase1b_attempts; ++iter1b) {
        if (is_time_up()) {
            summary.early_stopped = true;
            summary.message = "Optimization stopped early due to time limit in Phase 1B.";
            break;
        }

        summary.iterations += 1;

        if (phase1b_timing.timing.ss.tns == 0.0 && phase1b_timing.timing.ff.tns == 0.0) {
            break;
        }

        WorstPathChoice choice = choose_worst_target(phase1b_timing.timing, blacklist);
        if (!choice.valid) {
            summary.message = "Phase 1B stopped: no non-blacklisted violating target found.";
            break;
        }

        ClockNode* target = tree.find_node(choice.target_node);
        if (!target || !target->parent) {
            blacklist.insert(choice.target_node);
            continue;
        }

        const std::string parent_name = target->parent->name;
        const std::string buffer_name = tree.generate_unique_name("NEW_BUF");
        const double required_delay = -choice.violation_slack;
        const BufSpec* adaptive_buffer = choose_batch_buffer(required_delay);
        if (!adaptive_buffer) adaptive_buffer = smallest_buffer;
        if (!tree.insert_buffer_between(parent_name, choice.target_node, buffer_name, adaptive_buffer->name)) {
            blacklist.insert(choice.target_node);
            continue;
        }

        ClockNode* inserted_buffer = tree.find_node(buffer_name);
        ArrivalRollback ss_rollback;
        ArrivalRollback ff_rollback;
        update_subtree_arrivals(model, inserted_buffer, true, phase1b_ss_arrival, ss_rollback);
        update_subtree_arrivals(model, inserted_buffer, false, phase1b_ff_arrival, ff_rollback);

        std::vector<size_t> affected_groups = affected_path_groups(phase1b_timing, ss_rollback);
        const TimingCornerResult previous_ss = phase1b_timing.timing.ss;
        const TimingCornerResult previous_ff = phase1b_timing.timing.ff;
        PathTimingRollback path_rollback = update_affected_path_groups(phase1b_timing,
                                                                       affected_groups,
                                                                       phase1b_ss_arrival,
                                                                       phase1b_ff_arrival);
        constexpr double timing_eps = 1e-9;
        const bool all_not_worse =
            phase1b_timing.timing.ss.wns >= previous_ss.wns - timing_eps &&
            phase1b_timing.timing.ff.wns >= previous_ff.wns - timing_eps &&
            phase1b_timing.timing.ss.tns >= previous_ss.tns - timing_eps &&
            phase1b_timing.timing.ff.tns >= previous_ff.tns - timing_eps;
        const bool any_improved =
            phase1b_timing.timing.ss.wns > previous_ss.wns + timing_eps ||
            phase1b_timing.timing.ff.wns > previous_ff.wns + timing_eps ||
            phase1b_timing.timing.ss.tns > previous_ss.tns + timing_eps ||
            phase1b_timing.timing.ff.tns > previous_ff.tns + timing_eps;
        const bool improved = all_not_worse && any_improved;

        if (improved) {
            summary.phase1b_insertions += 1;
            const std::string move = "Phase1B insert " + buffer_name + " above " + choice.target_node +
                                     " from path " + choice.path_name +
                                     (choice.is_setup ? " [setup]" : " [hold]");
            summary.applied_moves.push_back(move);
            verify_cached_timing(move,
                                 model,
                                 tree,
                                 ss_paths,
                                 ff_paths,
                                 clock_period,
                                 phase1b_timing.timing);
            continue;
        }

        rollback_path_groups(phase1b_timing, path_rollback);
        rollback_arrivals(phase1b_ss_arrival, ss_rollback);
        rollback_arrivals(phase1b_ff_arrival, ff_rollback);
        RemovedBufferState removed;
        if (remove_buffer_node(tree, buffer_name, removed)) {
            blacklist.insert(choice.target_node);
        }
    }

    capture_stage(summary.after_phase1b,
                  "Phase1B",
                  summary.phase1b_insertions,
                  0,
                  0,
                  &phase1b_timing.timing);
    if (summary.runtime_profile.enabled) {
        summary.runtime_profile.phase1b_seconds +=
            seconds_since(phase1b_profile_start);
    }

    // Phase 2: Multi-pass Pattern-Matching and Rule-Based Area Recovery
    const auto phase2_profile_start = std::chrono::steady_clock::now();
    const double phase2_gap_threshold = cfg.legacy_phase2_gap_threshold;
    const std::vector<Pattern> pattern_sequence = {
        Pattern::ParallelMerge,
        Pattern::CascadedCollapse,
        Pattern::SizeSwap,
        Pattern::Rebalance,
        Pattern::CascadedCollapse,
        Pattern::GreedyFallback,
    };

    auto phase2_ss_arrival = model.compute_clock_arrivals(tree, true);
    auto phase2_ff_arrival = model.compute_clock_arrivals(tree, false);
    Phase1bTimingCache phase2_timing = build_phase1b_timing_cache(ss_paths,
                                                                   ff_paths,
                                                                   phase2_ss_arrival,
                                                                   phase2_ff_arrival,
                                                                   clock_period);
    TimingAnalysisResult current_timing = phase2_timing.timing;
    bool phase2_stopped = false;

    auto node_depth = [](const ClockNode* node) -> size_t {
        size_t depth = 0;
        for (const ClockNode* cur = node; cur && cur->parent; cur = cur->parent) {
            ++depth;
        }
        return depth;
    };

    auto try_change_types = [&](const std::vector<std::pair<std::string, std::string>>& changes) -> bool {
        struct SavedType {
            std::string name;
            std::string old_type;
        };

        std::vector<SavedType> saved;
        saved.reserve(changes.size());
        ClockNode* highest_node = nullptr;
        size_t highest_depth = std::numeric_limits<size_t>::max();

        for (const auto& change : changes) {
            ClockNode* node = tree.find_node(change.first);
            if (!is_buffer_node(node)) {
                for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                    tree.set_buffer_type(it->name, it->old_type);
                }
                return false;
            }

            saved.push_back({change.first, node->type});
            const size_t depth = node_depth(node);
            if (!highest_node || depth < highest_depth) {
                highest_node = node;
                highest_depth = depth;
            }
        }

        for (const auto& change : changes) {
            if (!tree.set_buffer_type(change.first, change.second)) {
                for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                    tree.set_buffer_type(it->name, it->old_type);
                }
                return false;
            }
        }

        ArrivalRollback ss_rollback;
        ArrivalRollback ff_rollback;
        TimingAnalysisResult previous_timing = phase2_timing.timing;
        update_subtree_arrivals(model, highest_node, true, phase2_ss_arrival, ss_rollback);
        update_subtree_arrivals(model, highest_node, false, phase2_ff_arrival, ff_rollback);

        ArrivalRollback affected_rollback;
        affected_rollback.old_values = ss_rollback.old_values;
        affected_rollback.old_values.insert(affected_rollback.old_values.end(),
                                           ff_rollback.old_values.begin(),
                                           ff_rollback.old_values.end());

        std::vector<size_t> affected_groups = affected_path_groups(phase2_timing, affected_rollback);
        PathTimingRollback path_rollback = update_affected_path_groups(phase2_timing,
                                                                       affected_groups,
                                                                       phase2_ss_arrival,
                                                                       phase2_ff_arrival);

        if (timing_not_worse(phase2_timing.timing, previous_timing)) {
            current_timing = phase2_timing.timing;
            return true;
        }

        rollback_path_groups(phase2_timing, path_rollback);
        rollback_arrivals(phase2_ss_arrival, ss_rollback);
        rollback_arrivals(phase2_ff_arrival, ff_rollback);
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            tree.set_buffer_type(it->name, it->old_type);
        }
        phase2_timing.timing = previous_timing;
        current_timing = previous_timing;
        return false;
    };

    auto pattern_name = [](Pattern pattern) -> std::string {
        switch (pattern) {
            case Pattern::ParallelMerge: return "ParallelMerge";
            case Pattern::CascadedCollapse: return "CascadedCollapse";
            case Pattern::SizeSwap: return "SizeSwap";
            case Pattern::Rebalance: return "Rebalance";
            case Pattern::GreedyFallback: return "GreedyFallback";
        }
        return "Unknown";
    };

    int phase2_iteration_index = 0;
    auto capture_phase2_iteration = [&](Pattern pattern,
                                        int added_buffers,
                                        int removed_buffers,
                                        int downsized_buffers) {
        StageSnapshot snapshot;
        snapshot.valid = true;
        snapshot.name = "Phase 2 iter " + std::to_string(++phase2_iteration_index) + " [" + pattern_name(pattern) + "]";
        snapshot.added_buffers = added_buffers;
        snapshot.removed_buffers = removed_buffers;
        snapshot.downsized_buffers = downsized_buffers;
        snapshot.runtime_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        snapshot.timing = phase2_timing.timing;
        snapshot.area = model.compute_tree_area(tree);
        snapshot.legality = model.validate_legality(tree, libs);
        summary.phase2_iteration_snapshots.push_back(snapshot);
        print_stage_summary(snapshot);
        verify_cached_timing(snapshot.name + " snapshot",
                             model,
                             tree,
                             ss_paths,
                             ff_paths,
                             clock_period,
                             phase2_timing.timing);
    };

    auto try_full_removal = [&](const std::string& target_name, int& removed_counter) -> bool {
        ClockNode* target = tree.find_node(target_name);
        if (!target || target->original || !target->parent) return false;

        ClockNode* parent = target->parent;
        TimingAnalysisResult previous_timing = phase2_timing.timing;

        RemovedBufferState removed;
        if (!remove_buffer_node(tree, target_name, removed)) return false;

        ArrivalRollback ss_rollback;
        ArrivalRollback ff_rollback;
        update_subtree_arrivals(model, parent, true, phase2_ss_arrival, ss_rollback);
        update_subtree_arrivals(model, parent, false, phase2_ff_arrival, ff_rollback);

        ArrivalRollback affected_rollback;
        affected_rollback.old_values = ss_rollback.old_values;
        affected_rollback.old_values.insert(affected_rollback.old_values.end(),
                                           ff_rollback.old_values.begin(),
                                           ff_rollback.old_values.end());

        std::vector<size_t> affected_groups = affected_path_groups(phase2_timing, affected_rollback);
        PathTimingRollback path_rollback = update_affected_path_groups(phase2_timing,
                                                                       affected_groups,
                                                                       phase2_ss_arrival,
                                                                       phase2_ff_arrival);

        if (timing_not_worse(phase2_timing.timing, previous_timing)) {
            summary.phase2_removals += 1;
            const std::string move = "Phase2 remove " + target_name;
            summary.applied_moves.push_back(move);
            current_timing = phase2_timing.timing;
            removed_counter += 1;
            verify_cached_timing(move,
                                 model,
                                 tree,
                                 ss_paths,
                                 ff_paths,
                                 clock_period,
                                 phase2_timing.timing);
            return true;
        }

        rollback_path_groups(phase2_timing, path_rollback);
        rollback_arrivals(phase2_ss_arrival, ss_rollback);
        rollback_arrivals(phase2_ff_arrival, ff_rollback);
        restore_buffer_node(tree, removed);
        phase2_timing.timing = previous_timing;
        current_timing = previous_timing;
        return false;
    };

    for (Pattern pattern : pattern_sequence) {
        int pattern_removed = 0;
        int pattern_downsized = 0;
        bool made_changes = true;
        while (made_changes) {
            if (is_time_up()) {
                summary.early_stopped = true;
                summary.message = "Optimization stopped early due to time limit in Phase 2.";
                phase2_stopped = true;
                break;
            }

            made_changes = false;
            std::vector<std::string> preorder = collect_preorder_names(tree.root.get());

            for (const std::string& node_name : preorder) {
                if (is_time_up()) {
                    summary.early_stopped = true;
                    summary.message = "Optimization stopped early due to time limit in Phase 2.";
                    phase2_stopped = true;
                    break;
                }

                ClockNode* node = tree.find_node(node_name);
                if (!is_buffer_node(node)) continue;

                summary.iterations += 1;

                if (pattern == Pattern::ParallelMerge) {
                    ClockNode* parent = node->parent;
                    if (!parent) continue;

                    std::vector<size_t> same_type_indices;
                    same_type_indices.reserve(parent->children.size());
                    for (size_t i = 0; i < parent->children.size(); ++i) {
                        ClockNode* sibling = parent->children[i].get();
                        if (!is_buffer_node(sibling) || sibling->type != node->type) continue;
                        same_type_indices.push_back(i);
                    }
                    if (same_type_indices.size() < 2) continue;

                    size_t survivor_index = same_type_indices.front();
                    for (size_t index : same_type_indices) {
                        ClockNode* sibling = parent->children[index].get();
                        if (sibling && sibling->original) {
                            survivor_index = index;
                            break;
                        }
                    }

                    ClockNode* survivor = parent->children[survivor_index].get();
                    const BufSpec* survivor_lib = survivor ? find_lib_by_name(libs, survivor->type) : nullptr;
                    if (!survivor_lib) continue;

                    std::vector<size_t> remove_indices;
                    remove_indices.reserve(same_type_indices.size() - 1);
                    size_t merged_fanout = survivor->children.size();
                    for (size_t index : same_type_indices) {
                        ClockNode* sibling = parent->children[index].get();
                        if (!sibling || sibling == survivor || sibling->original) {
                            remove_indices.clear();
                            break;
                        }
                        merged_fanout += sibling->children.size();
                        remove_indices.push_back(index);
                    }
                    if (remove_indices.empty()) continue;
                    if (merged_fanout > survivor_lib->ss_delay.size()) continue;

                    const std::string parent_name = parent->name;
                    const std::string survivor_name = survivor->name;
                    ParallelMergeRollback rollback;
                    if (!apply_parallel_merge(tree, parent, survivor, remove_indices, rollback)) {
                        rollback_parallel_merge(tree, rollback);
                        continue;
                    }

                    ArrivalRollback ss_rollback;
                    ArrivalRollback ff_rollback;
                    TimingAnalysisResult previous_timing = phase2_timing.timing;
                    update_subtree_arrivals(model, parent, true, phase2_ss_arrival, ss_rollback);
                    update_subtree_arrivals(model, parent, false, phase2_ff_arrival, ff_rollback);

                    ArrivalRollback affected_rollback;
                    affected_rollback.old_values = ss_rollback.old_values;
                    affected_rollback.old_values.insert(affected_rollback.old_values.end(),
                                                       ff_rollback.old_values.begin(),
                                                       ff_rollback.old_values.end());

                    std::vector<size_t> affected_groups = affected_path_groups(phase2_timing, affected_rollback);
                    PathTimingRollback path_rollback = update_affected_path_groups(phase2_timing,
                                                                                   affected_groups,
                                                                                   phase2_ss_arrival,
                                                                                   phase2_ff_arrival);

                    if (timing_not_worse(phase2_timing.timing, previous_timing)) {
                        summary.phase2_removals += static_cast<int>(remove_indices.size());
                        const std::string move = "Phase2 parallel-merge at " + parent_name + " keep " + survivor_name;
                        summary.applied_moves.push_back(move);
                        current_timing = phase2_timing.timing;
                        pattern_removed += static_cast<int>(remove_indices.size());
                        made_changes = true;
                        verify_cached_timing(move,
                                             model,
                                             tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period,
                                             phase2_timing.timing);
                        continue;
                    }

                    rollback_path_groups(phase2_timing, path_rollback);
                    rollback_arrivals(phase2_ss_arrival, ss_rollback);
                    rollback_arrivals(phase2_ff_arrival, ff_rollback);
                    phase2_timing.timing = previous_timing;
                    current_timing = previous_timing;
                    rollback_parallel_merge(tree, rollback);
                    continue;
                }

                if (pattern == Pattern::CascadedCollapse) {
                    if (node->children.size() != 1) continue;

                    const std::string node_name_copy = node->name;
                    const std::string child_name_copy = node->children.front()->name;
                    ClockNode* child = tree.find_node(child_name_copy);
                    if (!is_buffer_node(child)) continue;

                    if (try_full_removal(node_name_copy, pattern_removed)) {
                        made_changes = true;
                        continue;
                    }

                    node = tree.find_node(node_name_copy);
                    child = tree.find_node(child_name_copy);
                    if (!is_buffer_node(node) || !is_buffer_node(child)) continue;

                    if (try_full_removal(child_name_copy, pattern_removed)) {
                        made_changes = true;
                        continue;
                    }

                    node = tree.find_node(node_name_copy);
                    child = tree.find_node(child_name_copy);
                    if (!is_buffer_node(node) || !is_buffer_node(child)) continue;

                    const BufSpec* first_downsize = choose_next_smaller_buffer(libs, model, node->type, node->children.size());
                    const BufSpec* second_downsize = choose_next_smaller_buffer(libs, model, child->type, child->children.size());

                    if (first_downsize && second_downsize) {
                        const std::string first_name = node->name;
                        const std::string second_name = child->name;
                        if (try_change_types({{first_name, first_downsize->name}, {second_name, second_downsize->name}})) {
                            summary.phase2_downsizes += 2;
                            const std::string move = "Phase2 cascaded-collapse downsize " + first_name + " and " + second_name;
                            summary.applied_moves.push_back(move);
                            pattern_downsized += 2;
                            made_changes = true;
                            verify_cached_timing(move,
                                                 model,
                                                 tree,
                                                 ss_paths,
                                                 ff_paths,
                                                 clock_period,
                                                 phase2_timing.timing);
                        }
                    }

                    node = tree.find_node(node_name_copy);
                    child = tree.find_node(child_name_copy);
                    if (!is_buffer_node(node) || !is_buffer_node(child)) continue;

                    if (first_downsize && try_change_types({{node->name, first_downsize->name}})) {
                        summary.phase2_downsizes += 1;
                        const std::string move = "Phase2 cascaded-collapse downsize " + node->name + " to " + first_downsize->name;
                        summary.applied_moves.push_back(move);
                        pattern_downsized += 1;
                        made_changes = true;
                        verify_cached_timing(move,
                                             model,
                                             tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period,
                                             phase2_timing.timing);
                    }

                    node = tree.find_node(node_name_copy);
                    child = tree.find_node(child_name_copy);
                    if (!is_buffer_node(node) || !is_buffer_node(child)) continue;

                    if (second_downsize && try_change_types({{child->name, second_downsize->name}})) {
                        summary.phase2_downsizes += 1;
                        const std::string move = "Phase2 cascaded-collapse downsize " + child->name + " to " + second_downsize->name;
                        summary.applied_moves.push_back(move);
                        pattern_downsized += 1;
                        made_changes = true;
                        verify_cached_timing(move,
                                             model,
                                             tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period,
                                             phase2_timing.timing);
                    }

                    continue;
                }

                if (pattern == Pattern::SizeSwap) {
                    if (node->children.size() != 1) continue;
                    ClockNode* child = node->children.front().get();
                    if (!is_buffer_node(child)) continue;

                    const BufSpec* first_lib = find_lib_by_name(libs, node->type);
                    const BufSpec* second_lib = find_lib_by_name(libs, child->type);
                    if (!first_lib || !second_lib) continue;
                    if (!is_physically_smaller(*first_lib, *second_lib, model)) continue;
                    if (node->children.size() > second_lib->ss_delay.size() || child->children.size() > first_lib->ss_delay.size()) continue;

                    const std::string first_name = node->name;
                    const std::string second_name = child->name;
                    if (try_change_types({{first_name, second_lib->name}, {second_name, first_lib->name}})) {
                        summary.phase2_downsizes += 2;
                        const std::string move = "Phase2 size-swap " + first_name + " <-> " + second_name;
                        summary.applied_moves.push_back(move);
                        pattern_downsized += 2;
                        made_changes = true;
                        verify_cached_timing(move,
                                             model,
                                             tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period,
                                             phase2_timing.timing);
                    }
                    continue;
                }

                if (pattern == Pattern::Rebalance) {
                    if (node->children.size() != 1) continue;
                    ClockNode* child = node->children.front().get();
                    if (!is_buffer_node(child)) continue;

                    const BufSpec* first_lib = find_lib_by_name(libs, node->type);
                    const BufSpec* second_lib = find_lib_by_name(libs, child->type);
                    if (!first_lib || !second_lib) continue;

                    const double first_area = model.estimate_buffer_area(*first_lib);
                    const double second_area = model.estimate_buffer_area(*second_lib);
                    const double smaller_area = std::min(first_area, second_area);
                    const double larger_area = std::max(first_area, second_area);
                    if (smaller_area <= 0.0 || larger_area / smaller_area < phase2_gap_threshold) continue;

                    const BufSpec* medium = choose_medium_buffer(libs, model, node->children.size(), child->children.size());
                    if (!medium) continue;
                    if (medium->name == node->type && medium->name == child->type) continue;
                    if (node->children.size() > medium->ss_delay.size() || child->children.size() > medium->ss_delay.size()) continue;

                    const std::string first_name = node->name;
                    const std::string second_name = child->name;
                    if (try_change_types({{first_name, medium->name}, {second_name, medium->name}})) {
                        summary.phase2_downsizes += 2;
                        const std::string move = "Phase2 rebalance " + first_name + " and " + second_name + " -> " + medium->name;
                        summary.applied_moves.push_back(move);
                        pattern_downsized += 2;
                        made_changes = true;
                        verify_cached_timing(move,
                                             model,
                                             tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period,
                                             phase2_timing.timing);
                    }
                    continue;
                }

                if (pattern == Pattern::GreedyFallback) {
                    const std::string node_name_copy = node->name;

                    if (!node->original) {
                        if (try_full_removal(node_name_copy, pattern_removed)) {
                            made_changes = true;
                            continue;
                        }
                    }

                    node = tree.find_node(node_name_copy);
                    if (!is_buffer_node(node)) continue;

                    const BufSpec* smaller = choose_next_smaller_buffer(libs, model, node->type, node->children.size());
                    if (!smaller) continue;

                    const std::string old_type = node->type;
                    if (try_change_types({{node->name, smaller->name}})) {
                        summary.phase2_downsizes += 1;
                        const std::string move = "Phase2 greedy-downsize " + node->name + " to " + smaller->name;
                        summary.applied_moves.push_back(move);
                        pattern_downsized += 1;
                        made_changes = true;
                        verify_cached_timing(move,
                                             model,
                                             tree,
                                             ss_paths,
                                             ff_paths,
                                             clock_period,
                                             phase2_timing.timing);
                    } else {
                        tree.set_buffer_type(node->name, old_type);
                    }
                    continue;
                }
            }

            if (phase2_stopped) break;
            if (!made_changes) break;
        }

        capture_phase2_iteration(pattern, 0, pattern_removed, pattern_downsized);

        if (phase2_stopped) break;
    }

    capture_stage(summary.after_phase2,
                  "Phase2",
                  0,
                  summary.phase2_removals,
                  summary.phase2_downsizes,
                  &phase2_timing.timing);
    if (summary.runtime_profile.enabled) {
        summary.runtime_profile.phase2_seconds +=
            seconds_since(phase2_profile_start);
    }
    }

    if (cfg.enable_best_checkpoint) {
        const TimingAnalysisResult pre_restore_timing =
            model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
        const double pre_restore_area = compute_area_full();
        const double pre_restore_score =
            weighted_objective_score(model,
                                     pre_restore_timing,
                                     pre_restore_area,
                                     baseline,
                                     cfg);
        summary.checkpoint.pre_restore_score = pre_restore_score;
        update_best_checkpoint(pre_restore_timing,
                               pre_restore_area,
                               "pipeline_end");

        if (cfg.restore_best_checkpoint_at_end &&
            score_strictly_better(best_checkpoint_score,
                                  pre_restore_score,
                                  cfg)) {
            tree = best_checkpoint_tree.clone();
            summary.checkpoint.restored = true;
            summary.applied_moves.push_back(
                "Restore best weighted-score checkpoint from " +
                summary.checkpoint.best_stage);
        }
    }

    const auto final_validation_profile_start = std::chrono::steady_clock::now();
    const auto old_timing_start = std::chrono::steady_clock::now();
    summary.final_timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
    summary.second_round.old_timing_analysis_time_seconds +=
        seconds_since(old_timing_start);
    if (cfg.enable_indexed_timing_paths) {
        const auto index_start = std::chrono::steady_clock::now();
        TreeIndexCache tree_index = build_tree_index_cache(tree);
        summary.second_round.tree_index_rebuilds += 1;
        summary.second_round.tree_index_rebuild_time_seconds +=
            seconds_since(index_start);
        const std::vector<IndexedPathInfo> indexed_ss_paths =
            build_indexed_paths(ss_paths, tree_index, true);
        const std::vector<IndexedPathInfo> indexed_ff_paths =
            build_indexed_paths(ff_paths, tree_index, false);

        std::vector<double> indexed_ss_arrival;
        std::vector<double> indexed_ff_arrival;
        const auto indexed_arrival_start = std::chrono::steady_clock::now();
        compute_clock_arrivals_indexed(tree_index,
                                       lib_cache,
                                       true,
                                       indexed_ss_arrival);
        compute_clock_arrivals_indexed(tree_index,
                                       lib_cache,
                                       false,
                                       indexed_ff_arrival);
        summary.second_round.indexed_arrival_time_seconds +=
            seconds_since(indexed_arrival_start);

        const auto indexed_timing_start = std::chrono::steady_clock::now();
        const TimingAnalysisResult indexed_timing =
            analyze_timing_indexed(indexed_ss_arrival,
                                   indexed_ff_arrival,
                                   indexed_ss_paths,
                                   indexed_ff_paths,
                                   clock_period);
        summary.second_round.indexed_timing_analysis_time_seconds +=
            seconds_since(indexed_timing_start);

        if (cfg.enable_indexed_timing_verify &&
            !timing_metrics_close(indexed_timing, summary.final_timing, 1e-6)) {
            std::cerr << "Indexed timing mismatch during final validation\n";
            print_timing_metric_mismatch("indexed final validation",
                                         indexed_timing,
                                         summary.final_timing,
                                         1e-6);
            std::abort();
        }
    }
    if (summary.second_round.final_alt_candidates_tried > 0) {
        summary.second_round.final_alt_accept_rate =
            static_cast<double>(
                summary.second_round.final_alt_candidates_accepted) /
            static_cast<double>(
                summary.second_round.final_alt_candidates_tried);
    }
    summary.final_area = compute_area_full();
    summary.final_legality = validate_full();
    summary.final_score = model.compute_score_metrics(summary.final_timing,
                                                      baseline.timing,
                                                      summary.final_timing.clock_period > 0.0 ? summary.final_area : baseline.area,
                                                      baseline.area,
                                                      cfg.alpha,
                                                      cfg.beta,
                                                      cfg.gamma);
    if (summary.runtime_profile.enabled) {
        summary.runtime_profile.final_validation_seconds +=
            seconds_since(final_validation_profile_start);
    }
    if (cfg.enable_fast_timing_engine &&
        cfg.enable_fast_timing_verify &&
        fast_timing_engine &&
        !sync_fast_timing("final validation")) {
        std::abort();
    }
    summary.runtime_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    if (fast_timing_engine) {
        const FastTimingCounters& fast = fast_timing_engine->counters();
        summary.fast_timing.build_time_seconds += fast.build_time_seconds;
        summary.fast_timing.sync_time_seconds += fast.sync_time_seconds;
        summary.fast_timing.trial_time_seconds += fast.trial_time_seconds;
        summary.fast_timing.group_collection_time_seconds +=
            fast.group_collection_time_seconds;
        summary.fast_timing.group_update_time_seconds +=
            fast.group_update_time_seconds;
        summary.fast_timing.sync_count += fast.sync_count;
        summary.fast_timing.trial_count += fast.trial_count;
        summary.fast_timing.resize_trials += fast.resize_trials;
        summary.fast_timing.resize_batch_trials += fast.resize_batch_trials;
        summary.fast_timing.resize_batch_candidates +=
            fast.resize_batch_candidates;
        summary.fast_timing.insert_trials += fast.insert_trials;
        summary.fast_timing.delete_trials += fast.delete_trials;
        summary.fast_timing.commit_count += fast.commit_count;
        summary.fast_timing.rollback_count += fast.rollback_count;
        summary.fast_timing.verify_count += fast.verify_count;
        summary.fast_timing.fallback_count += fast.fallback_count;
        summary.fast_timing.arrival_snapshot_count +=
            fast.arrival_snapshot_count;
        summary.fast_timing.affected_group_count +=
            fast.affected_group_count;
        summary.fast_timing.max_arrival_snapshots_per_trial =
            std::max(summary.fast_timing.max_arrival_snapshots_per_trial,
                     fast.max_arrival_snapshots_per_trial);
        summary.fast_timing.max_affected_groups_per_trial =
            std::max(summary.fast_timing.max_affected_groups_per_trial,
                     fast.max_affected_groups_per_trial);
        summary.fast_timing.resize_trials_enabled = fast.resize_enabled;
        summary.fast_timing.delete_trials_enabled = fast.delete_enabled;
        summary.fast_timing.insert_trials_enabled = fast.insert_enabled;
    }
    summary.success = summary.final_legality.ok;
    if (!summary.early_stopped && summary.success) {
        summary.message = "Optimization completed successfully.";
    } else if (!summary.early_stopped) {
        summary.message = "Optimization completed with legality issues.";
    } else if (summary.message.empty()) {
        summary.message = "Optimization stopped early.";
    }
    summary.final_timing = compact_timing_snapshot(summary.final_timing);
    return summary;
}

void Optimizer::analyze(const ClockTree& tree,
                        const std::vector<BufSpec>& libs,
                        const std::vector<PathInfo>& ss_paths,
                        const std::vector<PathInfo>& ff_paths,
                        double clock_period,
                        const std::string& report_path,
                        const std::string& testcase_name,
                        const OptimizationSummary* optimization) {
    const auto report_profile_start = std::chrono::steady_clock::now();
    DelayModel model(libs);
    TimingAnalysisResult timing = model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
    double current_area = model.compute_tree_area(tree);

    if (!baseline.valid) {
        baseline.timing = timing;
        baseline.area = current_area;
        baseline.valid = true;
    }

    ScoreMetrics score = model.compute_score_metrics(timing, baseline.timing, current_area, baseline.area,
                                                    cfg.alpha, cfg.beta, cfg.gamma);

#ifndef ENABLE_OUTPUT
    // The submission binary intentionally compiles reporting out. Retain the
    // same public analyze() implementation without release-build warnings.
    (void)report_profile_start;
    (void)score;
    (void)report_path;
    (void)testcase_name;
    (void)optimization;
#endif

    DEBUG_PRINT("Timing analysis");
    DEBUG_PRINT("  Clock period: " << timing.clock_period);
    DEBUG_PRINT("  Tsetup: " << timing.t_setup);
    DEBUG_PRINT("  Thold: " << timing.t_hold);
    DEBUG_PRINT("  SS: TNS=" << timing.ss.tns << " WNS=" << timing.ss.wns << " Violations=" << timing.ss.violating_paths);
    DEBUG_PRINT("  FF: TNS=" << timing.ff.tns << " WNS=" << timing.ff.wns << " Violations=" << timing.ff.violating_paths);
    DEBUG_PRINT("  Area: " << current_area);

    #ifdef ENABLE_OUTPUT
    std::ofstream report(report_path);
    if (!report) {
        std::cerr << "Failed to open per-path timing report file: " << report_path << std::endl;
    } else {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        constexpr int name_width = 36;
        constexpr int report_width = 153;

        auto write_rule = [&](char fill = '-') {
            report << std::string(report_width, fill) << '\n';
        };

        auto write_section = [&](const std::string& title) {
            report << '\n';
            write_rule('=');
            report << title << '\n';
            write_rule('=');
        };

        auto write_subsection = [&](const std::string& title) {
            report << '\n' << title << '\n';
            write_rule('-');
        };

        auto write_table_header = [&]() {
            write_rule();
            report << std::left << std::setw(name_width) << "Stage / snapshot"
                   << std::right << std::setw(9) << "Elapsed"
                   << std::setw(8) << "+Buf"
                   << std::setw(8) << "-Buf"
                   << std::setw(8) << "Resize"
                   << " |"
                   << std::setw(11) << "SS TNS"
                   << std::setw(10) << "SS WNS"
                   << std::setw(8) << "SS V"
                   << " |"
                   << std::setw(11) << "FF TNS"
                   << std::setw(10) << "FF WNS"
                   << std::setw(8) << "FF V"
                   << " |"
                   << std::setw(12) << "Area"
                   << std::setw(8) << "Legal"
                   << '\n';
            write_rule();
        };

        auto write_baseline_row = [&]() {
            report << std::left << std::setw(name_width) << "Baseline"
                   << std::right << std::setw(9) << "-"
                   << std::setw(8) << "-"
                   << std::setw(8) << "-"
                   << std::setw(8) << "-"
                   << " |"
                   << std::fixed << std::setprecision(4)
                   << std::setw(11) << baseline.timing.ss.tns
                   << std::setw(10) << baseline.timing.ss.wns
                   << std::setw(8) << baseline.timing.ss.violating_paths
                   << " |"
                   << std::setw(11) << baseline.timing.ff.tns
                   << std::setw(10) << baseline.timing.ff.wns
                   << std::setw(8) << baseline.timing.ff.violating_paths
                   << " |"
                   << std::setprecision(3)
                   << std::setw(12) << baseline.area
                   << std::setw(8)
                   << (optimization && optimization->phase0.before_phase0.valid
                           ? (optimization->phase0.before_phase0.legality.ok ? "OK" : "FAIL")
                           : "-")
                   << '\n';
        };

        auto write_snapshot_row = [&](const StageSnapshot& snapshot) {
            report << std::left << std::setw(name_width) << snapshot.name
                   << std::right << std::fixed << std::setprecision(3)
                   << std::setw(9) << snapshot.runtime_seconds
                   << std::setw(8) << snapshot.added_buffers
                   << std::setw(8) << snapshot.removed_buffers
                   << std::setw(8) << snapshot.downsized_buffers
                   << " |"
                   << std::setprecision(4)
                   << std::setw(11) << snapshot.timing.ss.tns
                   << std::setw(10) << snapshot.timing.ss.wns
                   << std::setw(8) << snapshot.timing.ss.violating_paths
                   << " |"
                   << std::setw(11) << snapshot.timing.ff.tns
                   << std::setw(10) << snapshot.timing.ff.wns
                   << std::setw(8) << snapshot.timing.ff.violating_paths
                   << " |"
                   << std::setprecision(3)
                   << std::setw(12) << snapshot.area
                   << std::setw(8) << (snapshot.legality.ok ? "OK" : "FAIL")
                   << '\n';
        };

        auto write_iteration_table = [&](const std::string& title,
                                         const std::vector<StageSnapshot>& snapshots) {
            if (snapshots.empty()) return;
            write_subsection(title);
            write_table_header();
            for (const auto& snapshot : snapshots) {
                write_snapshot_row(snapshot);
            }
            write_rule();
        };

        auto write_stage_contribution_summary = [&]() {
            if (!optimization) return;
            write_section("Stage Contribution Summary");
            write_table_header();
            write_baseline_row();

            auto write_stage = [&](const StageSnapshot& stage) {
                if (stage.valid) write_snapshot_row(stage);
            };

            write_stage(optimization->phase0.after_reset);
            write_stage(optimization->phase0.after_phase0);
            write_stage(optimization->after_existing_buffer_shared_reclaim);
            write_stage(optimization->after_phase1a);
            if (optimization->final_alt.enabled &&
                optimization->final_alt.replace_phase1b_and_phase2) {
                write_stage(optimization->after_final_alt);
                write_stage(optimization->after_perturb_recover);
            } else {
                write_stage(optimization->after_phase1b);
            write_stage(optimization->after_phase2);
            }
            write_rule();
            const bool selected_branch_reruns_phase0 =
                optimization->post_phase0_portfolio.enabled &&
                std::any_of(
                    optimization->post_phase0_portfolio.branches.begin(),
                    optimization->post_phase0_portfolio.branches.end(),
                    [&](const PostPhase0PortfolioBranchRecord& branch) {
                        return branch.name ==
                                   optimization->post_phase0_portfolio
                                       .overall_winner &&
                               branch.initial_state_reruns_phase0;
                    });
            if (selected_branch_reruns_phase0) {
                report << "Note: This selected branch restarted from a "
                          "perturbed pre-Phase0 state, so stage elapsed times "
                          "are route-local.  The portfolio section reports the "
                          "complete sequential wall time; final legality is "
                          "verified again before output.\n";
            } else {
                report << "Note: Elapsed is cumulative wall time from optimizer "
                          "startup (Phase0 uses the same startup boundary); "
                          "final legality is verified again before output.\n";
            }
        };

        auto write_post_phase0_portfolio = [&]() {
            if (!optimization ||
                !optimization->post_phase0_portfolio.enabled) {
                return;
            }
            const PostPhase0PortfolioSummary& portfolio =
                optimization->post_phase0_portfolio;
            write_section(
                "Post-Phase0 Full-Pipeline Portfolio Experiment");
            write_subsection("Configuration and Selection");
            report << std::left << std::setw(36)
                   << "Branch wall-clock limits"
                   << ": "
                   << (portfolio.time_limits_disabled
                           ? "disabled for experiment"
                           : "production limits")
                   << '\n';
            report << std::left << std::setw(36)
                   << "Sequential experiment runtime (s)"
                   << ": " << portfolio.runtime_seconds << '\n';
            report << std::left << std::setw(36)
                   << "Phase0 checkpoint score"
                   << ": " << portfolio.phase0_score << '\n';
            report << std::left << std::setw(36)
                   << "Phase0 checkpoint area"
                   << ": " << portfolio.phase0_area << '\n';
            report << std::left << std::setw(36)
                   << "Winner objective weights"
                   << ": " << portfolio.target_alpha << "/"
                   << portfolio.target_beta << "/"
                   << portfolio.target_gamma << '\n';
            report << std::left << std::setw(36)
                   << "Maximum scheduled routes"
                   << ": "
                   << (cfg.portfolio_max_routes <= 0
                           ? "deadline-controlled (no count cap)"
                           : std::to_string(cfg.portfolio_max_routes))
                   << '\n';
            report << std::left << std::setw(36)
                   << "Deadline-filling restarts"
                   << ": "
                   << (cfg.portfolio_fill_remaining_time_with_restarts
                           ? "enabled; 5%/10%/20%, rotating seeds"
                           : "disabled")
                   << '\n';
            report << std::left << std::setw(36)
                   << "Minimum extra-route allocation (s)"
                   << ": " << cfg.portfolio_min_route_seconds << '\n';
            report << std::left << std::setw(36)
                   << "Safety reserve (small/medium/large)"
                   << ": "
                   << cfg.portfolio_small_finalization_reserve_seconds << "/"
                   << cfg.portfolio_medium_finalization_reserve_seconds << "/"
                   << cfg.portfolio_large_finalization_reserve_seconds
                   << '\n';
            report << std::left << std::setw(36)
                   << "Pre-Phase0 route on large case"
                   << ": "
                   << (cfg.portfolio_allow_prephase_routes_on_large_case
                           ? "allowed"
                           : "disabled to limit tree-copy memory")
                   << '\n';
            report << std::left << std::setw(36)
                   << "Objective-weight scheduler"
                   << ": "
                   << (cfg.enable_score_weight_scheduler
                           ? "on; start " +
                                 std::to_string(
                                     cfg.score_weight_schedule_start_alpha) +
                                 "/" +
                                 std::to_string(
                                     cfg.score_weight_schedule_start_beta) +
                                 "/" +
                                 std::to_string(
                                     cfg.score_weight_schedule_start_gamma) +
                                 ", hold " +
                                 std::to_string(
                                     cfg.score_weight_schedule_hold_repair_cycles) +
                                 " RR cycles, cosine to " +
                                 std::to_string(
                                     cfg.score_weight_schedule_end_alpha) +
                                 "/" +
                                 std::to_string(
                                     cfg.score_weight_schedule_end_beta) +
                                 "/" +
                                 std::to_string(
                                     cfg.score_weight_schedule_end_gamma)
                           : "off")
                   << '\n';
            report << std::left << std::setw(36)
                   << "Full-fork winner (H vs Score)"
                   << ": " << portfolio.full_fork_winner << '\n';
            report << std::left << std::setw(36)
                   << "Overall selected branch"
                   << ": " << portfolio.overall_winner << '\n';

            write_subsection("Complete Branch Comparison");
            report << std::left
                   << std::setw(22) << "Branch"
                   << std::setw(22) << "RR policy"
                   << std::right
                   << std::setw(12) << "Runtime"
                   << std::setw(13) << "Score"
                   << std::setw(13) << "Target"
                   << std::setw(11) << "SS_TNS"
                   << std::setw(10) << "SS_WNS"
                   << std::setw(8) << "SS_V"
                   << std::setw(11) << "FF_TNS"
                   << std::setw(10) << "FF_WNS"
                   << std::setw(8) << "FF_V"
                   << std::setw(12) << "Area"
                   << std::setw(9) << "Legal"
                   << "  Stage stops"
                   << '\n';
            write_rule('-');
            for (const PostPhase0PortfolioBranchRecord& branch :
                 portfolio.branches) {
                const std::string rr_policy =
                    branch.alternating_repair_reclaim
                        ? "H/Score + delete"
                    : branch.repair_reclaim_score_based
                        ? "score"
                        : "heuristic";
                const std::string displayed_policy =
                    branch.objective_weight_scheduler
                        ? rr_policy + " + cosine"
                        : rr_policy;
                report << std::left
                       << std::setw(22) << branch.name
                       << std::setw(22) << displayed_policy
                       << std::right << std::fixed
                       << std::setprecision(3)
                       << std::setw(12) << branch.runtime_seconds
                       << std::setprecision(6)
                       << std::setw(13)
                       << branch.final_score.total_score
                       << std::setw(13)
                       << branch.target_score
                       << std::setprecision(4)
                       << std::setw(11)
                       << branch.final_timing.ss.tns
                       << std::setw(10)
                       << branch.final_timing.ss.wns
                       << std::setw(8)
                       << branch.final_timing.ss.violating_paths
                       << std::setw(11)
                       << branch.final_timing.ff.tns
                       << std::setw(10)
                       << branch.final_timing.ff.wns
                       << std::setw(8)
                       << branch.final_timing.ff.violating_paths
                       << std::setprecision(3)
                       << std::setw(12) << branch.final_area
                       << std::setw(9)
                       << (branch.legal ? "OK" : "FAIL")
                       << "  RR=" << branch.repair_reclaim_stop_reason
                       << ", FinalAlt=" << branch.final_alt_stop_reason
                       << ", S8=" << branch.perturb_recover_stop_reason
                       << '\n';
            }

            for (const PostPhase0PortfolioBranchRecord& branch :
                 portfolio.branches) {
                write_subsection(
                    "Branch " + branch.name +
                    ": Repair/Reclaim Cycle Detail");
                report << "Policy: " << branch.policy << '\n';
                report << "Initial state: " << branch.initial_state_policy
                       << " (resizes=" << branch.initial_state_resizes
                       << ", fraction="
                       << branch.initial_state_perturb_fraction
                       << ", seed=" << branch.initial_state_seed
                       << ", reruns Phase0="
                       << (branch.initial_state_reruns_phase0 ? "yes" : "no")
                       << ")\n";
                report << "Initial timing: SS TNS/WNS/NVP="
                       << branch.initial_state_timing.ss.tns << "/"
                       << branch.initial_state_timing.ss.wns << "/"
                       << branch.initial_state_timing.ss.violating_paths
                       << ", FF TNS/WNS/NVP="
                       << branch.initial_state_timing.ff.tns << "/"
                       << branch.initial_state_timing.ff.wns << "/"
                       << branch.initial_state_timing.ff.violating_paths
                       << ", Area=" << branch.initial_state_area << '\n';
                report << std::left
                       << std::setw(6) << "Cycle"
                       << std::setw(15) << "Mode"
                       << std::setw(17) << "Objective a/b/g"
                       << std::right
                       << std::setw(7) << "PDel"
                       << std::setw(10) << "PDelArea"
                       << std::setw(8) << "+Buf"
                       << std::setw(9) << "R-Try"
                       << std::setw(8) << "R-Acc"
                       << std::setw(13) << "ScoreStart"
                       << std::setw(13) << "ScorePert"
                       << std::setw(13) << "ScorePulse"
                       << std::setw(13) << "ScoreEnd"
                       << std::setw(11) << "SS_TNS"
                       << std::setw(10) << "SS_WNS"
                       << std::setw(8) << "SS_V"
                       << std::setw(11) << "FF_TNS"
                       << std::setw(10) << "FF_WNS"
                       << std::setw(8) << "FF_V"
                       << std::setw(12) << "AreaEnd"
                       << "  Pulse stop"
                       << '\n';
                write_rule('-');
                for (const RepairReclaimCycleRecord& cycle :
                     branch.repair_reclaim.cycles) {
                    std::ostringstream objective_label;
                    objective_label << std::fixed << std::setprecision(2)
                                    << cycle.objective_alpha << "/"
                                    << cycle.objective_beta << "/"
                                    << cycle.objective_gamma;
                    const double pulse_score =
                        model.compute_score_metrics(
                                 cycle.after_pulse_timing,
                                 baseline.timing,
                                 cycle.after_pulse_area,
                                 baseline.area,
                                 cycle.objective_alpha,
                                 cycle.objective_beta,
                                 cycle.objective_gamma)
                            .total_score;
                    const double end_score =
                        model.compute_score_metrics(
                                 cycle.after_reclaim_timing,
                                 baseline.timing,
                                 cycle.after_reclaim_area,
                                 baseline.area,
                                 cycle.objective_alpha,
                                 cycle.objective_beta,
                                 cycle.objective_gamma)
                            .total_score;
                    report << std::left
                           << std::setw(6) << cycle.cycle_index
                           << std::setw(15) << cycle.branch_mode
                           << std::setw(17) << objective_label.str()
                           << std::right
                           << std::setw(7)
                           << cycle.pre_cycle_deletes
                           << std::fixed << std::setprecision(3)
                           << std::setw(10)
                           << cycle.pre_cycle_deleted_area
                           << std::setw(8)
                           << cycle.pulse_inserted_buffers
                           << std::setw(9)
                           << cycle.reclaim_candidates_tried
                           << std::setw(8)
                           << cycle.reclaim_candidates_accepted
                           << std::setprecision(6)
                           << std::setw(13)
                           << cycle.pre_cycle_score_before
                           << std::setw(13)
                           << cycle.pre_cycle_score_after
                           << std::setw(13) << pulse_score
                           << std::setw(13) << end_score
                           << std::setprecision(4)
                           << std::setw(11)
                           << cycle.after_reclaim_timing.ss.tns
                           << std::setw(10)
                           << cycle.after_reclaim_timing.ss.wns
                           << std::setw(8)
                           << cycle.after_reclaim_timing.ss.violating_paths
                           << std::setw(11)
                           << cycle.after_reclaim_timing.ff.tns
                           << std::setw(10)
                           << cycle.after_reclaim_timing.ff.wns
                           << std::setw(8)
                           << cycle.after_reclaim_timing.ff.violating_paths
                           << std::setprecision(3)
                           << std::setw(12)
                           << cycle.after_reclaim_area
                           << "  "
                           << (cycle.pulse_stop_reason.empty()
                                   ? "-"
                                   : cycle.pulse_stop_reason)
                           << '\n';
                }
                report << "FinalAlt: iterations="
                       << branch.final_alt.iterations
                       << ", insertions="
                       << branch.final_alt.repair_insertions
                       << ", reclaim accepted="
                       << branch.final_alt.reclaim_moves_accepted
                       << ", stop=" << branch.final_alt.stop_reason
                       << '\n';
                report << "Strategy8: cycles="
                       << branch.perturb_recover.cycles
                       << ", accepted="
                       << branch.perturb_recover.accepted_cycles
                       << ", score="
                       << branch.perturb_recover.score_before
                       << " -> "
                       << branch.perturb_recover.score_after
                       << ", stop="
                       << branch.perturb_recover.stop_reason
                       << '\n';
            }
        };

        report << "TIMING OPTIMIZATION REPORT\n";
        write_rule('=');
        report << "Generated : "
               << std::put_time(std::localtime(&now_time),
                                "%Y-%m-%d %H:%M:%S")
               << '\n';
        report << "Testcase  : " << testcase_name << '\n';

        // Keep the stage-level QoR table at the top of the report so the
        // pipeline trajectory is visible before detailed diagnostics.
        write_stage_contribution_summary();
        write_post_phase0_portfolio();

        write_section("Run Summary");
        if (optimization) {
            report << std::left << std::setw(24) << "Status"
                   << ": "
                   << (optimization->early_stopped ? "EARLY_STOPPED"
                                                   : "COMPLETED")
                   << '\n';
            report << std::left << std::setw(24) << "Final legality"
                   << ": "
                   << (!optimization->final_legality.ok ? "FAIL" : "OK")
                   << '\n';
            report << std::left << std::setw(24) << "Runtime (s)"
                   << ": " << std::fixed << std::setprecision(4)
                   << optimization->runtime_seconds << '\n';
            report << std::left << std::setw(24) << "Applied modifications"
                   << ": "
                   << optimization->applied_moves.size() +
                          optimization->phase0.accepted_resizes +
                          optimization->phase0.reset_resizes
                   << '\n';
            report << std::left << std::setw(24) << "Phase 0 enabled"
                   << ": "
                   << (optimization->phase0_enabled ? "yes" : "no")
                   << '\n';
            report << std::left << std::setw(24) << "Reset experiment"
                   << ": "
                   << (optimization->phase0_reset_experiment_enabled ? "yes" : "no")
                   << '\n';
        }
        if (optimization && optimization->phase0.reset_based) {
            report << std::left << std::setw(24) << "Experimental branch"
                   << ": " << optimization->phase0.branch_name;
            if (!optimization->phase0.fastest_buffer_type.empty()) {
                report << "  fastest_type=" << optimization->phase0.fastest_buffer_type;
            }
            report << '\n';
        }

        write_subsection("Final Constraints and Timing");
        report << std::left << std::setw(12) << "Clock period"
               << ": " << std::fixed << std::setprecision(4)
               << timing.clock_period << '\n';
        report << std::left << std::setw(12) << "Setup time"
               << ": " << timing.t_setup << '\n';
        report << std::left << std::setw(12) << "Hold time"
               << ": " << timing.t_hold << '\n';
        report << '\n'
               << std::left << std::setw(8) << "Corner"
               << std::right << std::setw(14) << "TNS"
               << std::setw(14) << "WNS"
               << std::setw(14) << "Violations"
               << '\n';
        write_rule('-');
        report << std::left << std::setw(8) << "SS"
               << std::right << std::fixed << std::setprecision(4)
               << std::setw(14) << timing.ss.tns
               << std::setw(14) << timing.ss.wns
               << std::setw(14) << timing.ss.violating_paths << '\n';
        report << std::left << std::setw(8) << "FF"
               << std::right
               << std::setw(14) << timing.ff.tns
               << std::setw(14) << timing.ff.wns
               << std::setw(14) << timing.ff.violating_paths << '\n';
        report << std::left << std::setw(24) << "Total violations"
               << ": " << total_violations(timing) << '\n';
        report << std::left << std::setw(24) << "Final area"
               << ": " << std::fixed << std::setprecision(3)
               << current_area << '\n';

        write_subsection("Weighted Objective");
        report << std::left << std::setw(24) << "Total score"
               << ": " << std::fixed << std::setprecision(6)
               << score.total_score << '\n';
        report << std::left << std::setw(24) << "SS component"
               << ": " << score.ss_component << '\n';
        report << std::left << std::setw(24) << "FF component"
               << ": " << score.ff_component << '\n';
        report << std::left << std::setw(24) << "Area component"
               << ": " << score.area_component << '\n';
        report << std::left << std::setw(24) << "Weights (a/b/g)"
               << ": " << cfg.alpha << " / " << cfg.beta
               << " / " << cfg.gamma << '\n';
        report << std::left << std::setw(38) << "Objective scheduler"
               << ": ";
        if (cfg.enable_score_weight_scheduler) {
            report << "on; "
                   << cfg.score_weight_schedule_start_alpha << "/"
                   << cfg.score_weight_schedule_start_beta << "/"
                   << cfg.score_weight_schedule_start_gamma
                   << " for "
                   << cfg.score_weight_schedule_hold_repair_cycles
                   << " Repair/Reclaim cycles, then cosine-ramp over "
                   << cfg.score_weight_schedule_transition_repair_cycles
                   << " cycles to "
                   << cfg.score_weight_schedule_end_alpha << "/"
                   << cfg.score_weight_schedule_end_beta << "/"
                   << cfg.score_weight_schedule_end_gamma;
        } else {
            report << "off";
        }
        report << '\n';
        report << std::left << std::setw(38)
               << "Phase 0 score acceptance"
               << ": "
               << (cfg.enable_phase0_score_based_acceptance ? "on" : "off")
               << '\n';
        report << std::left << std::setw(38)
               << "Repair/Reclaim score acceptance"
               << ": ";
        if (optimization &&
            optimization->post_phase0_portfolio.enabled) {
            const PostPhase0PortfolioSummary& portfolio =
                optimization->post_phase0_portfolio;
            const auto selected =
                std::find_if(
                    portfolio.branches.begin(),
                    portfolio.branches.end(),
                    [&](const PostPhase0PortfolioBranchRecord& branch) {
                        return branch.name == portfolio.overall_winner;
                    });
            if (selected != portfolio.branches.end() &&
                selected->alternating_repair_reclaim) {
                report << "alternating";
            } else if (selected != portfolio.branches.end() &&
                       selected->repair_reclaim_score_based) {
                report << "on";
            } else {
                report << "off";
            }
        } else {
            report << (cfg.enable_repair_reclaim_score_based_acceptance
                           ? "on"
                           : "off");
        }
        report << '\n';
        report << std::left << std::setw(38)
               << "FinalAlt score acceptance"
               << ": "
               << (cfg.enable_final_alt_score_based_acceptance ? "on" : "off")
               << '\n';
        report << std::left << std::setw(38)
               << "Strategy8 perturb/recover"
               << ": "
               << (optimization
                       ? (optimization->perturb_recover.enabled ? "on"
                                                               : "off")
                       : (cfg.enable_perturb_recover ? "on" : "off"))
               << '\n';
        if (optimization && optimization->checkpoint.enabled) {
            const BestCheckpointSummary& checkpoint =
                optimization->checkpoint;
            write_subsection("Best-State Checkpoint");
            report << std::left << std::setw(24) << "Updates"
                   << ": " << checkpoint.updates << '\n';
            report << std::left << std::setw(24) << "Best stage"
                   << ": " << checkpoint.best_stage << '\n';
            report << std::left << std::setw(24) << "Initial score"
                   << ": " << checkpoint.initial_score << '\n';
            report << std::left << std::setw(24) << "Best score"
                   << ": " << checkpoint.best_score << '\n';
            report << std::left << std::setw(24) << "Pre-restore score"
                   << ": " << checkpoint.pre_restore_score << '\n';
            report << std::left << std::setw(24) << "Restored"
                   << ": "
                   << (checkpoint.restored ? "yes" : "no")
                   << '\n';
        }

        if (optimization) {
            if (optimization->runtime_profile.enabled) {
                const RuntimeProfileSummary& profile =
                    optimization->runtime_profile;
                write_section("Runtime Profile");
                report << std::left << std::setw(32) << "Phase 0"
                       << std::right << std::fixed << std::setprecision(4)
                       << std::setw(12) << profile.phase0_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "Repair/Reclaim total"
                       << std::right << std::setw(12)
                       << profile.repair_reclaim_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "  Repair pulse"
                       << std::right << std::setw(12)
                       << profile.phase1a_pulse_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "  Pressure calculation"
                       << std::right << std::setw(12)
                       << profile.pressure_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "  Reclaim candidate generation"
                       << std::right << std::setw(12)
                       << profile.reclaim_candidate_generation_seconds
                       << " s\n";
                report << std::left << std::setw(32)
                       << "  Reclaim candidate sorting"
                       << std::right << std::setw(12)
                       << profile.reclaim_sorting_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "  Reclaim trial loop"
                       << std::right << std::setw(12)
                       << profile.reclaim_trial_loop_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "Legacy Phase 1B"
                       << std::right << std::setw(12)
                       << profile.phase1b_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "Legacy Phase 2"
                       << std::right << std::setw(12)
                       << profile.phase2_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "Final Alternating Greedy"
                       << std::right << std::setw(12)
                       << optimization->final_alt.runtime_seconds << " s\n";
                report << std::left << std::setw(32)
                       << "Strategy8 Perturb/Recover"
                       << std::right << std::setw(12)
                       << optimization->perturb_recover.runtime_seconds
                       << " s\n";
                report << std::left << std::setw(32)
                       << "Final validation"
                       << std::right << std::setw(12)
                       << profile.final_validation_seconds << " s\n";
            }

            const RuntimeValidationDiagnostics& diag = optimization->diagnostics;
            write_section("Diagnostics: Validation and Incremental Updates");
            write_subsection("Enabled Modes");
            report << std::left << std::setw(38) << "Incremental area tracking"
                   << ": " << (diag.incremental_area_enabled ? "yes" : "no")
                   << '\n';
            report << std::left << std::setw(38) << "Incremental area verification"
                   << ": " << (diag.incremental_area_verify ? "yes" : "no")
                   << '\n';
            report << std::left << std::setw(38) << "Phase 0 full trial validation"
                   << ": "
                   << (diag.phase0_trial_full_validation_verify ? "yes" : "no")
                   << '\n';
            report << std::left << std::setw(38) << "FinalAlt full trial validation"
                   << ": "
                   << (diag.final_alt_trial_full_validation_verify ? "yes" : "no")
                   << '\n';

            write_subsection("Validation Work");
            report << std::left << std::setw(38) << "Full legality validations"
                   << ": " << diag.full_legality_validations_total << '\n';
            report << std::left << std::setw(38) << "  Phase 0 trial validations"
                   << ": " << diag.full_legality_validations_phase0_trials
                   << '\n';
            report << std::left << std::setw(38) << "  FinalAlt trial validations"
                   << ": " << diag.full_legality_validations_final_alt_trials
                   << '\n';
            report << std::left << std::setw(38) << "Phase 0 local legality checks"
                   << ": " << diag.local_legality_checks_phase0 << '\n';
            report << std::left << std::setw(38) << "FinalAlt local legality checks"
                   << ": " << diag.local_legality_checks_final_alt << '\n';
            report << std::left << std::setw(38) << "Full area recomputations"
                   << ": " << diag.full_area_recomputations << '\n';
            report << std::left << std::setw(38) << "Incremental area updates"
                   << ": " << diag.incremental_area_updates << '\n';

            write_subsection("Library Cache");
            report << std::left << std::setw(38) << "Lookup hits"
                   << ": " << diag.library_lookup_cache_hits << '\n';
            report << std::left << std::setw(38) << "Lookup misses"
                   << ": " << diag.library_lookup_cache_misses << '\n';

            write_subsection("Repair/Reclaim No-Progress Stop");
            report << std::left << std::setw(38) << "Enabled"
                   << ": "
                   << (optimization->repair_reclaim.no_progress_stop_enabled
                           ? "yes"
                           : "no")
                   << '\n';
            report << std::left << std::setw(38) << "Triggered"
                   << ": "
                   << (optimization->repair_reclaim.no_progress_stop_triggered
                           ? "yes"
                           : "no")
                   << '\n';
            report << std::left << std::setw(38) << "Streak limit"
                   << ": "
                   << optimization->repair_reclaim.no_progress_streak_limit
                   << '\n';
            report << std::left << std::setw(38) << "Final streak"
                   << ": "
                   << optimization->repair_reclaim.final_no_progress_streak
                   << '\n';
            report << std::left << std::setw(38) << "Triggered stop count"
                   << ": " << diag.repair_reclaim_no_progress_stops << '\n';

            const SecondRoundRuntimeDiagnostics& sr =
                optimization->second_round;
            write_section("Diagnostics: Optional Caches and Candidate Filtering");
            report << "type_id_cache_enabled                     : "
                   << (sr.type_id_cache_enabled ? "yes" : "no") << '\n';
            report << "indexed_timing_enabled                    : "
                   << (sr.indexed_timing_enabled ? "yes" : "no") << '\n';
            report << "phase0_endpoint_delta_pressure_enabled    : "
                   << (sr.phase0_endpoint_delta_pressure_enabled ? "yes" : "no") << '\n';
            report << "final_alt_ranked_reclaim_enabled          : "
                   << (sr.final_alt_ranked_reclaim_enabled ? "yes" : "no") << '\n';
            report << "library_cache_lookups                     : "
                   << sr.library_cache_lookups << '\n';
            report << "library_cache_misses                      : "
                   << sr.library_cache_misses << '\n';
            report << "linear_library_scans_remaining            : "
                   << sr.linear_library_scans_remaining << '\n';
            report << "tree_index_rebuilds                       : "
                   << sr.tree_index_rebuilds << '\n';
            report << std::fixed << std::setprecision(6)
                   << "tree_index_rebuild_time_s                 : "
                   << sr.tree_index_rebuild_time_seconds << '\n';
            report << "indexed_arrival_time_s                    : "
                   << sr.indexed_arrival_time_seconds << '\n';
            report << "indexed_timing_analysis_time_s            : "
                   << sr.indexed_timing_analysis_time_seconds << '\n';
            report << "old_timing_analysis_time_s                : "
                   << sr.old_timing_analysis_time_seconds << '\n';
            report << "phase0_pressure_method                    : "
                   << sr.phase0_pressure_method << '\n';
            report << "phase0_pressure_time_s                    : "
                   << sr.phase0_pressure_time_seconds << '\n';
            report << "phase0_pressure_verify_max_abs_diff       : "
                   << sr.phase0_pressure_verify_max_abs_diff << '\n';
            report << "final_alt_candidates_built                : "
                   << sr.final_alt_candidates_built << '\n';
            report << "final_alt_candidates_kept_after_topk      : "
                   << sr.final_alt_candidates_kept_after_topk << '\n';
            report << "final_alt_candidates_tried                : "
                   << sr.final_alt_candidates_tried << '\n';
            report << "final_alt_candidates_accepted             : "
                   << sr.final_alt_candidates_accepted << '\n';
            report << "final_alt_accept_rate                     : "
                   << sr.final_alt_accept_rate << '\n';
            report << "final_alt_topk_limit                      : "
                   << sr.final_alt_topk_limit << '\n';
            report << "reclaim_candidates_before_topk            : "
                   << sr.reclaim_candidates_before_topk << '\n';
            report << "reclaim_candidates_after_topk             : "
                   << sr.reclaim_candidates_after_topk << '\n';

            const FastTimingDiagnostics& fast = optimization->fast_timing;
            write_section("Diagnostics: FastTimingEngine");
            report << "fast_engine_enabled                       : "
                   << (fast.enabled ? "yes" : "no") << '\n';
            report << "verify_enabled                            : "
                   << (fast.verify_enabled ? "yes" : "no") << '\n';
            report << "verify_interval                           : "
                   << fast.verify_interval << '\n';
            report << "resize_trials_enabled                     : "
                   << (fast.resize_trials_enabled ? "yes" : "no") << '\n';
            report << "delete_trials_enabled                     : "
                   << (fast.delete_trials_enabled ? "yes" : "no") << '\n';
            report << "insert_trials_enabled                     : "
                   << (fast.insert_trials_enabled ? "yes" : "no") << '\n';
            report << std::fixed << std::setprecision(6)
                   << "fast_engine_build_time                    : "
                   << fast.build_time_seconds << '\n';
            report << "fast_sync_time_seconds                    : "
                   << fast.sync_time_seconds << '\n';
            report << "fast_trial_time_seconds                   : "
                   << fast.trial_time_seconds << '\n';
            report << "fast_group_collection_time_seconds        : "
                   << fast.group_collection_time_seconds << '\n';
            report << "fast_group_update_time_seconds            : "
                   << fast.group_update_time_seconds << '\n';
            report << "fast_sync_count                           : "
                   << fast.sync_count << '\n';
            report << "fast_resize_trials                        : "
                   << fast.resize_trials << '\n';
            report << "fast_resize_batch_trials                  : "
                   << fast.resize_batch_trials << '\n';
            report << "fast_resize_batch_candidates              : "
                   << fast.resize_batch_candidates << '\n';
            report << "fast_insert_trials                        : "
                   << fast.insert_trials << '\n';
            report << "fast_delete_trials                        : "
                   << fast.delete_trials << '\n';
            report << "fast_trial_count                          : "
                   << fast.trial_count << '\n';
            report << "fast_commit_count                         : "
                   << fast.commit_count << '\n';
            report << "fast_rollback_count                       : "
                   << fast.rollback_count << '\n';
            report << "fast_full_delaymodel_verifies             : "
                   << fast.verify_count << '\n';
            report << "fast_fallback_count                       : "
                   << fast.fallback_count << '\n';
            report << "fast_arrival_snapshot_count               : "
                   << fast.arrival_snapshot_count << '\n';
            report << "fast_affected_group_count                 : "
                   << fast.affected_group_count << '\n';
            report << "fast_avg_arrival_snapshots_per_trial      : "
                   << (fast.trial_count > 0
                           ? static_cast<double>(fast.arrival_snapshot_count) /
                                 static_cast<double>(fast.trial_count)
                           : 0.0)
                   << '\n';
            report << "fast_avg_affected_groups_per_trial        : "
                   << (fast.trial_count > 0
                           ? static_cast<double>(fast.affected_group_count) /
                                 static_cast<double>(fast.trial_count)
                           : 0.0)
                   << '\n';
            report << "fast_max_arrival_snapshots_per_trial      : "
                   << fast.max_arrival_snapshots_per_trial << '\n';
            report << "fast_max_affected_groups_per_trial        : "
                   << fast.max_affected_groups_per_trial << '\n';
            report << "fast_backend_scope                        : "
                   << "Phase0 resize, Repair/Reclaim insert/resize/delete, "
                      "FinalAlt repair/reclaim, and Strategy8 recovery when enabled"
                   << '\n';

            if (optimization->phase0.enabled) {
                const Phase0Summary& phase0 = optimization->phase0;
                write_section("Pipeline Stage Details: Phase 0");
                write_subsection("Configuration");
                report << std::left << std::setw(34) << "Candidate budget"
                       << ": " << phase0.max_trial_nodes << '\n';
                report << std::left << std::setw(34) << "Node fraction"
                       << ": " << phase0.node_fraction << '\n';
                report << std::left << std::setw(34) << "Types per node"
                       << ": " << phase0.max_types_per_node << '\n';
                report << std::left << std::setw(34) << "Maximum passes"
                       << ": " << phase0.max_passes << '\n';
                report << std::left << std::setw(34) << "Time budget (s)"
                       << ": " << phase0.time_budget_seconds << '\n';
                report << std::left << std::setw(34) << "Legacy WNS weight"
                       << ": " << phase0.wns_weight << '\n';
                report << std::left << std::setw(34)
                       << "Area tie-break penalty"
                       << ": " << phase0.area_tiebreak_penalty << '\n';

                write_subsection("Candidate Search");
                report << std::left << std::setw(34) << "Node ranking"
                       << ": " << phase0.node_ranking_method << '\n';
                report << std::left << std::setw(34) << "Budget mode"
                       << ": "
                       << (phase0.unlimited_by_count
                               ? "unlimited by count"
                               : "limited")
                       << '\n';
                report << std::left << std::setw(34) << "Ranked candidates"
                       << ": " << phase0.ranked_candidates_available << '\n';
                report << std::left << std::setw(34) << "Candidates scanned"
                       << ": " << phase0.candidates_scanned << '\n';
                report << std::left << std::setw(34)
                       << "Consecutive reject limit"
                       << ": " << phase0.max_consecutive_rejects << '\n';
                report << std::left << std::setw(34)
                       << "Failed batch limit"
                       << ": " << phase0.max_consecutive_failed_batches
                       << '\n';
                report << std::left << std::setw(34) << "Stop reason"
                       << ": " << phase0.early_stop_reason << '\n';
                report << std::left << std::setw(34) << "Early-stop flag"
                       << ": "
                       << (phase0.early_stop_triggered ? "yes" : "no")
                       << '\n';

                write_subsection("Timing Backend");
                report << std::left << std::setw(34) << "Timing mode"
                       << ": "
                       << (phase0.fast_batch_timing_used
                               ? "FastTimingEngine batch"
                               : (phase0.fast_trial_count > 0
                                      ? "FastTimingEngine individual"
                                      : (phase0.incremental_timing_enabled
                                             ? "legacy incremental"
                                             : "full")))
                       << '\n';
                report << std::left << std::setw(34)
                       << "Incremental verification"
                       << ": "
                       << (phase0.incremental_verify_enabled ? "on" : "off")
                       << '\n';
                report << std::left << std::setw(34) << "Verification failures"
                       << ": " << phase0.incremental_verify_failures << '\n';
                report << std::left << std::setw(34)
                       << "Average trial time (s)"
                       << ": " << phase0.average_trial_time_seconds << '\n';

                write_subsection("Batching");
                report << std::left << std::setw(34) << "Manual batching"
                       << ": "
                       << (phase0.batch_manual_enabled ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(34) << "Automatic batching"
                       << ": "
                       << (phase0.batch_auto_enabled ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(34) << "Batching used"
                       << ": "
                       << (phase0.batch_mode_enabled ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(34) << "Selection reason"
                       << ": " << phase0.batch_auto_reason << '\n';
                report << std::left << std::setw(34) << "Batch size"
                       << ": " << phase0.batch_size << '\n';
                report << std::left << std::setw(34)
                       << "Fast batch timing enabled"
                       << ": "
                       << (phase0.fast_batch_timing_enabled ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(34)
                       << "Fast batch timing used"
                       << ": "
                       << (phase0.fast_batch_timing_used ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(34) << "Split rejected batch"
                       << ": "
                       << (phase0.batch_split_on_fail ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(34)
                       << "Maximum split depth"
                       << ": " << phase0.batch_max_split_depth << '\n';
                report << std::left << std::setw(34)
                       << "Auto SS violation threshold"
                       << ": " << phase0.batch_auto_min_ss_violations << '\n';
                report << std::left << std::setw(34)
                       << "Auto pressure threshold"
                       << ": "
                       << phase0.batch_auto_min_pressure_candidates << '\n';
                report << std::left << std::setw(34) << "Batch attempts"
                       << ": " << phase0.batch_attempts << '\n';
                report << std::left << std::setw(34) << "Accepted batches"
                       << ": " << phase0.batch_accepted << '\n';
                report << std::left << std::setw(34) << "Rejected batches"
                       << ": " << phase0.batch_rejected << '\n';
                report << std::left << std::setw(34) << "Batch splits"
                       << ": " << phase0.batch_split_count << '\n';
                report << std::left << std::setw(34)
                       << "Split depth limit hits"
                       << ": " << phase0.batch_split_depth_limit_hits
                       << '\n';
                report << std::left << std::setw(34)
                       << "Candidates in accepted batches"
                       << ": " << phase0.batch_accepted_candidates << '\n';
                report << std::left << std::setw(34)
                       << "Accepted individual candidates"
                       << ": " << phase0.fallback_individual_accepted << '\n';

                write_subsection("Outcome");
                if (cfg.enable_phase0_score_based_acceptance) {
                    report << std::left << std::setw(34) << "Objective"
                           << ": maximize weighted score ("
                           << cfg.alpha << ", " << cfg.beta << ", "
                           << cfg.gamma << ")\n";
                } else {
                    report << std::left << std::setw(34) << "Objective"
                           << ": Pareto guard SS/FF WNS, TNS, and area\n";
                }
                report << std::left << std::setw(34) << "Reset resizes"
                       << ": " << phase0.reset_resizes << '\n';
                report << std::left << std::setw(34) << "Attempted resizes"
                       << ": " << phase0.attempted_resizes << '\n';
                report << std::left << std::setw(34) << "Accepted resizes"
                       << ": " << phase0.accepted_resizes << '\n';
                report << std::left << std::setw(34) << "Rejected resizes"
                       << ": " << phase0.rejected_resizes << '\n';
                report << std::left << std::setw(34)
                       << "Pressure candidates (+/-/0)"
                       << ": " << phase0.positive_pressure_candidates << " / "
                       << phase0.negative_pressure_candidates << " / "
                       << phase0.zero_pressure_candidates << '\n';
                report << std::left << std::setw(34) << "Legacy timing cost"
                       << ": " << phase0.original_timing_cost << " -> "
                       << phase0.final_phase0_timing_cost << '\n';
                report << std::left << std::setw(34) << "Area"
                       << ": " << phase0.area_before_phase0 << " -> "
                       << phase0.area_after_phase0
                       << "  (ratio " << phase0.area_ratio_after_phase0
                       << ")\n";
                report << std::left << std::setw(34) << "Runtime"
                       << ": " << phase0.runtime_seconds << " s\n";

                if (!phase0.move_records.empty()) {
                    write_subsection("Top Accepted Phase 0 Moves");
                    report << std::left
                           << std::setw(24) << "Node"
                           << std::setw(16) << "Old Type"
                           << std::setw(16) << "New Type"
                           << std::right
                           << std::setw(12) << "Pressure"
                           << std::setw(14) << "Old Cost"
                           << std::setw(14) << "New Cost"
                           << std::setw(12) << "Old Area"
                           << std::setw(12) << "New Area"
                           << std::setw(12) << "dArea"
                           << '\n';
                    const size_t move_limit = std::min<size_t>(10, phase0.move_records.size());
                    for (size_t i = 0; i < move_limit; ++i) {
                        const Phase0MoveRecord& move = phase0.move_records[i];
                        report << std::left
                               << std::setw(24) << move.node_name
                               << std::setw(16) << move.old_type
                               << std::setw(16) << move.new_type
                               << std::right << std::fixed << std::setprecision(6)
                               << std::setw(12) << move.pressure
                               << std::setw(14) << move.old_timing_cost
                               << std::setw(14) << move.new_timing_cost
                               << std::setprecision(3)
                               << std::setw(12) << move.old_area
                               << std::setw(12) << move.new_area
                               << std::setw(12) << move.area_delta
                               << '\n';
                    }
                }
            }

            if (optimization) {
                if (optimization->existing_buffer_shared_reclaim_skipped_for_large_case) {
                    write_section(
                        "Pipeline Stage Details: Existing-Buffer Shared/Reclaim");
                    report << std::left << std::setw(36)
                           << "Status"
                           << ": skipped for large case\n";
                    report << std::left << std::setw(36)
                           << "Large-case path threshold"
                           << ": "
                           << cfg.perturb_recover_large_case_min_paths
                           << '\n';
                    report << std::left << std::setw(36)
                           << "Reason"
                           << ": avoid spending the shared-stage budget after repeated zero-yield large-case trials\n";
                } else if (optimization->existing_buffer_shared_reclaim_enabled) {
                    const RepairReclaimCycleRecord& cycle =
                        optimization->existing_buffer_shared_reclaim_cycle;
                    write_section(
                        "Pipeline Stage Details: Existing-Buffer Shared/Reclaim");
                    report << std::left << std::setw(36)
                           << "Placement restriction"
                           << ": original buffer parent -> original "
                           << (cfg.existing_shared_children_must_be_buffers
                                   ? "buffer children only\n"
                                   : "buffer or FF children\n");
                    report << std::left << std::setw(36)
                           << "Endpoint repair"
                           << ": disabled\n";
                    report << std::left << std::setw(36)
                           << "Alternated with Repair/Reclaim"
                           << ": "
                           << (cfg.enable_existing_buffer_shared_reclaim_alternation
                                   ? "yes"
                                   : "no")
                           << '\n';
                    report << std::left << std::setw(36)
                           << "Completed shared/reclaim cycles"
                           << ": "
                           << optimization->existing_buffer_shared_reclaim_cycles.size()
                           << '\n';
                    report << std::left << std::setw(36)
                           << "Stop alternation if initial empty"
                           << ": "
                           << (cfg.existing_shared_disable_alternation_if_initial_no_insertion
                                   ? "yes"
                                   : "no")
                           << '\n';
                    report << std::left << std::setw(36)
                           << "No-insertion cycle limit"
                           << ": "
                           << cfg.existing_shared_max_consecutive_no_insertion_cycles
                           << " (<=0: disabled)\n";
                    report << std::left << std::setw(36)
                           << "Alternation disabled by no insertion"
                           << ": "
                           << (optimization->existing_buffer_shared_reclaim_disabled_by_no_insertion
                                   ? "yes, after cycle " +
                                         std::to_string(optimization->existing_buffer_shared_reclaim_disabled_after_cycle)
                                   : "no")
                           << '\n';
                    report << std::left << std::setw(36)
                           << "Final no-insertion streak"
                           << ": "
                           << optimization->existing_buffer_shared_final_no_insertion_streak
                           << '\n';
                    report << std::left << std::setw(36)
                           << "Shared candidates tried"
                           << ": " << cycle.pulse_shared_candidates_tried << '\n';
                    report << std::left << std::setw(36)
                           << "Shared insertions"
                           << ": " << cycle.pulse_shared_insertions << '\n';
                    report << std::left << std::setw(36)
                           << "Reclaim deletes / resizes"
                           << ": " << cycle.newbuf_deletes_accepted << " / "
                           << (cycle.original_resizes_accepted +
                               cycle.newbuf_resizes_accepted) << '\n';
                    report << std::left << std::setw(36)
                           << "Shared+Reclaim elapsed (s)"
                           << ": "
                           << (cycle.end_time_seconds -
                               cycle.start_time_seconds) << '\n';
                    report << std::left << std::setw(36)
                           << "Time budget (s)"
                           << ": " << cfg.cycle_reclaim_time_budget_seconds
                           << '\n';
                    report << std::left << std::setw(36)
                           << "Pulse stop reason"
                           << ": "
                           << (cycle.pulse_stop_reason.empty()
                                   ? "accepted"
                                   : cycle.pulse_stop_reason)
                           << '\n';
                    if (optimization->existing_buffer_shared_reclaim_cycles.size() > 1) {
                        write_subsection("Existing-Buffer Shared/Reclaim Cycle History");
                        report << std::left << std::setw(7) << "Cycle"
                               << std::right << std::setw(10) << "Elapsed"
                               << std::setw(9) << "Cand"
                               << std::setw(8) << "+Buf"
                               << std::setw(9) << "Reclaim"
                               << std::setw(12) << "SS_TNS"
                               << std::setw(10) << "SS_V"
                               << std::setw(12) << "Area"
                               << "  Stop\n";
                        write_rule('-');
                        for (const RepairReclaimCycleRecord& record :
                             optimization->existing_buffer_shared_reclaim_cycles) {
                            report << std::left << std::setw(7) << record.cycle_index
                                   << std::right << std::fixed << std::setprecision(3)
                                   << std::setw(10)
                                   << (record.end_time_seconds - record.start_time_seconds)
                                   << std::setw(9) << record.pulse_shared_candidates_tried
                                   << std::setw(8) << record.pulse_shared_insertions
                                   << std::setw(9)
                                   << (record.newbuf_deletes_accepted +
                                       record.original_resizes_accepted +
                                       record.newbuf_resizes_accepted)
                                   << std::setprecision(4)
                                   << std::setw(12) << record.after_reclaim_timing.ss.tns
                                   << std::setw(10)
                                   << record.after_reclaim_timing.ss.violating_paths
                                   << std::setprecision(3)
                                   << std::setw(12) << record.after_reclaim_area
                                   << "  "
                                   << (record.pulse_stop_reason.empty()
                                           ? "accepted"
                                           : record.pulse_stop_reason)
                                   << '\n';
                        }
                    }
                    report << '\n';
                }
                const AdaptiveRepairReclaimParams& adaptive =
                    optimization->adaptive_repair_reclaim;
                write_section(
                    "Pipeline Stage Details: Adaptive Repair/Reclaim Parameters");
                if (adaptive.enabled) {
                    report << "enabled                     : yes\n";
                    report << "baseline_ss_violations      : "
                           << adaptive.baseline_ss_violations << '\n';
                    report << "phase0_pressure_candidates  : "
                           << adaptive.phase0_pressure_candidates << '\n';
                    report << "phase0_batch_used           : "
                           << (adaptive.phase0_batch_used ? "yes" : "no") << '\n';
                    report << "classified_case_size        : "
                           << adaptive.classified_case_size << '\n';
                    report << "selected_giveback_ratio     : "
                           << adaptive.selected_reclaim_giveback_ratio << '\n';
                    report << "selected_pulse_delay_scale  : "
                           << adaptive.selected_phase1a_pulse_delay_scale << '\n';
                    report << "manual_giveback_ratio       : "
                           << adaptive.manual_reclaim_giveback_ratio << '\n';
                    report << "manual_pulse_delay_scale    : "
                           << adaptive.manual_phase1a_pulse_delay_scale << '\n';
                    if (adaptive.missing_statistics) {
                        report << "WARNING: adaptive classification missing some statistics; used fallback classification.\n";
                    }
                } else {
                    report << "Adaptive Repair/Reclaim Parameters: disabled\n";
                    report << "using manual reclaim_giveback_ratio = "
                           << adaptive.manual_reclaim_giveback_ratio << '\n';
                    report << "using manual phase1a_pulse_delay_scale = "
                           << adaptive.manual_phase1a_pulse_delay_scale << '\n';
                }
            }

            if (optimization->repair_reclaim.enabled) {
                const RepairReclaimSummary& rr = optimization->repair_reclaim;
                const bool rr_has_score_cycles =
                    std::any_of(
                        rr.cycles.begin(),
                        rr.cycles.end(),
                        [](const RepairReclaimCycleRecord& cycle) {
                            return cycle.branch_mode == "score" ||
                                   cycle.branch_mode == "score_delete";
                        });
                const bool rr_has_heuristic_cycles =
                    std::any_of(
                        rr.cycles.begin(),
                        rr.cycles.end(),
                        [](const RepairReclaimCycleRecord& cycle) {
                            return cycle.branch_mode == "heuristic";
                        });
                write_section(
                    "Pipeline Stage Details: Repair/Reclaim Cycles");
                write_subsection("Configuration");
                report << std::left << std::setw(36) << "Reclaim enabled"
                       << ": " << (rr.reclaim_enabled ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Absolute stage end time (s)"
                       << ": " << rr.cycle_end_time_seconds << '\n';
                report << std::left << std::setw(36) << "Maximum cycles"
                       << ": " << rr.max_cycles << '\n';
                report << std::left << std::setw(36)
                       << "Repair pulse delay scale"
                       << ": " << rr.phase1a_pulse_delay_scale << '\n';
                report << std::left << std::setw(36)
                       << "Split rejected repair pulse"
                       << ": "
                       << (rr.pulse_split_on_fail ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Maximum repair split depth"
                       << ": " << rr.pulse_max_split_depth << '\n';
                report << std::left << std::setw(36)
                       << "Pulse failure recovery"
                       << ": "
                       << (rr.pulse_failure_recovery_enabled
                               ? "on"
                               : "off")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Maximum consecutive failures"
                       << ": " << rr.pulse_max_consecutive_failures
                       << '\n';
                report << std::left << std::setw(36)
                       << "Recovery scale multiplier"
                       << ": " << rr.pulse_recovery_scale_multiplier
                       << '\n';
                report << std::left << std::setw(36)
                       << "Recovery minimum delay scale"
                       << ": " << rr.pulse_recovery_min_delay_scale
                       << '\n';
                report << std::left << std::setw(36)
                       << "Blacklisted targets per failure"
                       << ": " << rr.pulse_recovery_blacklist_targets
                       << '\n';
                report << std::left << std::setw(36)
                       << "Non-score TNS acceptance"
                       << ": "
                       << (cfg.repair_pulse_require_both_tns_not_worse
                               ? "SS AND FF not worse"
                               : "Guarded OR (FF closed => SS strict)")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Closed-corner TNS epsilon"
                       << ": " << std::scientific << std::setprecision(3)
                       << cfg.repair_pulse_closed_tns_epsilon
                       << '\n';
                report << std::left << std::setw(36)
                       << "Required SS TNS improvement"
                       << ": "
                       << cfg.repair_pulse_required_tns_improvement
                       << std::fixed << std::setprecision(3) << '\n';
                report << std::left << std::setw(36)
                       << "Shared-path repair"
                       << ": "
                       << (rr.shared_path_repair_enabled ? "on" : "off")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Shared no-insertion pulse limit"
                       << ": "
                       << rr.shared_max_consecutive_no_insertion_pulses
                       << " (<=0: disabled)\n";
                report << std::left << std::setw(36)
                       << "Per-cycle reclaim budget (s)"
                       << ": " << rr.cycle_reclaim_time_budget_seconds << '\n';
                report << std::left << std::setw(36)
                       << "Legacy giveback ratio"
                       << ": " << rr.reclaim_giveback_ratio << '\n';
                report << std::left << std::setw(36)
                       << "Score-based acceptance"
                       << ": "
                       << (rr_has_score_cycles && rr_has_heuristic_cycles
                               ? "alternating with heuristic cycles"
                           : rr_has_score_cycles
                               ? "on"
                               : "off")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Alternating recent-delete limit"
                       << ": "
                       << (rr_has_score_cycles && rr_has_heuristic_cycles
                               ? std::to_string(
                                     cfg.repair_reclaim_alternating_delete_recent_limit)
                               : "N/A")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Reclaim ranking policy"
                       << ": "
                       << "area/pressure heuristic"
                       << '\n';
                report << std::left << std::setw(36)
                       << "Reclaim score acceptance"
                       << ": "
                       << (rr_has_score_cycles
                               ? "not worse"
                               : "legacy timing giveback guards")
                       << '\n';
                report << std::left << std::setw(36)
                       << "No-progress stop"
                       << ": " << (rr.no_progress_stop_enabled ? "on" : "off")
                       << '\n';
                report << std::left << std::setw(36)
                       << "No-progress streak limit"
                       << ": " << rr.no_progress_streak_limit << '\n';

                write_subsection("Outcome");
                report << std::left << std::setw(36) << "Completed cycles"
                       << ": " << rr.total_cycles << '\n';
                report << std::left << std::setw(36) << "Stop reason"
                       << ": " << rr.stop_reason << '\n';
                report << std::left << std::setw(36)
                       << "Final no-progress streak"
                       << ": " << rr.final_no_progress_streak << '\n';
                report << std::left << std::setw(36)
                       << "No-progress stop triggered"
                       << ": "
                       << (rr.no_progress_stop_triggered ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Repair pulse insertions"
                       << ": " << rr.total_pulse_inserted_buffers << '\n';
                report << std::left << std::setw(36)
                       << "Shared-path candidates tried"
                       << ": " << rr.total_shared_candidates_tried << '\n';
                report << std::left << std::setw(36)
                       << "Shared-path insertions"
                       << ": " << rr.total_shared_insertions << '\n';
                report << std::left << std::setw(36)
                       << "Shared no-insertion streak"
                       << ": " << rr.final_shared_no_insertion_streak
                       << '\n';
                report << std::left << std::setw(36)
                       << "Shared-path disabled by streak"
                       << ": "
                       << (rr.shared_path_repair_disabled_by_streak
                               ? "yes, after cycle " +
                                     std::to_string(
                                         rr.shared_path_repair_disabled_after_cycle)
                               : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Repair batch attempts"
                       << ": " << rr.total_pulse_batch_attempts << '\n';
                report << std::left << std::setw(36)
                       << "Rejected repair batches"
                       << ": " << rr.total_pulse_rejected_batches << '\n';
                report << std::left << std::setw(36)
                       << "Repair batch splits"
                       << ": " << rr.total_pulse_batch_splits << '\n';
                report << std::left << std::setw(36)
                       << "Repair split depth-limit hits"
                       << ": "
                       << rr.total_pulse_split_depth_limit_hits << '\n';
                report << std::left << std::setw(36)
                       << "Maximum split depth reached"
                       << ": " << rr.max_pulse_split_depth_reached
                       << '\n';
                report << std::left << std::setw(36)
                       << "Pulse recovery retries"
                       << ": " << rr.total_pulse_recovery_retries
                       << '\n';
                report << std::left << std::setw(36)
                       << "Successful pulse recoveries"
                       << ": " << rr.successful_pulse_recoveries
                       << '\n';
                report << std::left << std::setw(36)
                       << "Maximum pulse failure streak"
                       << ": " << rr.max_consecutive_pulse_failures
                       << '\n';
                report << std::left << std::setw(36)
                       << "Original-buffer resizes"
                       << ": " << rr.total_original_resizes << '\n';
                report << std::left << std::setw(36) << "NEW_BUF resizes"
                       << ": " << rr.total_newbuf_resizes << '\n';
                report << std::left << std::setw(36) << "NEW_BUF deletes"
                       << ": " << rr.total_newbuf_deletes << '\n';
                report << std::left << std::setw(36)
                       << "Pre-cycle perturb deletes"
                       << ": " << rr.total_pre_cycle_deletes << '\n';
                report << std::left << std::setw(36)
                       << "Pre-cycle perturb area removed"
                       << ": " << rr.total_pre_cycle_deleted_area << '\n';

                write_subsection("Area Balance");
                report << std::left << std::setw(36)
                       << "Pulse area increase"
                       << ": " << rr.total_pulse_area_increase << '\n';
                report << std::left << std::setw(36)
                       << "Total reclaim area saved"
                       << ": " << rr.total_reclaim_area_saved << '\n';
                report << std::left << std::setw(36)
                       << "Net cycle area delta"
                       << ": " << rr.total_net_cycle_area_delta << '\n';
                report << std::left << std::setw(36)
                       << "Average reclaim coverage"
                       << ": " << rr.average_reclaim_coverage << '\n';
                report << std::left << std::setw(36)
                       << "Weighted reclaim coverage"
                       << ": " << rr.weighted_reclaim_coverage << '\n';

                write_subsection("Reclaim Search");
                report << std::left << std::setw(36) << "Runtime (s)"
                       << ": " << rr.total_reclaim_runtime_seconds << '\n';
                report << std::left << std::setw(36) << "Candidates built"
                       << ": " << rr.total_reclaim_candidates_built << '\n';
                report << std::left << std::setw(36) << "Candidates tried"
                       << ": " << rr.total_reclaim_candidates_tried << '\n';
                report << std::left << std::setw(36)
                       << "Candidates accepted"
                       << ": " << rr.total_reclaim_candidates_accepted << '\n';
                report << std::left << std::setw(36) << "Accept rate"
                       << ": " << rr.total_reclaim_accept_rate << '\n';
                report << std::left << std::setw(36)
                       << "Area saved: original resize"
                       << ": " << rr.total_original_resize_area_saved << '\n';
                report << std::left << std::setw(36)
                       << "Area saved: NEW_BUF resize"
                       << ": " << rr.total_newbuf_resize_area_saved << '\n';
                report << std::left << std::setw(36)
                       << "Area saved: NEW_BUF delete"
                       << ": " << rr.total_newbuf_delete_area_saved << '\n';

                if (!rr.cycles.empty()) {
                    write_subsection(
                        "Repair/Reclaim Branch and Pre-Cycle Perturbation");
                    report << std::left
                           << std::setw(7) << "Cycle"
                           << std::setw(15) << "Mode"
                           << std::right
                           << std::setw(9) << "DelTry"
                           << std::setw(8) << "Del"
                           << std::setw(12) << "DelArea"
                           << std::setw(14) << "ScoreBefore"
                           << std::setw(14) << "ScoreAfter"
                           << std::setw(13) << "ScoreDelta"
                           << std::setw(11) << "SS_TNS"
                           << std::setw(10) << "SS_WNS"
                           << std::setw(8) << "SS_V"
                           << std::setw(11) << "FF_TNS"
                           << std::setw(10) << "FF_WNS"
                           << std::setw(8) << "FF_V"
                           << std::setw(12) << "AreaEnd"
                           << '\n';
                    write_rule('-');
                    for (const RepairReclaimCycleRecord& cycle :
                         rr.cycles) {
                        report << std::left
                               << std::setw(7) << cycle.cycle_index
                               << std::setw(15) << cycle.branch_mode
                               << std::right << std::fixed
                               << std::setprecision(3)
                               << std::setw(9)
                               << cycle.pre_cycle_delete_attempts
                               << std::setw(8)
                               << cycle.pre_cycle_deletes
                               << std::setw(12)
                               << cycle.pre_cycle_deleted_area
                               << std::setprecision(6)
                               << std::setw(14)
                               << cycle.pre_cycle_score_before
                               << std::setw(14)
                               << cycle.pre_cycle_score_after
                               << std::setw(13)
                               << (cycle.pre_cycle_score_after -
                                   cycle.pre_cycle_score_before)
                               << std::setprecision(4)
                               << std::setw(11)
                               << cycle.after_reclaim_timing.ss.tns
                               << std::setw(10)
                               << cycle.after_reclaim_timing.ss.wns
                               << std::setw(8)
                               << cycle.after_reclaim_timing.ss.violating_paths
                               << std::setw(11)
                               << cycle.after_reclaim_timing.ff.tns
                               << std::setw(10)
                               << cycle.after_reclaim_timing.ff.wns
                               << std::setw(8)
                               << cycle.after_reclaim_timing.ff.violating_paths
                               << std::setprecision(3)
                               << std::setw(12)
                               << cycle.after_reclaim_area
                               << '\n';
                    }

                    if (rr.pulse_failure_recovery_enabled) {
                        write_subsection(
                            "Repair Pulse Failure Recovery");
                        report << std::left
                               << std::setw(7) << "Cycle"
                               << std::right
                               << std::setw(10) << "Scale"
                               << std::setw(10) << "Recovery"
                               << std::setw(9) << "Failures"
                               << std::setw(11) << "Blacklist"
                               << "  Stop reason"
                               << '\n';
                        write_rule('-');
                        for (const RepairReclaimCycleRecord& cycle :
                             rr.cycles) {
                            report << std::left
                                   << std::setw(7) << cycle.cycle_index
                                   << std::right << std::fixed
                                   << std::setprecision(3)
                                   << std::setw(10)
                                   << cycle.phase1a_pulse_delay_scale
                                   << std::setw(10)
                                   << (cycle.pulse_recovery_attempt
                                           ? "yes"
                                           : "no")
                                   << std::setw(9)
                                   << cycle.pulse_failure_streak
                                   << std::setw(11)
                                   << cycle.pulse_recovery_blacklist_size
                                   << "  "
                                   << (cycle.pulse_stop_reason.empty()
                                           ? "-"
                                           : cycle.pulse_stop_reason)
                                   << '\n';
                        }
                    }

                    write_subsection("Repair/Reclaim Cycle Area Balance");
                    report << std::left
                           << std::setw(6) << "Cycle"
                           << std::right
                           << std::setw(10) << "Start"
                           << std::setw(10) << "End"
                           << std::setw(8) << "+Buf"
                           << std::setw(12) << "AreaBefore"
                           << std::setw(12) << "AreaPulse"
                           << std::setw(13) << "AreaReclaim"
                           << std::setw(11) << "PulseArea+"
                           << std::setw(12) << "ReclaimSave"
                           << std::setw(11) << "ReclaimCov"
                           << std::setw(11) << "NetArea"
                           << '\n';
                    write_rule('-');
                    for (const RepairReclaimCycleRecord& cycle : rr.cycles) {
                        report << std::left << std::setw(6) << cycle.cycle_index
                               << std::right << std::fixed << std::setprecision(3)
                               << std::setw(10) << cycle.start_time_seconds
                               << std::setw(10) << cycle.end_time_seconds
                               << std::setw(8) << cycle.pulse_inserted_buffers
                               << std::setw(12) << cycle.before_pulse_area
                               << std::setw(12) << cycle.after_pulse_area
                               << std::setw(13) << cycle.after_reclaim_area
                               << std::setw(11) << cycle.pulse_area_increase
                               << std::setw(12) << cycle.reclaim_area_saved
                               << std::setw(11) << cycle.reclaim_coverage
                               << std::setw(11) << cycle.net_cycle_area_delta
                               << '\n';
                    }

                    write_subsection(
                        "Repair/Reclaim Cycle Timing Giveback");
                    report << std::left
                           << std::setw(6) << "Cycle"
                           << std::right
                           << std::setw(11) << "SSGain"
                           << std::setw(12) << "SSAllow"
                           << std::setw(11) << "SSUsed"
                           << std::setw(11) << "SSUtil"
                           << std::setw(11) << "SSWnsBad"
                           << std::setw(11) << "FFGain"
                           << std::setw(12) << "FFAllow"
                           << std::setw(11) << "FFUsed"
                           << std::setw(11) << "FFUtil"
                           << std::setw(11) << "FFWnsBad"
                           << std::setw(11) << "SS_TNS"
                           << std::setw(10) << "SS_WNS"
                           << std::setw(8) << "SS_V"
                           << std::setw(11) << "FF_TNS"
                           << std::setw(10) << "FF_WNS"
                           << std::setw(8) << "FF_V"
                           << '\n';
                    write_rule('-');
                    for (const RepairReclaimCycleRecord& cycle : rr.cycles) {
                        report << std::left << std::setw(6) << cycle.cycle_index
                               << std::right << std::fixed << std::setprecision(4)
                               << std::setw(11) << cycle.pulse_ss_tns_gain
                               << std::setw(12) << cycle.allowed_ss_tns_giveback
                               << std::setw(11) << cycle.ss_tns_giveback_used
                               << std::setw(11) << cycle.ss_giveback_utilization
                               << std::setw(11) << cycle.ss_wns_worsen
                               << std::setw(11) << cycle.pulse_ff_tns_gain
                               << std::setw(12) << cycle.allowed_ff_tns_giveback
                               << std::setw(11) << cycle.ff_tns_giveback_used
                               << std::setw(11) << cycle.ff_giveback_utilization
                               << std::setw(11) << cycle.ff_wns_worsen
                               << std::setw(11) << cycle.after_reclaim_timing.ss.tns
                               << std::setw(10) << cycle.after_reclaim_timing.ss.wns
                               << std::setw(8) << cycle.after_reclaim_timing.ss.violating_paths
                               << std::setw(11) << cycle.after_reclaim_timing.ff.tns
                               << std::setw(10) << cycle.after_reclaim_timing.ff.wns
                               << std::setw(8) << cycle.after_reclaim_timing.ff.violating_paths
                               << '\n';
                    }

                    write_subsection(
                        "Repair/Reclaim Cycle Reclaim Efficiency");
                    report << std::left
                           << std::setw(6) << "Cycle"
                           << std::right
                           << std::setw(10) << "Rt(s)"
                           << std::setw(8) << "Cand"
                           << std::setw(8) << "Tried"
                           << std::setw(8) << "Acc"
                           << std::setw(8) << "Rej"
                           << std::setw(10) << "AccRate"
                           << std::setw(11) << "Trial/s"
                           << std::setw(11) << "Area/s"
                           << std::setw(11) << "Area/Acc"
                           << std::setw(8) << "OrigR"
                           << std::setw(8) << "NewR"
                           << std::setw(8) << "Del"
                           << std::setw(11) << "OrigSave"
                           << std::setw(11) << "NewRSave"
                           << std::setw(11) << "DelSave"
                           << '\n';
                    write_rule('-');
                    for (const RepairReclaimCycleRecord& cycle : rr.cycles) {
                        report << std::left << std::setw(6) << cycle.cycle_index
                               << std::right << std::fixed << std::setprecision(3)
                               << std::setw(10) << cycle.reclaim_runtime_seconds
                               << std::setw(8) << cycle.reclaim_candidates_built
                               << std::setw(8) << cycle.reclaim_candidates_tried
                               << std::setw(8) << cycle.reclaim_candidates_accepted
                               << std::setw(8) << cycle.reclaim_candidates_rejected
                               << std::setw(10) << cycle.reclaim_accept_rate
                               << std::setw(11) << cycle.reclaim_trials_per_second
                               << std::setw(11) << cycle.reclaim_area_saved_per_second
                               << std::setw(11) << cycle.reclaim_area_saved_per_accepted_move
                               << std::setw(8) << cycle.original_resizes_accepted
                               << std::setw(8) << cycle.newbuf_resizes_accepted
                               << std::setw(8) << cycle.newbuf_deletes_accepted
                               << std::setw(11) << cycle.original_resize_area_saved
                               << std::setw(11) << cycle.newbuf_resize_area_saved
                               << std::setw(11) << cycle.newbuf_delete_area_saved
                               << '\n';
                    }

                    bool wrote_diagnostic_header = false;
                    auto write_diagnostic_header = [&]() {
                        if (wrote_diagnostic_header) return;
                        write_subsection(
                            "Repair/Reclaim Cycle Diagnostics");
                        wrote_diagnostic_header = true;
                    };
                    for (const RepairReclaimCycleRecord& cycle : rr.cycles) {
                        if (cycle.reclaim_coverage < 0.5 &&
                            cycle.pulse_area_increase > 0.0) {
                            write_diagnostic_header();
                            report << "Cycle " << cycle.cycle_index
                                   << ": WARNING: reclaim coverage below 50%; area may be accumulating.\n";
                        }
                        if (cycle.net_cycle_area_delta > 0.0) {
                            write_diagnostic_header();
                            report << "Cycle " << cycle.cycle_index
                                   << ": NOTE: cycle increased net area.\n";
                        }
                        if (cycle.reclaim_accept_rate < 0.05 &&
                            cycle.reclaim_candidates_tried > 0) {
                            write_diagnostic_header();
                            report << "Cycle " << cycle.cycle_index
                                   << ": NOTE: low reclaim accept rate; candidate ranking or timing budget may be too restrictive.\n";
                        }
                    }
                }
            }

            {
                const FinalAlternatingGreedySummary& final_alt =
                    optimization->final_alt;
                write_section(
                    "Pipeline Stage Details: Final Alternating Greedy");
                write_subsection("Configuration");
                report << std::left << std::setw(36) << "Enabled"
                       << ": " << (final_alt.enabled ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Replaces legacy Phase 1B/2"
                       << ": "
                       << (final_alt.replace_phase1b_and_phase2 ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(36) << "Maximum iterations"
                       << ": " << final_alt.max_iters << '\n';
                report << std::left << std::setw(36)
                       << "Repair attempts per iteration"
                       << ": " << final_alt.repair_insertions_per_iter << '\n';
                report << std::left << std::setw(36)
                       << "Blacklist cooldown iterations"
                       << ": " << cfg.final_alt_blacklist_cooldown_iters
                       << '\n';
                report << std::left << std::setw(36)
                       << "Maximum failures per target"
                       << ": " << cfg.final_alt_max_failures_per_target
                       << '\n';
                report << std::left << std::setw(36)
                       << "Repair types per target"
                       << ": " << cfg.final_alt_repair_types_per_target
                       << '\n';
                report << std::left << std::setw(36)
                       << "Include closest delay overshoot"
                       << ": "
                       << (cfg.final_alt_include_delay_overshoot_types
                               ? "yes"
                               : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Reclaim types per node"
                       << ": " << cfg.final_alt_reclaim_types_per_node
                       << '\n';
                report << std::left << std::setw(36)
                       << "Effective reclaim top-K"
                       << ": "
                       << optimization->second_round.final_alt_topk_limit
                       << '\n';
                report << std::left << std::setw(36)
                       << "Effective reclaim top-K per kind"
                       << ": "
                       << optimization->second_round.final_alt_topk_per_kind_limit
                       << " (<=0: disabled)\n";
                report << std::left << std::setw(36)
                       << "Local time budget (s)"
                       << ": " << cfg.final_alt_time_budget_seconds
                       << '\n';
                report << std::left << std::setw(36)
                       << "Global safety margin (s)"
                       << ": " << cfg.final_alt_safety_margin_seconds
                       << '\n';
                report << std::left << std::setw(36)
                       << "Legacy reclaim giveback ratio"
                       << ": " << final_alt.reclaim_giveback_ratio << '\n';
                report << std::left << std::setw(36)
                       << "Score-based acceptance"
                       << ": "
                       << (cfg.enable_final_alt_score_based_acceptance
                               ? "on"
                               : "off")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Target checkpoint"
                       << ": ";
                if (final_alt.target_checkpoint_enabled) {
                    report << "on (SS/FF/Area = "
                           << cfg.perturb_recover_checkpoint_alpha << "/"
                           << cfg.perturb_recover_checkpoint_beta << "/"
                           << cfg.perturb_recover_checkpoint_gamma << ")";
                } else {
                    report << "off";
                }
                report << '\n';
                report << std::left << std::setw(36) << "Hard WNS guard"
                       << ": " << (final_alt.hard_wns_guard ? "yes" : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Area-decrease-only reclaim"
                       << ": "
                       << (final_alt.area_decrease_only ? "yes" : "no")
                       << '\n';

                write_subsection("Outcome");
                report << std::left << std::setw(36) << "Runtime (s)"
                       << ": " << final_alt.runtime_seconds << '\n';
                report << std::left << std::setw(36) << "Iterations"
                       << ": " << final_alt.iterations << '\n';
                report << std::left << std::setw(36) << "Stop reason"
                       << ": " << final_alt.stop_reason << '\n';
                report << std::left << std::setw(36)
                       << "Target checkpoint score"
                       << ": " << final_alt.target_score_before << " -> "
                       << final_alt.target_score_after << '\n';
                report << std::left << std::setw(36)
                       << "Target checkpoint best iteration"
                       << ": "
                       << final_alt.target_checkpoint_best_iteration
                       << '\n';
                report << std::left << std::setw(36)
                       << "Restored target checkpoint"
                       << ": "
                       << (final_alt.target_checkpoint_restored
                               ? "yes"
                               : "no")
                       << '\n';
                report << std::left << std::setw(36) << "Repair insertions"
                       << ": " << final_alt.repair_insertions << '\n';
                report << std::left << std::setw(36)
                       << "Accepted reclaim moves"
                       << ": " << final_alt.reclaim_moves_accepted << '\n';
                report << std::left << std::setw(36) << "NEW_BUF deletes"
                       << ": " << final_alt.newbuf_deletes << '\n';
                report << std::left << std::setw(36) << "Resizes"
                       << ": " << final_alt.resizes << '\n';
                report << std::left << std::setw(36) << "Area"
                       << ": " << final_alt.area_before << " -> "
                       << final_alt.area_after << "  (saved "
                       << final_alt.area_saved << ")\n";

                write_subsection("Timing Change");
                report << std::left << std::setw(8) << "Corner"
                       << std::right << std::setw(14) << "TNS before"
                       << std::setw(14) << "TNS after"
                       << std::setw(14) << "WNS before"
                       << std::setw(14) << "WNS after"
                       << std::setw(14) << "Viol before"
                       << std::setw(14) << "Viol after"
                       << '\n';
                write_rule('-');
                report << std::left << std::setw(8) << "SS"
                       << std::right << std::fixed << std::setprecision(4)
                       << std::setw(14) << final_alt.ss_tns_before
                       << std::setw(14) << final_alt.ss_tns_after
                       << std::setw(14) << final_alt.ss_wns_before
                       << std::setw(14) << final_alt.ss_wns_after
                       << std::setw(14) << final_alt.ss_violations_before
                       << std::setw(14) << final_alt.ss_violations_after
                       << '\n';
                report << std::left << std::setw(8) << "FF"
                       << std::right
                       << std::setw(14) << final_alt.ff_tns_before
                       << std::setw(14) << final_alt.ff_tns_after
                       << std::setw(14) << "-"
                       << std::setw(14) << "-"
                       << std::setw(14) << final_alt.ff_violations_before
                       << std::setw(14) << final_alt.ff_violations_after
                       << '\n';

                if (!optimization->final_alt_iterations.empty()) {
                    write_subsection(
                        "Final Alternating Greedy Iteration Detail");
                    report << std::left
                           << std::setw(5) << "Iter"
                           << std::right
                           << std::setw(8) << "Start"
                           << std::setw(8) << "End"
                           << std::setw(7) << "Need"
                           << std::setw(7) << "+Buf"
                           << std::setw(7) << "RepTry"
                           << std::setw(7) << "RepRej"
                           << std::setw(8) << "RecTry"
                           << std::setw(7) << "RecAcc"
                           << std::setw(7) << "RecRej"
                           << std::setw(7) << "Resize"
                           << std::setw(7) << "Del"
                           << std::setw(11) << "SSGain"
                           << std::setw(10) << "SSWNS"
                           << std::setw(7) << "dSSV"
                           << std::setw(11) << "FFGain"
                           << std::setw(10) << "FFWNS"
                           << std::setw(7) << "dFFV"
                           << std::setw(11) << "dArea"
                           << "  Stop"
                           << '\n';
                    write_rule('-');
                    for (const FinalAltIterationRecord& iter :
                         optimization->final_alt_iterations) {
                        const double ss_gain =
                            iter.after_timing.ss.tns -
                            iter.before_timing.ss.tns;
                        const double ff_gain =
                            iter.after_timing.ff.tns -
                            iter.before_timing.ff.tns;
                        const long long dss_v =
                            static_cast<long long>(
                                iter.after_timing.ss.violating_paths) -
                            static_cast<long long>(
                                iter.before_timing.ss.violating_paths);
                        const long long dff_v =
                            static_cast<long long>(
                                iter.after_timing.ff.violating_paths) -
                            static_cast<long long>(
                                iter.before_timing.ff.violating_paths);
                        const double d_area =
                            iter.area_after - iter.area_before;
                        report << std::left << std::setw(5)
                               << iter.iteration
                               << std::right << std::fixed
                               << std::setprecision(3)
                               << std::setw(8) << iter.start_time_seconds
                               << std::setw(8) << iter.end_time_seconds
                               << std::setw(7)
                               << (iter.repair_needed ? "yes" : "no")
                               << std::setw(7) << iter.repair_insertions
                               << std::setw(7) << iter.repair_attempts
                               << std::setw(7) << iter.repair_rejections
                               << std::setw(8) << iter.reclaim_candidates_tried
                               << std::setw(7) << iter.reclaim_moves_accepted
                               << std::setw(7) << iter.reclaim_moves_rejected
                               << std::setw(7) << iter.reclaim_resizes
                               << std::setw(7) << iter.reclaim_deletes
                               << std::setprecision(4)
                               << std::setw(11) << ss_gain
                               << std::setw(10) << iter.after_timing.ss.wns
                               << std::setw(7) << dss_v
                               << std::setw(11) << ff_gain
                               << std::setw(10) << iter.after_timing.ff.wns
                               << std::setw(7) << dff_v
                               << std::setprecision(3)
                               << std::setw(11) << d_area
                               << "  " << iter.stop_reason
                               << '\n';
                    }
                }
            }

            if (optimization->perturb_recover.enabled) {
                const PerturbRecoverSummary& perturb =
                    optimization->perturb_recover;
                write_section(
                    "Pipeline Stage Details: Strategy8 Perturb-and-Recover");
                write_subsection("Configuration");
                report << std::left << std::setw(36) << "Enabled"
                       << ": yes\n";
                report << std::left << std::setw(36) << "Random seed"
                       << ": " << perturb.random_seed << '\n';
                report << std::left << std::setw(36) << "Time budget (s)"
                       << ": " << perturb.time_budget_seconds << '\n';
                report << std::left << std::setw(36)
                       << "Final validation reserve (s)"
                       << ": " << perturb.validation_reserve_seconds
                       << '\n';
                report << std::left << std::setw(36) << "Maximum cycles"
                       << ": " << perturb.max_cycles << '\n';
                report << std::left << std::setw(36)
                       << "Candidates per cycle"
                       << ": " << perturb.candidates_per_cycle << '\n';
                report << std::left << std::setw(36)
                       << "Perturbation policy"
                       << ": "
                       << (perturb.brutal_area_path_enabled
                               ? "maximum-area path deletable-buffer stripping"
                               : "profiled beam mutations")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Candidate profiles"
                       << ": "
                       << (perturb.brutal_area_path_enabled
                               ? "replaced by brutal area path"
                           : perturb.candidate_profiles_enabled
                               ? "timing / area / balanced / random"
                               : "off (legacy guided beam)")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Guided target ratio"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "N/A (whole path selected)";
                } else {
                    report << perturb.guided_target_ratio;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Perturb moves (min/max)"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "N/A (all nodes on selected path)";
                } else {
                    report << perturb.min_moves << " / "
                           << perturb.max_moves;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Intensity step rejection streak"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "N/A";
                } else {
                    report << perturb.intensity_step_streak;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Move attempt multiplier"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "N/A";
                } else {
                    report << perturb.move_attempt_multiplier;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Quick recovery iterations"
                       << ": "
                       << (perturb.brutal_area_path_enabled
                               ? 0
                               : perturb.quick_recovery_iters)
                       << '\n';
                report << std::left << std::setw(36)
                       << "Quick repair attempts"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "N/A";
                } else {
                    report << perturb.quick_repair_attempts;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Quick reclaim candidates"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "N/A";
                } else {
                    report << perturb.quick_reclaim_candidates;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Selected-candidate deep recovery"
                       << ": "
                       << (perturb.brutal_area_path_enabled
                               ? "direct full reclaim-first Repair/Reclaim"
                           : perturb.cycle_deep_recovery_enabled
                               ? "reclaim-first Repair/Reclaim cycles"
                               : "legacy FinalAlt recovery")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Recovery Repair/Reclaim acceptance"
                       << ": "
                       << (perturb.recovery_score_based
                               ? "score-based"
                               : "heuristic")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Bootstrap reclaim candidates"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "unlimited";
                } else {
                    report << perturb.bootstrap_reclaim_candidates;
                }
                report
                       << '\n';
                report << std::left << std::setw(36)
                       << "Protect perturb-inserted buffers"
                       << ": "
                       << (perturb.brutal_area_path_enabled
                               ? "N/A (brutal perturb does not insert)"
                           : perturb.bootstrap_protect_inserted
                               ? "yes"
                               : "no")
                       << '\n';
                report << std::left << std::setw(36)
                       << "Deep recovery iterations"
                       << ": "
                       << (perturb.brutal_area_path_enabled
                               ? perturb.brutal_recovery_max_cycles
                               : perturb.recovery_iters)
                       << '\n';
                report << std::left << std::setw(36)
                       << (perturb.cycle_deep_recovery_enabled
                               ? "Repair Pulse max targets"
                               : "Deep repair attempts")
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "unlimited";
                } else {
                    report << perturb.repair_attempts;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Deep reclaim candidates"
                       << ": ";
                if (perturb.brutal_area_path_enabled) {
                    report << "unlimited";
                } else {
                    report << perturb.reclaim_candidates;
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Acceptance"
                       << ": legality + strict "
                       << (perturb.target_outer_acceptance_enabled
                               ? "target-score"
                               : "pipeline-score")
                       << " gain\n";
                report << std::left << std::setw(36)
                       << "Final checkpoint objective"
                       << ": ";
                if (perturb.target_checkpoint_enabled) {
                    report << "target score (SS/FF/Area = "
                           << perturb.target_checkpoint_alpha << "/"
                           << perturb.target_checkpoint_beta << "/"
                           << perturb.target_checkpoint_gamma << ")";
                } else {
                    report << "pipeline score";
                }
                report << '\n';
                report << std::left << std::setw(36)
                       << "Target no-improvement cycle limit"
                       << ": "
                       << perturb.max_cycles_without_target_improvement
                       << " (<=0: disabled)\n";
                report << std::left << std::setw(36)
                       << "Timing-not-worse guard"
                       << ": "
                       << (cfg.perturb_recover_require_timing_not_worse
                               ? "on"
                               : "off")
                       << '\n';
                report << std::left << std::setw(36)
                       << "NVP-growth guard"
                       << ": "
                       << (cfg.perturb_recover_require_violation_guard
                               ? "on"
                               : "off")
                       << '\n';
                report << std::left << std::setw(36)
                       << "NVP growth ratio"
                       << ": "
                       << cfg.perturb_recover_violation_growth_ratio
                       << '\n';

                write_subsection("Outcome");
                report << std::left << std::setw(36) << "Runtime (s)"
                       << ": " << perturb.runtime_seconds << '\n';
                report << std::left << std::setw(36) << "Cycles"
                       << ": " << perturb.cycles << '\n';
                int total_screened_candidates = 0;
                for (const PerturbRecoverCycleRecord& cycle :
                     perturb.cycle_records) {
                    total_screened_candidates +=
                        cycle.candidates_quick_recovered;
                }
                report << std::left << std::setw(36)
                       << "Quick-screened candidates"
                       << ": " << total_screened_candidates << '\n';
                report << std::left << std::setw(36) << "Accepted cycles"
                       << ": " << perturb.accepted_cycles << '\n';
                report << std::left << std::setw(36) << "Rejected cycles"
                       << ": " << perturb.rejected_cycles << '\n';
                report << std::left << std::setw(36) << "Best updates"
                       << ": " << perturb.best_updates << '\n';
                report << std::left << std::setw(36) << "Stop reason"
                       << ": " << perturb.stop_reason << '\n';
                report << std::left << std::setw(36) << "Perturb attempts"
                       << ": " << perturb.perturb_attempts << '\n';
                report << std::left << std::setw(36)
                       << "Perturb resize/insert/delete"
                       << ": " << perturb.perturb_resizes << " / "
                       << perturb.perturb_inserts << " / "
                       << perturb.perturb_deletes << '\n';
                report << std::left << std::setw(36)
                       << "Recovery iterations"
                       << ": " << perturb.recovery_iterations << '\n';
                report << std::left << std::setw(36)
                       << "Recovery insertions/reclaim moves"
                       << ": " << perturb.recovery_insertions << " / "
                       << perturb.recovery_reclaim_moves << '\n';
                report << std::left << std::setw(36)
                       << "Bootstrap reclaim tried/accepted"
                       << ": " << perturb.bootstrap_reclaim_tried << " / "
                       << perturb.bootstrap_reclaim_accepted << '\n';
                report << std::left << std::setw(36)
                       << "Cycle pulse batches/insertions"
                       << ": " << perturb.cycle_pulse_batch_attempts << " / "
                       << perturb.cycle_pulse_insertions << '\n';
                report << std::left << std::setw(36)
                       << "Cycle normal reclaim tried/accepted"
                       << ": " << perturb.cycle_reclaim_tried << " / "
                       << perturb.cycle_reclaim_accepted << '\n';
                report << std::left << std::setw(36)
                       << "Accepted +Buf/-Buf/resize"
                       << ": " << perturb.accepted_insertions << " / "
                       << perturb.accepted_deletes << " / "
                       << perturb.accepted_resizes << '\n';
                report << std::left << std::setw(36) << "Weighted score"
                       << ": " << std::fixed << std::setprecision(6)
                       << perturb.score_before << " -> "
                       << perturb.score_after << '\n';
                report << std::left << std::setw(36) << "Score gain"
                       << ": " << std::showpos
                       << (perturb.score_after - perturb.score_before)
                       << std::noshowpos << '\n';
                report << std::left << std::setw(36)
                       << "Checkpoint target score"
                       << ": " << perturb.target_score_before << " -> "
                       << perturb.target_score_after << '\n';
                report << std::left << std::setw(36)
                       << "Restored earlier target checkpoint"
                       << ": "
                       << (perturb.target_checkpoint_restored
                               ? "yes"
                               : "no")
                       << '\n';
                report << std::left << std::setw(36) << "Area"
                       << ": " << std::setprecision(3)
                       << perturb.area_before << " -> "
                       << perturb.area_after << '\n';

                write_subsection("Timing Change");
                report << std::left << std::setw(8) << "Corner"
                       << std::right << std::setw(14) << "TNS before"
                       << std::setw(14) << "TNS after"
                       << std::setw(14) << "WNS before"
                       << std::setw(14) << "WNS after"
                       << std::setw(14) << "Viol before"
                       << std::setw(14) << "Viol after"
                       << '\n';
                write_rule('-');
                report << std::left << std::setw(8) << "SS"
                       << std::right << std::fixed << std::setprecision(4)
                       << std::setw(14) << perturb.timing_before.ss.tns
                       << std::setw(14) << perturb.timing_after.ss.tns
                       << std::setw(14) << perturb.timing_before.ss.wns
                       << std::setw(14) << perturb.timing_after.ss.wns
                       << std::setw(14)
                       << perturb.timing_before.ss.violating_paths
                       << std::setw(14)
                       << perturb.timing_after.ss.violating_paths
                       << '\n';
                report << std::left << std::setw(8) << "FF"
                       << std::right
                       << std::setw(14) << perturb.timing_before.ff.tns
                       << std::setw(14) << perturb.timing_after.ff.tns
                       << std::setw(14) << perturb.timing_before.ff.wns
                       << std::setw(14) << perturb.timing_after.ff.wns
                       << std::setw(14)
                       << perturb.timing_before.ff.violating_paths
                       << std::setw(14)
                       << perturb.timing_after.ff.violating_paths
                       << '\n';

                if (!perturb.cycle_records.empty()) {
                    write_subsection("Perturb/Recover Cycle Detail");
                    report << std::left << std::setw(5) << "Iter"
                           << std::right << std::setw(6) << "Cand"
                           << std::setw(5) << "Q"
                           << std::setw(5) << "Sel"
                           << std::setw(10) << "Profile"
                           << std::right << std::setw(6) << "Req"
                           << std::setw(7) << "PathN"
                           << std::setw(10) << "PathArea"
                           << std::setw(5) << "G"
                           << std::setw(5) << "Rnd"
                           << std::setw(7) << "Try"
                           << std::setw(7) << "P-Rsz"
                           << std::setw(7) << "P-Ins"
                           << std::setw(7) << "P-Del"
                           << std::setw(6) << "Q-It"
                           << std::setw(6) << "D-It"
                           << std::setw(7) << "B-Acc"
                           << std::setw(7) << "P-Ins"
                           << std::setw(7) << "C-Acc"
                           << std::setw(7) << "R-Ins"
                           << std::setw(7) << "R-Rec"
                           << std::setw(13) << "ScoreBefore"
                           << std::setw(13) << "ScoreAfter"
                           << std::setw(11) << "dArea"
                           << std::setw(8) << "Accept"
                           << "  Stop\n";
                    write_rule('-');
                    for (const PerturbRecoverCycleRecord& cycle :
                         perturb.cycle_records) {
                        report << std::left << std::setw(5)
                               << cycle.cycle_index
                               << std::right << std::setw(6)
                               << cycle.candidates_generated
                               << std::setw(5)
                               << cycle.candidates_quick_recovered
                               << std::setw(5)
                               << cycle.selected_candidate
                               << std::setw(10)
                               << cycle.selected_profile
                               << std::right << std::setw(6)
                               << cycle.requested_moves
                               << std::setw(7)
                               << cycle.brutal_path_nodes
                               << std::fixed << std::setprecision(3)
                               << std::setw(10)
                               << cycle.brutal_path_area
                               << std::setw(5) << cycle.guided_moves
                               << std::setw(5) << cycle.random_moves
                               << std::setw(7) << cycle.perturb_attempts
                               << std::setw(7) << cycle.perturb_resizes
                               << std::setw(7) << cycle.perturb_inserts
                               << std::setw(7) << cycle.perturb_deletes
                               << std::setw(6)
                               << cycle.quick_recovery_iterations
                               << std::setw(6)
                               << cycle.deep_recovery_iterations
                               << std::setw(7)
                               << cycle.bootstrap_reclaim_accepted
                               << std::setw(7)
                               << cycle.cycle_pulse_insertions
                               << std::setw(7)
                               << cycle.cycle_reclaim_accepted
                               << std::setw(7) << cycle.recovery_insertions
                               << std::setw(7)
                               << cycle.recovery_reclaim_moves
                               << std::fixed << std::setprecision(6)
                               << std::setw(13) << cycle.score_before
                               << std::setw(13) << cycle.score_after
                               << std::setprecision(3)
                               << std::setw(11)
                               << (cycle.area_after - cycle.area_before)
                               << std::setw(8)
                               << (cycle.accepted ? "yes" : "no")
                               << "  " << cycle.stop_reason;
                        if (!cycle.brutal_path_leaf.empty()) {
                            report << " leaf=" << cycle.brutal_path_leaf;
                        }
                        report << '\n';
                    }
                }
            }

            write_section("Pipeline Stop Summary");
            report << std::left << std::setw(32)
                   << "Repair/Reclaim / Phase 1A stop"
                   << ": " << optimization->phase1a_stop_reason << '\n';
            report << std::left << std::setw(32)
                   << "FinalAlt stop"
                   << ": " << optimization->final_alt.stop_reason
                   << '\n';
            report << std::left << std::setw(32)
                   << "Strategy8 stop"
                   << ": " << optimization->perturb_recover.stop_reason
                   << '\n';

            write_iteration_table("Phase 1A Iteration Detail (buffer counts are cumulative)",
                                  optimization->phase1a_iteration_snapshots);

            write_iteration_table("Phase 2 Pattern Detail (buffer counts are per pass)",
                                  optimization->phase2_iteration_snapshots);

            if (!optimization->final_legality.ok) {
                write_section("Final Legality Issues");
                for (const auto& issue : optimization->final_legality.issues) {
                    report << "- " << issue << '\n';
                }
            }

            if (optimization->runtime_profile.enabled) {
                write_section("Report Generation");
                report << std::left << std::setw(32)
                       << "Report generation time (s)"
                       << ": "
                       << std::fixed << std::setprecision(4)
                       << std::chrono::duration<double>(
                              std::chrono::steady_clock::now() -
                              report_profile_start)
                              .count()
                       << '\n';
            }
        }
    }

    DEBUG_PRINT("Timing analysis");
    DEBUG_PRINT("  Clock period: " << timing.clock_period);
    DEBUG_PRINT("  Tsetup: " << timing.t_setup);
    DEBUG_PRINT("  Thold: " << timing.t_hold);
    DEBUG_PRINT("  SS: TNS=" << timing.ss.tns << " WNS=" << timing.ss.wns << " Violations=" << timing.ss.violating_paths);
    DEBUG_PRINT("  FF: TNS=" << timing.ff.tns << " WNS=" << timing.ff.wns << " Violations=" << timing.ff.violating_paths);
    DEBUG_PRINT("  Area: " << current_area);
    if (optimization) {
        if (optimization->early_stopped) {
            DEBUG_PRINT("  Optimization ended early before full completion.");
        } else {
            DEBUG_PRINT("  Optimization finished successfully.");
        }
    }

#endif

}

bool Optimizer::insert_buffer(ClockTree& tree, const std::string& parent_name,
                              const std::string& child_name, const std::string& buffer_type,
                              std::string& out_buffer_name) {
    std::string name = tree.generate_unique_name("NEW_BUF");
    bool ok = tree.insert_buffer_between(parent_name, child_name, name, buffer_type);
    if (ok) {
        out_buffer_name = name;
        DEBUG_PRINT("Inserted buffer: " << name << " between " << parent_name << " and " << child_name);
        return true;
    }
    out_buffer_name.clear();
    return false;
}
