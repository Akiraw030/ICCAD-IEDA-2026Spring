#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "delay_model.h"
#include "parser.h"
#include "tree.h"

struct FastTimingCounters {
    double build_time_seconds = 0.0;
    double sync_time_seconds = 0.0;
    double trial_time_seconds = 0.0;
    double group_collection_time_seconds = 0.0;
    double group_update_time_seconds = 0.0;
    long long sync_count = 0;
    long long trial_count = 0;
    long long resize_trials = 0;
    long long resize_batch_trials = 0;
    long long resize_batch_candidates = 0;
    long long insert_trials = 0;
    long long delete_trials = 0;
    long long commit_count = 0;
    long long rollback_count = 0;
    long long verify_count = 0;
    long long fallback_count = 0;
    long long arrival_snapshot_count = 0;
    long long affected_group_count = 0;
    long long max_arrival_snapshots_per_trial = 0;
    long long max_affected_groups_per_trial = 0;
    bool resize_enabled = true;
    bool delete_enabled = true;
    bool insert_enabled = true;
};

struct FastTimingTrialSummary {
    bool ok = false;
    std::string reason;
    TimingAnalysisResult timing;
    double area = 0.0;
};

struct FastTimingResizeRequest {
    std::string node_name;
    std::string new_type;
};

struct FastTimingWorstViolation {
    bool valid = false;
    bool is_setup = true;
    std::string launch_ff;
    std::string capture_ff;
    std::string target_node;
    double slack = 0.0;
};

class FastTimingEngine {
public:
    struct Transaction {
        enum class Kind {
            None,
            Resize,
            ResizeBatch,
            Insert,
            Delete,
        };

        Kind kind = Kind::None;
        bool active = false;
        std::string node_name;
        std::string parent_name;
        std::string child_name;
        std::string new_buffer_name;
        int inserted_l = 0;
        int inserted_r = 0;
        std::string old_type;
        std::string new_type;
        struct ResizeChange {
            std::string node_name;
            std::string old_type;
            std::string new_type;
        };
        std::vector<ResizeChange> resize_changes;
        double old_area = 0.0;
        TimingAnalysisResult old_timing;
        struct ArrivalSnapshot {
            int sink_id = -1;
            double ss_arrival = 0.0;
            double ff_arrival = 0.0;
        };
        std::vector<ArrivalSnapshot> old_arrivals;
        struct GroupSnapshot {
            int group_id = -1;
            double setup_slack = 0.0;
            double hold_slack = 0.0;
        };
        std::vector<GroupSnapshot> old_groups;

        void reset() {
            kind = Kind::None;
            active = false;
            node_name.clear();
            parent_name.clear();
            child_name.clear();
            new_buffer_name.clear();
            inserted_l = 0;
            inserted_r = 0;
            old_type.clear();
            new_type.clear();
            resize_changes.clear();
            old_area = 0.0;
            old_timing = TimingAnalysisResult{};
            old_arrivals.clear();
            old_groups.clear();
        }
    };

    FastTimingEngine(const std::vector<BufSpec>& libs,
                     const std::vector<PathInfo>& ss_paths,
                     const std::vector<PathInfo>& ff_paths,
                     double clock_period);

    bool sync_from_tree(const ClockTree& tree);

    FastTimingTrialSummary trial_resize(const ClockTree& tree,
                                        const std::string& node_name,
                                        const std::string& new_type,
                                        Transaction& txn);
    FastTimingTrialSummary trial_resize_batch(
        const ClockTree& tree,
        const std::vector<FastTimingResizeRequest>& requests,
        Transaction& txn);
    FastTimingTrialSummary trial_delete(const ClockTree& tree,
                                        const std::string& node_name,
                                        Transaction& txn);
    FastTimingTrialSummary trial_insert_between(const ClockTree& tree,
                                                const std::string& parent_name,
                                                const std::string& child_name,
                                                const std::string& new_buffer_name,
                                                const std::string& buffer_type,
                                                Transaction& txn);

    bool commit(ClockTree& tree, Transaction& txn);
    void rollback(Transaction& txn);

    const TimingAnalysisResult& timing() const { return timing_; }
    double area() const { return area_; }
    FastTimingWorstViolation worst_violation(
        const std::set<std::string>& target_blacklist) const;
    const FastTimingCounters& counters() const { return counters_; }
    FastTimingCounters& counters() { return counters_; }

    bool verify_against_delay_model(const ClockTree& tree,
                                    const DelayModel& model,
                                    const std::vector<PathInfo>& ss_paths,
                                    const std::vector<PathInfo>& ff_paths,
                                    double clock_period,
                                    const std::string& context,
                                    double epsilon,
                                    std::ostream* err);

private:
    struct LibInfo {
        std::string name;
        double area = 0.0;
        std::vector<double> ss_delay;
        std::vector<double> ff_delay;
    };

    struct TimingGroup {
        int launch = -1;
        int capture = -1;
        bool has_setup = false;
        bool has_hold = false;
        double setup_data_delay = 0.0;
        double hold_data_delay = 0.0;
        double setup_slack = 0.0;
        double hold_slack = 0.0;
    };

    struct NodeRange {
        int l = 0;
        int r = 0;
    };

    int type_id(const std::string& type_name) const;
    bool type_supports_fanout(int type_id, size_t fanout) const;
    double type_delay(int type_id, size_t fanout, bool ss_corner) const;
    double type_area(int type_id) const;

    void rebuild_static_paths_if_needed();
    void recompute_all_group_slacks();
    void clear_corner_summary();
    void remove_group_contribution(int group_id);
    void add_group_contribution(int group_id);
    void recompute_group_slack(int group_id);
    void apply_arrival_delta(int l,
                             int r,
                             double ss_delta,
                             double ff_delta,
                             Transaction& txn);
    std::vector<int> collect_affected_groups(const Transaction& txn);
    void update_affected_groups(const std::vector<int>& group_ids,
                                Transaction& txn);
    FastTimingTrialSummary make_summary(bool ok, const std::string& reason) const;
    bool build_ranges_and_arrivals(const ClockTree& tree);

    const std::vector<BufSpec>& libs_source_;
    const std::vector<PathInfo>& ss_paths_source_;
    const std::vector<PathInfo>& ff_paths_source_;
    double clock_period_ = 0.0;
    double t_setup_ = 0.0;
    double t_hold_ = 0.0;

    std::vector<LibInfo> libs_;
    std::unordered_map<std::string, int> type_id_by_name_;

    std::vector<std::string> ff_names_;
    std::unordered_map<std::string, int> sink_id_by_name_;
    std::unordered_map<std::string, NodeRange> node_range_by_name_;

    std::vector<TimingGroup> groups_;
    std::vector<std::vector<int>> by_sink_;
    std::vector<std::uint32_t> group_visit_epoch_;
    std::uint32_t current_group_visit_epoch_ = 0;

    std::vector<double> ss_arrival_;
    std::vector<double> ff_arrival_;
    TimingAnalysisResult timing_;
    double area_ = 0.0;
    std::set<std::pair<double, int>> ss_violations_;
    std::set<std::pair<double, int>> ff_violations_;
    FastTimingCounters counters_;
};
