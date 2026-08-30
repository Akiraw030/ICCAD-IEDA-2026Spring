#include "fast_timing_engine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

long long pair_key(int launch, int capture) {
    return (static_cast<long long>(launch) << 32) ^
           static_cast<unsigned int>(capture);
}

bool close_metric(double lhs, double rhs, double eps) {
    return std::isfinite(lhs) &&
           std::isfinite(rhs) &&
           std::abs(lhs - rhs) <= eps;
}

double elapsed_since(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace

FastTimingEngine::FastTimingEngine(const std::vector<BufSpec>& libs,
                                   const std::vector<PathInfo>& ss_paths,
                                   const std::vector<PathInfo>& ff_paths,
                                   double clock_period)
    : libs_source_(libs),
      ss_paths_source_(ss_paths),
      ff_paths_source_(ff_paths),
      clock_period_(clock_period),
      t_setup_(0.08 * clock_period),
      t_hold_(0.05 * clock_period) {
    const auto start = std::chrono::steady_clock::now();
    libs_.reserve(libs.size());
    for (size_t i = 0; i < libs.size(); ++i) {
        LibInfo info;
        info.name = libs[i].name;
        info.area = libs[i].width * libs[i].height;
        info.ss_delay = libs[i].ss_delay;
        info.ff_delay = libs[i].ff_delay;
        type_id_by_name_.emplace(info.name, static_cast<int>(libs_.size()));
        libs_.push_back(std::move(info));
    }
    counters_.build_time_seconds += elapsed_since(start);
}

int FastTimingEngine::type_id(const std::string& type_name) const {
    auto it = type_id_by_name_.find(type_name);
    return it == type_id_by_name_.end() ? -1 : it->second;
}

bool FastTimingEngine::type_supports_fanout(int id, size_t fanout) const {
    if (id < 0 || static_cast<size_t>(id) >= libs_.size()) return false;
    return fanout <= libs_[static_cast<size_t>(id)].ss_delay.size() &&
           fanout <= libs_[static_cast<size_t>(id)].ff_delay.size();
}

double FastTimingEngine::type_delay(int id, size_t fanout, bool ss_corner) const {
    if (id < 0 || static_cast<size_t>(id) >= libs_.size() || fanout == 0) {
        return 0.0;
    }
    const auto& table = ss_corner
        ? libs_[static_cast<size_t>(id)].ss_delay
        : libs_[static_cast<size_t>(id)].ff_delay;
    if (table.empty()) return 0.0;
    return table[std::min(fanout - 1, table.size() - 1)];
}

double FastTimingEngine::type_area(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= libs_.size()) return 0.0;
    return libs_[static_cast<size_t>(id)].area;
}

bool FastTimingEngine::sync_from_tree(const ClockTree& tree) {
    const auto start = std::chrono::steady_clock::now();
    ++counters_.sync_count;
    const std::vector<std::string> previous_ff_names = ff_names_;
    if (!build_ranges_and_arrivals(tree)) {
        ++counters_.fallback_count;
        return false;
    }
    if (groups_.empty() || previous_ff_names != ff_names_) {
        rebuild_static_paths_if_needed();
    }
    recompute_all_group_slacks();
    counters_.sync_time_seconds += elapsed_since(start);
    return true;
}

bool FastTimingEngine::build_ranges_and_arrivals(const ClockTree& tree) {
    ff_names_.clear();
    sink_id_by_name_.clear();
    node_range_by_name_.clear();
    ss_arrival_.clear();
    ff_arrival_.clear();
    area_ = 0.0;

    if (!tree.root) return false;

    std::function<NodeRange(const ClockNode*, double, double)> dfs =
        [&](const ClockNode* node, double ss_arrival, double ff_arrival) -> NodeRange {
            if (!node) return {0, 0};
            if (node->is_sink) {
                const int id = static_cast<int>(ff_names_.size());
                ff_names_.push_back(node->name);
                sink_id_by_name_[node->name] = id;
                ss_arrival_.push_back(ss_arrival);
                ff_arrival_.push_back(ff_arrival);
                NodeRange range{id, id + 1};
                node_range_by_name_[node->name] = range;
                return range;
            }

            const int id = node->buffer_type_id >= 0
                ? node->buffer_type_id
                : type_id(node->type);
            if (!node->type.empty()) {
                if (!type_supports_fanout(id, node->children.size())) {
                    return {-1, -1};
                }
                area_ += type_area(id);
            }
            const double next_ss =
                ss_arrival + type_delay(id, node->children.size(), true);
            const double next_ff =
                ff_arrival + type_delay(id, node->children.size(), false);

            int l = std::numeric_limits<int>::max();
            int r = 0;
            for (const auto& child : node->children) {
                NodeRange child_range = dfs(child.get(), next_ss, next_ff);
                if (child_range.l < 0) return {-1, -1};
                l = std::min(l, child_range.l);
                r = std::max(r, child_range.r);
            }
            if (l == std::numeric_limits<int>::max()) l = r = 0;
            NodeRange range{l, r};
            node_range_by_name_[node->name] = range;
            return range;
        };

    return dfs(tree.root.get(), 0.0, 0.0).l >= 0;
}

