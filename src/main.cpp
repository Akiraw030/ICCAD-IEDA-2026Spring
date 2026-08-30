#include <iostream>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>
#include "parser.h"
#include "io.h"
#include "optimizer.h"

namespace {

struct Phase0BranchResult {
    std::string name;
    OptimizationSummary summary;
};

Phase0BranchResult run_phase0_branch(const ClockTree& original_tree,
                                     const std::vector<BufSpec>& libs,
                                     const std::vector<PathInfo>& ss_paths,
                                     const std::vector<PathInfo>& ff_paths,
                                     double clock_period,
                                     const OptimizerConfig& config,
                                     const std::string& branch_name,
                                     const std::filesystem::path& report_path,
                                     const std::string& testcase_dir) {
    ClockTree branch_tree = original_tree.clone();
    Optimizer branch_optimizer(config);
    OptimizationSummary summary =
        branch_optimizer.optimize(branch_tree,
                                  libs,
                                  ss_paths,
                                  ff_paths,
                                  clock_period,
                                  true,
                                  branch_name);
    branch_optimizer.analyze(branch_tree,
                             libs,
                             ss_paths,
                             ff_paths,
                             clock_period,
                             report_path.string(),
                             testcase_dir,
                             &summary);
    return {branch_name, std::move(summary)};
}

bool write_phase0_comparison(const std::filesystem::path& path,
                             const std::vector<Phase0BranchResult>& branches) {
    std::ofstream report(path);
    if (!report) return false;

    report << "Phase 0 Reset Experiment Comparison\n";
    report << std::string(170, '=') << '\n';
    report << std::left << std::setw(24) << "Branch"
           << std::right
           << std::setw(11) << "SS TNS"
           << std::setw(10) << "SS WNS"
           << std::setw(8) << "SS V"
           << " |"
           << std::setw(11) << "FF TNS"
           << std::setw(10) << "FF WNS"
           << std::setw(8) << "FF V"
           << " |"
           << std::setw(12) << "Area"
           << std::setw(9) << "+Buf"
           << std::setw(9) << "-Buf"
           << std::setw(9) << "Resize"
           << std::setw(12) << "Runtime"
           << std::setw(9) << "Legal"
           << std::setw(12) << "Score"
           << '\n';
    report << std::string(170, '-') << '\n';

    for (const auto& branch : branches) {
        const OptimizationSummary& summary = branch.summary;
        const int inserted = summary.phase1a_insertions + summary.phase1b_insertions;
        const int resized = summary.phase0.reset_resizes +
                            summary.phase0.accepted_resizes +
                            summary.phase2_downsizes;
        report << std::left << std::setw(24) << branch.name
               << std::right << std::fixed << std::setprecision(4)
               << std::setw(11) << summary.final_timing.ss.tns
               << std::setw(10) << summary.final_timing.ss.wns
               << std::setw(8) << summary.final_timing.ss.violating_paths
               << " |"
               << std::setw(11) << summary.final_timing.ff.tns
               << std::setw(10) << summary.final_timing.ff.wns
               << std::setw(8) << summary.final_timing.ff.violating_paths
               << " |"
               << std::setprecision(3)
               << std::setw(12) << summary.final_area
               << std::setw(9) << inserted
               << std::setw(9) << summary.phase2_removals
               << std::setw(9) << resized
               << std::setw(12) << summary.runtime_seconds
               << std::setw(9) << (summary.final_legality.ok ? "OK" : "FAIL")
               << std::setprecision(6)
               << std::setw(12) << summary.final_score.total_score
               << '\n';
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
#ifdef ENABLE_OUTPUT
        std::cout << "Usage: cadd0045 <testcase_dir_absolute_path> <output_file_absolute_path>" << std::endl;
#endif
        return 1;
    }

    std::string testcase_dir = argv[1];
    std::string output_file = argv[2];

    ClockTree tree;
    if (!parse_clk_tree(testcase_dir + "/clk_tree.structure", tree)) {
        std::cerr << "Failed to parse clk_tree.structure" << std::endl;
        return 1;
    }

    std::vector<BufSpec> libs;
    if (!parse_buf_lib(testcase_dir + "/buf.lib", libs)) {
        std::cerr << "Failed to parse buf.lib" << std::endl;
        return 1;
    }

    std::vector<std::string> buffer_type_names;
    buffer_type_names.reserve(libs.size());
    for (const auto& lib : libs) buffer_type_names.push_back(lib.name);
    if (!tree.configure_buffer_types(buffer_type_names)) {
        std::cerr << "Clock tree contains a buffer type not present in buf.lib" << std::endl;
        return 1;
    }

    DelayModel legality_model(libs);
    LegalityReport legality = legality_model.validate_legality(tree, libs);
    if (!legality.ok) {
        std::cerr << "Clock tree legality check failed:" << std::endl;
        for (const auto& issue : legality.issues) {
            std::cerr << "  - " << issue << std::endl;
        }
        return 1;
    }

    std::vector<PathInfo> ss_paths;
    std::vector<PathInfo> ff_paths;
    double ss_clock_period = 0.0;
    double ff_clock_period = 0.0;
    if (!parse_rpt(testcase_dir + "/SS_delay.rpt", ss_paths, ss_clock_period)) {
        std::cerr << "Failed to parse SS_delay.rpt" << std::endl;
        return 1;
    }
    if (!parse_rpt(testcase_dir + "/FF_delay.rpt", ff_paths, ff_clock_period)) {
        std::cerr << "Failed to parse FF_delay.rpt" << std::endl;
        return 1;
    }

    double clock_period = ss_clock_period > 0.0 ? ss_clock_period : ff_clock_period;
    if (clock_period <= 0.0) {
        std::cerr << "Failed to determine clock period" << std::endl;
        return 1;
    }

    OptimizerConfig config;
    ClockTree original_tree;
    if (config.enable_phase0_reset_experiment) {
        original_tree = tree.clone();
    }
    Optimizer optimizer(config);
    OptimizationSummary optimization = optimizer.optimize(tree, libs, ss_paths, ff_paths, clock_period);
    if (!optimization.success) {
        std::cerr << optimization.message << std::endl;
        for (const auto& issue : optimization.final_legality.issues) {
            std::cerr << "  - " << issue << std::endl;
        }
        return 1;
    }

#ifdef ENABLE_OUTPUT
    std::cout << optimization.message << std::endl;
    if (!optimization.applied_moves.empty()) {
        std::cout << "Optimization moves applied: " << optimization.applied_moves.size() << std::endl;
    }
    std::cout << "Phase 1A buffers added: " << optimization.phase1a_insertions << std::endl;
    std::cout << "Phase 1B buffers added: " << optimization.phase1b_insertions << std::endl;
    std::cout << "Phase 2 buffers removed: " << optimization.phase2_removals << std::endl;
    std::cout << "Phase 2 buffers downsized: " << optimization.phase2_downsizes << std::endl;

    // Construct timing report filename that includes the testcase folder basename,
    // e.g. timing_report_testcase0.txt
    std::string testcase_base = std::filesystem::path(testcase_dir).filename().string();
    std::filesystem::path report_path = std::filesystem::path(output_file).parent_path() / (std::string("timing_report_") + testcase_base + ".txt");
    optimizer.analyze(tree, libs, ss_paths, ff_paths, clock_period, report_path.string(), testcase_dir, &optimization);
#endif

    // Write the current tree to the requested output path.
    if (!write_clk_tree(output_file, tree)) {
        std::cerr << "Failed to write output file" << std::endl;
        return 1;
    }

    if (config.enable_phase0_reset_experiment) {
        std::vector<Phase0BranchResult> branch_results;
        const std::filesystem::path output_dir = std::filesystem::path(output_file).parent_path();
        const std::string experiment_testcase_base = std::filesystem::path(testcase_dir).filename().string();

        branch_results.push_back({"normal", optimization});
        const std::filesystem::path experiment_report_path =
            output_dir / ("timing_report_" + experiment_testcase_base +
                          "_reset_phase0_experiment.txt");
        branch_results.push_back(run_phase0_branch(original_tree,
                                                   libs,
                                                   ss_paths,
                                                   ff_paths,
                                                   clock_period,
                                                   config,
                                                   "reset_phase0_experiment",
                                                   experiment_report_path,
                                                   testcase_dir));

        const std::filesystem::path comparison_path =
            output_dir / ("timing_report_" + experiment_testcase_base + "_phase0_compare.txt");
        if (!write_phase0_comparison(comparison_path, branch_results)) {
            std::cerr << "Failed to write Phase 0 comparison report: "
                      << comparison_path << std::endl;
            return 1;
        }
    }

#ifdef ENABLE_OUTPUT
    if (optimization.early_stopped) {
        std::cout << "Run finished early; output written to " << output_file << std::endl;
    } else {
        std::cout << "Run completed successfully; output written to " << output_file << std::endl;
    }
#endif
    return 0;
}
