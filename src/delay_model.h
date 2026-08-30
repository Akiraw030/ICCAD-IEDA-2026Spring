#pragma once
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include "parser.h"

struct TimingPathResult {
    std::string path_name;
    std::string launch_ff;
    std::string capture_ff;
    double data_delay = 0.0;
    double launch_clk_delay = 0.0;
    double capture_clk_delay = 0.0;
    double skew = 0.0;
    double setup_slack = 0.0;
    double hold_slack = 0.0;
};

struct TimingCornerResult {
    double tns = 0.0;
    double wns = 0.0;
    size_t violating_paths = 0;
};

struct TimingAnalysisResult {
    double clock_period = 0.0;
    double t_setup = 0.0;
    double t_hold = 0.0;
    TimingCornerResult ss;
    TimingCornerResult ff;
    std::vector<TimingPathResult> path_results;
};

struct ScoreMetrics {
    double original_ss_tns = 0.0;
    double original_ss_wns = 0.0;
    double original_ff_tns = 0.0;
    double original_ff_wns = 0.0;
    double original_area = 0.0;
    double current_ss_tns = 0.0;
    double current_ss_wns = 0.0;
    double current_ff_tns = 0.0;
    double current_ff_wns = 0.0;
    double current_area = 0.0;
    double ss_component = 0.0;
    double ff_component = 0.0;
    double area_component = 0.0;
    double total_score = 0.0;
};

struct LegalityReport {
    bool ok = true;
    std::vector<std::string> issues;
};

struct DelayModel {
    DelayModel() = default;
    explicit DelayModel(const std::vector<BufSpec>& libs);

    double estimate_buffer_area(const BufSpec& b) const { return b.width * b.height; }

    double buffer_delay_ss(const std::string& cell_type, size_t fanout) const;
    double buffer_delay_ff(const std::string& cell_type, size_t fanout) const;
    double compute_tree_area(const ClockTree& tree) const;

    std::unordered_map<std::string, double> compute_clock_arrivals(const ClockTree& tree, bool ss_corner) const;
    TimingAnalysisResult analyze_timing_from_arrivals(const std::unordered_map<std::string, double>& ss_arrival,
                                                      const std::unordered_map<std::string, double>& ff_arrival,
                                                      const std::vector<PathInfo>& ss_paths,
                                                      const std::vector<PathInfo>& ff_paths,
                                                      double clock_period) const;
    TimingAnalysisResult analyze_timing(const ClockTree& tree,
                                        const std::vector<PathInfo>& ss_paths,
                                        const std::vector<PathInfo>& ff_paths,
                                        double clock_period) const;

    ScoreMetrics compute_score_metrics(const TimingAnalysisResult& current,
                                       const TimingAnalysisResult& original,
                                       double current_area,
                                       double original_area,
                                       double alpha,
                                       double beta,
                                       double gamma) const;

    LegalityReport validate_legality(const ClockTree& tree, const std::vector<BufSpec>& libs) const;

private:
    std::unordered_map<std::string, BufSpec> lib_by_name;
    std::vector<BufSpec> lib_by_id;
};