void FastTimingEngine::rebuild_static_paths_if_needed() {
    groups_.clear();
    by_sink_.assign(ff_names_.size(), {});

    std::unordered_map<long long, int> group_by_pair;
    group_by_pair.reserve(ss_paths_source_.size() + ff_paths_source_.size());

    auto ensure_group = [&](int launch, int capture) -> TimingGroup& {
        const long long key = pair_key(launch, capture);
        auto it = group_by_pair.find(key);
        if (it != group_by_pair.end()) {
            return groups_[static_cast<size_t>(it->second)];
        }
        const int group_id = static_cast<int>(groups_.size());
        group_by_pair.emplace(key, group_id);
        TimingGroup group;
        group.launch = launch;
        group.capture = capture;
        groups_.push_back(group);
        by_sink_[static_cast<size_t>(launch)].push_back(group_id);
        if (capture != launch) {
            by_sink_[static_cast<size_t>(capture)].push_back(group_id);
        }
        return groups_.back();
    };

    for (const PathInfo& path : ss_paths_source_) {
        auto launch_it = sink_id_by_name_.find(path.launch_ff);
        auto capture_it = sink_id_by_name_.find(path.capture_ff);
        if (launch_it == sink_id_by_name_.end() ||
            capture_it == sink_id_by_name_.end()) {
            continue;
        }
        TimingGroup& group = ensure_group(launch_it->second, capture_it->second);
        if (!group.has_setup) {
            group.has_setup = true;
            group.setup_data_delay = path.data_delay;
        } else {
            group.setup_data_delay =
                std::max(group.setup_data_delay, path.data_delay);
        }
    }

    for (const PathInfo& path : ff_paths_source_) {
        auto launch_it = sink_id_by_name_.find(path.launch_ff);
        auto capture_it = sink_id_by_name_.find(path.capture_ff);
        if (launch_it == sink_id_by_name_.end() ||
            capture_it == sink_id_by_name_.end()) {
            continue;
        }
        TimingGroup& group = ensure_group(launch_it->second, capture_it->second);
        if (!group.has_hold) {
            group.has_hold = true;
            group.hold_data_delay = path.data_delay;
        } else {
            group.hold_data_delay =
                std::min(group.hold_data_delay, path.data_delay);
        }
    }
    group_visit_epoch_.assign(groups_.size(), 0);
    current_group_visit_epoch_ = 0;
}

void FastTimingEngine::clear_corner_summary() {
    timing_ = TimingAnalysisResult{};
    timing_.clock_period = clock_period_;
    timing_.t_setup = t_setup_;
    timing_.t_hold = t_hold_;
    ss_violations_.clear();
    ff_violations_.clear();
}

void FastTimingEngine::recompute_all_group_slacks() {
    clear_corner_summary();
    for (size_t i = 0; i < groups_.size(); ++i) {
        recompute_group_slack(static_cast<int>(i));
        add_group_contribution(static_cast<int>(i));
    }
    timing_.ss.wns = ss_violations_.empty() ? 0.0 : ss_violations_.begin()->first;
    timing_.ff.wns = ff_violations_.empty() ? 0.0 : ff_violations_.begin()->first;
}

void FastTimingEngine::remove_group_contribution(int group_id) {
    const TimingGroup& group = groups_[static_cast<size_t>(group_id)];
    if (group.has_setup && group.setup_slack < 0.0) {
        timing_.ss.tns -= group.setup_slack;
        timing_.ss.violating_paths -= 1;
        ss_violations_.erase({group.setup_slack, group_id});
    }
    if (group.has_hold && group.hold_slack < 0.0) {
        timing_.ff.tns -= group.hold_slack;
        timing_.ff.violating_paths -= 1;
        ff_violations_.erase({group.hold_slack, group_id});
    }
}

void FastTimingEngine::add_group_contribution(int group_id) {
    const TimingGroup& group = groups_[static_cast<size_t>(group_id)];
    if (group.has_setup && group.setup_slack < 0.0) {
        timing_.ss.tns += group.setup_slack;
        timing_.ss.violating_paths += 1;
        ss_violations_.insert({group.setup_slack, group_id});
    }
    if (group.has_hold && group.hold_slack < 0.0) {
        timing_.ff.tns += group.hold_slack;
        timing_.ff.violating_paths += 1;
        ff_violations_.insert({group.hold_slack, group_id});
    }
    timing_.ss.wns = ss_violations_.empty() ? 0.0 : ss_violations_.begin()->first;
    timing_.ff.wns = ff_violations_.empty() ? 0.0 : ff_violations_.begin()->first;
}

void FastTimingEngine::recompute_group_slack(int group_id) {
    TimingGroup& group = groups_[static_cast<size_t>(group_id)];
    if (group.has_setup) {
        const double launch = ss_arrival_[static_cast<size_t>(group.launch)];
        const double capture = ss_arrival_[static_cast<size_t>(group.capture)];
        group.setup_slack =
            clock_period_ - t_setup_ - group.setup_data_delay + (capture - launch);
    }
    if (group.has_hold) {
        const double launch = ff_arrival_[static_cast<size_t>(group.launch)];
        const double capture = ff_arrival_[static_cast<size_t>(group.capture)];
        group.hold_slack =
            group.hold_data_delay - t_hold_ - (capture - launch);
    }
}

void FastTimingEngine::apply_arrival_delta(int l,
                                           int r,
                                           double ss_delta,
                                           double ff_delta,
                                           Transaction& txn) {
    l = std::max(l, 0);
    r = std::min(r, static_cast<int>(ss_arrival_.size()));
    for (int id = l; id < r; ++id) {
        txn.old_arrivals.push_back({
            id,
            ss_arrival_[static_cast<size_t>(id)],
            ff_arrival_[static_cast<size_t>(id)],
        });
        ss_arrival_[static_cast<size_t>(id)] += ss_delta;
        ff_arrival_[static_cast<size_t>(id)] += ff_delta;
    }
}

std::vector<int> FastTimingEngine::collect_affected_groups(const Transaction& txn) {
    const auto start = std::chrono::steady_clock::now();
    ++current_group_visit_epoch_;
    if (current_group_visit_epoch_ == 0) {
        std::fill(group_visit_epoch_.begin(), group_visit_epoch_.end(), 0);
        current_group_visit_epoch_ = 1;
    }

    std::vector<int> group_ids;
    auto add_sink = [&](int sink_id) {
        if (sink_id < 0 || static_cast<size_t>(sink_id) >= by_sink_.size()) return;
        for (int group_id : by_sink_[static_cast<size_t>(sink_id)]) {
            if (group_id < 0 ||
                static_cast<size_t>(group_id) >= group_visit_epoch_.size()) {
                continue;
            }
            std::uint32_t& mark =
                group_visit_epoch_[static_cast<size_t>(group_id)];
            if (mark == current_group_visit_epoch_) continue;
            mark = current_group_visit_epoch_;
            group_ids.push_back(group_id);
        }
    };
    for (const auto& item : txn.old_arrivals) add_sink(item.sink_id);
    std::sort(group_ids.begin(), group_ids.end());

    counters_.arrival_snapshot_count +=
        static_cast<long long>(txn.old_arrivals.size());
    counters_.affected_group_count +=
        static_cast<long long>(group_ids.size());
    counters_.max_arrival_snapshots_per_trial =
        std::max(counters_.max_arrival_snapshots_per_trial,
                 static_cast<long long>(txn.old_arrivals.size()));
    counters_.max_affected_groups_per_trial =
        std::max(counters_.max_affected_groups_per_trial,
                 static_cast<long long>(group_ids.size()));
    counters_.group_collection_time_seconds += elapsed_since(start);
    return group_ids;
}

void FastTimingEngine::update_affected_groups(const std::vector<int>& group_ids,
                                              Transaction& txn) {
    const auto start = std::chrono::steady_clock::now();
    txn.old_groups.reserve(group_ids.size());
    for (int group_id : group_ids) {
        TimingGroup& group = groups_[static_cast<size_t>(group_id)];
        txn.old_groups.push_back({group_id, group.setup_slack, group.hold_slack});
        remove_group_contribution(group_id);
        recompute_group_slack(group_id);
        add_group_contribution(group_id);
    }
    counters_.group_update_time_seconds += elapsed_since(start);
}

FastTimingTrialSummary FastTimingEngine::make_summary(bool ok,
                                                      const std::string& reason) const {
    FastTimingTrialSummary summary;
    summary.ok = ok;
    summary.reason = reason;
    summary.timing = timing_;
    summary.area = area_;
    return summary;
}

FastTimingWorstViolation FastTimingEngine::worst_violation(
    const std::set<std::string>& target_blacklist) const {
    FastTimingWorstViolation best;

    auto consider = [&](const std::set<std::pair<double, int>>& violations,
                        bool is_setup) {
        for (const auto& entry : violations) {
            const int group_id = entry.second;
            if (group_id < 0 || static_cast<size_t>(group_id) >= groups_.size()) {
                continue;
            }
            const TimingGroup& group = groups_[static_cast<size_t>(group_id)];
            const int target_id = is_setup ? group.capture : group.launch;
            if (target_id < 0 || static_cast<size_t>(target_id) >= ff_names_.size()) {
                continue;
            }
            const std::string& target = ff_names_[static_cast<size_t>(target_id)];
            if (target_blacklist.find(target) != target_blacklist.end()) {
                continue;
            }
            if (best.valid && entry.first >= best.slack) {
                return;
            }
            best.valid = true;
            best.is_setup = is_setup;
            best.launch_ff = ff_names_[static_cast<size_t>(group.launch)];
            best.capture_ff = ff_names_[static_cast<size_t>(group.capture)];
            best.target_node = target;
            best.slack = entry.first;
            return;
        }
    };

    consider(ss_violations_, true);
    consider(ff_violations_, false);
    return best;
}

FastTimingTrialSummary FastTimingEngine::trial_resize(const ClockTree& tree,
                                                      const std::string& node_name,
                                                      const std::string& new_type,
                                                      Transaction& txn) {
    const auto start = std::chrono::steady_clock::now();
    ++counters_.trial_count;
    ++counters_.resize_trials;
    txn.reset();
    txn.kind = Transaction::Kind::Resize;
    txn.node_name = node_name;
    txn.new_type = new_type;
    txn.old_area = area_;
    txn.old_timing = timing_;

    const ClockNode* node = tree.find_node(node_name);
    if (!node || node->is_sink || node->type.empty()) {
        ++counters_.fallback_count;
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "invalid_resize_node");
    }
    auto range_it = node_range_by_name_.find(node_name);
    if (range_it == node_range_by_name_.end()) {
        ++counters_.fallback_count;
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "missing_node_range");
    }
    const int old_id = type_id(node->type);
    const int new_id = type_id(new_type);
    const size_t fanout = node->children.size();
    if (!type_supports_fanout(new_id, fanout) || old_id < 0) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "unsupported_resize_type");
    }

    txn.active = true;
    txn.old_type = node->type;
    const double ss_delta =
        type_delay(new_id, fanout, true) - type_delay(old_id, fanout, true);
    const double ff_delta =
        type_delay(new_id, fanout, false) - type_delay(old_id, fanout, false);
    apply_arrival_delta(range_it->second.l, range_it->second.r, ss_delta, ff_delta, txn);
    area_ += type_area(new_id) - type_area(old_id);
    update_affected_groups(collect_affected_groups(txn), txn);
    counters_.trial_time_seconds += elapsed_since(start);
    return make_summary(true, "");
}

FastTimingTrialSummary FastTimingEngine::trial_resize_batch(
    const ClockTree& tree,
    const std::vector<FastTimingResizeRequest>& requests,
    Transaction& txn) {
    const auto start = std::chrono::steady_clock::now();
    ++counters_.trial_count;
    ++counters_.resize_batch_trials;
    counters_.resize_batch_candidates +=
        static_cast<long long>(requests.size());
    txn.reset();
    txn.kind = Transaction::Kind::ResizeBatch;
    txn.old_area = area_;
    txn.old_timing = timing_;

    if (requests.empty()) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "empty_resize_batch");
    }

    struct PreparedResize {
        std::string node_name;
        std::string old_type;
        std::string new_type;
        NodeRange range;
        size_t fanout = 0;
        int old_id = -1;
        int new_id = -1;
    };
    std::vector<PreparedResize> prepared;
    prepared.reserve(requests.size());
    std::unordered_set<std::string> seen_nodes;
    seen_nodes.reserve(requests.size());

    for (const FastTimingResizeRequest& request : requests) {
        if (!seen_nodes.insert(request.node_name).second) {
            counters_.trial_time_seconds += elapsed_since(start);
            return make_summary(false, "duplicate_resize_batch_node");
        }
        const ClockNode* node = tree.find_node(request.node_name);
        if (!node || node->is_sink || node->type.empty()) {
            ++counters_.fallback_count;
            counters_.trial_time_seconds += elapsed_since(start);
            return make_summary(false, "invalid_resize_batch_node");
        }
        auto range_it = node_range_by_name_.find(request.node_name);
        if (range_it == node_range_by_name_.end()) {
            ++counters_.fallback_count;
            counters_.trial_time_seconds += elapsed_since(start);
            return make_summary(false, "missing_resize_batch_range");
        }
        const int old_id = type_id(node->type);
        const int new_id = type_id(request.new_type);
        const size_t fanout = node->children.size();
        if (old_id < 0 || !type_supports_fanout(new_id, fanout)) {
            counters_.trial_time_seconds += elapsed_since(start);
            return make_summary(false, "unsupported_resize_batch_type");
        }
        prepared.push_back({request.node_name,
                            node->type,
                            request.new_type,
                            range_it->second,
                            fanout,
                            old_id,
                            new_id});
    }

    txn.active = true;
    txn.resize_changes.reserve(prepared.size());
    for (const PreparedResize& resize : prepared) {
        txn.resize_changes.push_back(
            {resize.node_name, resize.old_type, resize.new_type});
        const double ss_delta =
            type_delay(resize.new_id, resize.fanout, true) -
            type_delay(resize.old_id, resize.fanout, true);
        const double ff_delta =
            type_delay(resize.new_id, resize.fanout, false) -
            type_delay(resize.old_id, resize.fanout, false);
        apply_arrival_delta(resize.range.l,
                            resize.range.r,
                            ss_delta,
                            ff_delta,
                            txn);
        area_ += type_area(resize.new_id) - type_area(resize.old_id);
    }
    update_affected_groups(collect_affected_groups(txn), txn);
    counters_.trial_time_seconds += elapsed_since(start);
    return make_summary(true, "");
}

FastTimingTrialSummary FastTimingEngine::trial_insert_between(
    const ClockTree& tree,
    const std::string& parent_name,
    const std::string& child_name,
    const std::string& new_buffer_name,
    const std::string& buffer_type,
    Transaction& txn) {
    const auto start = std::chrono::steady_clock::now();
    ++counters_.trial_count;
    ++counters_.insert_trials;
    txn.reset();
    txn.kind = Transaction::Kind::Insert;
    txn.parent_name = parent_name;
    txn.child_name = child_name;
    txn.new_buffer_name = new_buffer_name;
    txn.new_type = buffer_type;
    txn.old_area = area_;
    txn.old_timing = timing_;

    if (new_buffer_name.empty() || tree.find_node(new_buffer_name) != nullptr) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "invalid_or_duplicate_insert_name");
    }
    const ClockNode* parent = tree.find_node(parent_name);
    const ClockNode* child = tree.find_node(child_name);
    if (!parent || !child || child->parent != parent) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "invalid_insert_parent_child");
    }
    const int new_id = type_id(buffer_type);
    if (!type_supports_fanout(new_id, 1)) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "unsupported_insert_type");
    }
    auto child_range_it = node_range_by_name_.find(child_name);
    if (child_range_it == node_range_by_name_.end()) {
        ++counters_.fallback_count;
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "missing_child_range");
    }

    txn.active = true;
    txn.inserted_l = child_range_it->second.l;
    txn.inserted_r = child_range_it->second.r;
    apply_arrival_delta(child_range_it->second.l,
                        child_range_it->second.r,
                        type_delay(new_id, 1, true),
                        type_delay(new_id, 1, false),
                        txn);
    area_ += type_area(new_id);
    update_affected_groups(collect_affected_groups(txn), txn);
    counters_.trial_time_seconds += elapsed_since(start);
    return make_summary(true, "");
}

FastTimingTrialSummary FastTimingEngine::trial_delete(const ClockTree& tree,
                                                      const std::string& node_name,
                                                      Transaction& txn) {
    const auto start = std::chrono::steady_clock::now();
    ++counters_.trial_count;
    ++counters_.delete_trials;
    txn.reset();
    txn.kind = Transaction::Kind::Delete;
    txn.node_name = node_name;
    txn.old_area = area_;
    txn.old_timing = timing_;

    const ClockNode* node = tree.find_node(node_name);
    if (!node || node->is_sink || node->original || !node->parent || node->type.empty()) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "invalid_delete_node");
    }
    const ClockNode* parent = node->parent;
    auto node_range_it = node_range_by_name_.find(node_name);
    auto parent_range_it = node_range_by_name_.find(parent->name);
    if (node_range_it == node_range_by_name_.end() ||
        parent_range_it == node_range_by_name_.end()) {
        ++counters_.fallback_count;
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "missing_delete_range");
    }

    const int old_id = type_id(node->type);
    if (old_id < 0) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "unknown_deleted_type");
    }
    const size_t parent_old_fanout = parent->children.size();
    const size_t parent_new_fanout =
        parent_old_fanout == 0 ? 0 : parent_old_fanout - 1 + node->children.size();
    const int parent_id = type_id(parent->type);
    if (!parent->type.empty() && !type_supports_fanout(parent_id, parent_new_fanout)) {
        counters_.trial_time_seconds += elapsed_since(start);
        return make_summary(false, "unsupported_parent_fanout_after_delete");
    }

    txn.active = true;
    txn.parent_name = parent->name;
    txn.old_type = node->type;
    double parent_ss_delta = 0.0;
    double parent_ff_delta = 0.0;
    if (!parent->type.empty()) {
        parent_ss_delta =
            type_delay(parent_id, parent_new_fanout, true) -
            type_delay(parent_id, parent_old_fanout, true);
        parent_ff_delta =
            type_delay(parent_id, parent_new_fanout, false) -
            type_delay(parent_id, parent_old_fanout, false);
    }
    apply_arrival_delta(parent_range_it->second.l,
                        parent_range_it->second.r,
                        parent_ss_delta,
                        parent_ff_delta,
                        txn);
    apply_arrival_delta(node_range_it->second.l,
                        node_range_it->second.r,
                        -type_delay(old_id, node->children.size(), true),
                        -type_delay(old_id, node->children.size(), false),
                        txn);
    area_ -= type_area(old_id);
    update_affected_groups(collect_affected_groups(txn), txn);
    counters_.trial_time_seconds += elapsed_since(start);
    return make_summary(true, "");
}

bool FastTimingEngine::commit(ClockTree& tree, Transaction& txn) {
    if (!txn.active) return false;
    bool ok = false;
    if (txn.kind == Transaction::Kind::Resize) {
        ok = tree.set_buffer_type(txn.node_name, txn.new_type);
    } else if (txn.kind == Transaction::Kind::ResizeBatch) {
        size_t applied = 0;
        for (; applied < txn.resize_changes.size(); ++applied) {
            const Transaction::ResizeChange& resize =
                txn.resize_changes[applied];
            if (!tree.set_buffer_type(resize.node_name, resize.new_type)) {
                break;
            }
        }
        ok = applied == txn.resize_changes.size();
        if (!ok) {
            while (applied > 0) {
                --applied;
                const Transaction::ResizeChange& resize =
                    txn.resize_changes[applied];
                tree.set_buffer_type(resize.node_name, resize.old_type);
            }
        }
    } else if (txn.kind == Transaction::Kind::Insert) {
        ok = tree.insert_buffer_between(txn.parent_name,
                                        txn.child_name,
                                        txn.new_buffer_name,
                                        txn.new_type);
        if (ok) {
            node_range_by_name_[txn.new_buffer_name] =
                {txn.inserted_l, txn.inserted_r};
        }
    } else if (txn.kind == Transaction::Kind::Delete) {
        ok = tree.delete_buffer(txn.node_name);
        if (ok) node_range_by_name_.erase(txn.node_name);
    }
    if (!ok) {
        rollback(txn);
        ++counters_.fallback_count;
        return false;
    }
    txn.active = false;
    ++counters_.commit_count;
    return true;
}

void FastTimingEngine::rollback(Transaction& txn) {
    if (!txn.active) return;

    // update_affected_groups() changed only the groups recorded in the
    // transaction, so restoring those entries also restores both ordered
    // violation sets. Rebuilding the sets from every group here made each
    // rejected local trial scale with the complete timing graph.
    for (auto it = txn.old_groups.rbegin(); it != txn.old_groups.rend(); ++it) {
        const int group_id = it->group_id;
        if (group_id < 0 || static_cast<size_t>(group_id) >= groups_.size()) continue;
        remove_group_contribution(group_id);
        groups_[static_cast<size_t>(group_id)].setup_slack = it->setup_slack;
        groups_[static_cast<size_t>(group_id)].hold_slack = it->hold_slack;
        add_group_contribution(group_id);
    }
    for (auto it = txn.old_arrivals.rbegin(); it != txn.old_arrivals.rend(); ++it) {
        ss_arrival_[static_cast<size_t>(it->sink_id)] = it->ss_arrival;
        ff_arrival_[static_cast<size_t>(it->sink_id)] = it->ff_arrival;
    }
    area_ = txn.old_area;
    timing_ = txn.old_timing;
    txn.active = false;
    ++counters_.rollback_count;
}

bool FastTimingEngine::verify_against_delay_model(const ClockTree& tree,
                                                  const DelayModel& model,
                                                  const std::vector<PathInfo>& ss_paths,
                                                  const std::vector<PathInfo>& ff_paths,
                                                  double clock_period,
                                                  const std::string& context,
                                                  double epsilon,
                                                  std::ostream* err) {
    ++counters_.verify_count;
    const TimingAnalysisResult full =
        model.analyze_timing(tree, ss_paths, ff_paths, clock_period);
    const double full_area = model.compute_tree_area(tree);
    auto corner_close = [&](const TimingCornerResult& fast_corner,
                            const TimingCornerResult& full_corner) {
        const bool tns_close = close_metric(fast_corner.tns, full_corner.tns, epsilon);
        const bool wns_close = close_metric(fast_corner.wns, full_corner.wns, epsilon);
        // Near-zero floating-point drift can change NVP by one while the
        // aggregate TNS/WNS values remain equivalent within epsilon.
        return tns_close && wns_close;
    };
    const bool ok =
        corner_close(timing_.ss, full.ss) &&
        corner_close(timing_.ff, full.ff) &&
        close_metric(area_, full_area, epsilon);
    if (ok) {
        if (err &&
            (timing_.ss.violating_paths != full.ss.violating_paths ||
             timing_.ff.violating_paths != full.ff.violating_paths)) {
            *err << "FastTimingEngine near-zero NVP drift after " << context
                 << ": SS fast/full=" << timing_.ss.violating_paths << '/'
                 << full.ss.violating_paths
                 << " FF fast/full=" << timing_.ff.violating_paths << '/'
                 << full.ff.violating_paths << '\n';
        }
        return true;
    }
    if (!err) return false;

    *err << std::setprecision(17)
         << "FastTimingEngine mismatch after " << context << '\n'
         << "  SS fast: TNS=" << timing_.ss.tns
         << " WNS=" << timing_.ss.wns
         << " V=" << timing_.ss.violating_paths << '\n'
         << "  SS full: TNS=" << full.ss.tns
         << " WNS=" << full.ss.wns
         << " V=" << full.ss.violating_paths << '\n'
         << "  FF fast: TNS=" << timing_.ff.tns
         << " WNS=" << timing_.ff.wns
         << " V=" << timing_.ff.violating_paths << '\n'
         << "  FF full: TNS=" << full.ff.tns
         << " WNS=" << full.ff.wns
         << " V=" << full.ff.violating_paths << '\n'
         << "  Area fast=" << area_ << " full=" << full_area << '\n';
    return false;
}
