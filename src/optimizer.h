#pragma once
#include <chrono>
#include <vector>
#include <string>
#include "tree.h"
#include "delay_model.h"

struct OptimizerConfig {
    // Global objective and runtime policy.
    int time_limit_seconds = 570;                 // Hard limit for the complete optimizer run.
    double alpha = 0.20;                          // Weight of normalized SS timing improvement.
    double beta = 0.20;                           // Weight of normalized FF timing improvement.
    double gamma = 0.60;                          // Weight of normalized clock-tree area improvement.
    // Optional coarse-to-fine objective schedule.  It deliberately keeps the
    // early search area-heavy, then follows a cosine ramp toward a
    // timing-heavy objective over Repair/Reclaim cycles.
    bool enable_score_weight_scheduler = false;   // Enable the 0.10/0.10/0.80 -> 0.60/0.20/0.20 cosine objective schedule; E31's testcase3 screen lost target score, so keep it optional.
    bool score_weight_schedule_apply_to_phase0 = true; // Use the scheduler's initial weights for Phase0 and the initial shared-reclaim cycle.
    double score_weight_schedule_start_alpha = 0.10; // Initial SS timing weight during the area-focused hold.
    double score_weight_schedule_start_beta = 0.10;  // Initial FF timing weight during the area-focused hold.
    double score_weight_schedule_start_gamma = 0.80; // Initial area weight during the area-focused hold.
    double score_weight_schedule_end_alpha = 0.60; // Final SS timing weight after the cosine ramp.
    double score_weight_schedule_end_beta = 0.20;  // Final FF timing weight after the cosine ramp.
    double score_weight_schedule_end_gamma = 0.20; // Final area weight after the cosine ramp.
    int score_weight_schedule_hold_repair_cycles = 4; // Whole Repair/Reclaim cycles kept at the area-focused start point.
    int score_weight_schedule_transition_repair_cycles = 12; // Number of later cycles used by the smooth half-cosine transition.
    bool enable_phase0_score_based_acceptance = true; // Use weighted score for Phase 0 move acceptance.
    bool enable_repair_reclaim_score_based_acceptance = true; // Use weighted score for repair/reclaim moves.
    bool enable_final_alt_score_based_acceptance = false; // Use weighted score for FinalAlt moves.
    double score_acceptance_epsilon = 1e-9;       // Minimum meaningful weighted-score difference.
    bool enable_best_checkpoint = false;          // Keep one in-memory copy of the best tree seen.
    bool restore_best_checkpoint_at_end = true;   // Restore that best tree before final validation.

    // Phase 0: pressure-guided resizing of existing buffers.
    bool enable_phase0 = true;                    // Run Phase 0 before repair/reclaim.
    bool enable_phase0_reset_experiment = false;  // Compare against a fastest-buffer reset branch.
    double phase0_wns_weight = 0.5;               // Legacy timing-cost WNS weight when score mode is off.
    double phase0_area_tiebreak_penalty = 0.01;   // Area-growth penalty used in candidate ranking.
    double phase0_time_budget_seconds = 180.0;    // Phase 0 wall-clock safety limit.
    double phase0_node_fraction = 1.0;            // Fraction of ranked pressure nodes scanned per pass.
    int phase0_max_trial_nodes = 0;            // Total nodes examined across passes; <= 0 is unlimited.
    int phase0_max_types_per_node = 6;            // Maximum buffer types tried for each selected node. #TODO
    int phase0_max_passes = 5;                    // Maximum pressure-recompute passes. #TODO
    bool enable_phase0_score_aligned_ranking = false; // Rank trials by estimated alpha/beta/gamma score gain.
    bool enable_phase0_incremental_timing = true; // Use incremental timing if the fast engine is unavailable.
    bool enable_phase0_incremental_verify = false; // Check incremental timing against full timing.
    bool enable_phase0_batch_trial = false;       // Force batch trials on every case.
    bool enable_phase0_batch_auto = false;        // Automatically batch sufficiently large cases.
    int phase0_batch_size = 12;                   // Maximum independent resize candidates per batch.
    bool enable_phase0_fast_batch_timing = true;  // Evaluate Phase 0 batches with one FastTimingEngine transaction.
    bool phase0_batch_split_on_fail = true;       // Recursively split rejected batches.
    int phase0_batch_max_split_depth = 2;         // Maximum recursive split levels after a rejected batch.
    int phase0_batch_auto_min_ss_violations = 15000; // SS violations required for automatic batching.
    int phase0_batch_auto_min_pressure_candidates = 10000; // Candidates required for automatic batching.
    int phase0_max_consecutive_rejects = 1000;    // Individual rejections allowed before stopping a pass. #TODO
    int phase0_max_consecutive_failed_batches = 300; // Failed batches allowed before stopping a pass.
    double phase0_pressure_epsilon = 1e-12;       // Pressure magnitude treated as zero.
    double phase0_timing_improvement_epsilon = 1e-9; // Epsilon for comparing acceptable candidates.
    double phase0_wns_tolerance = 1e-9;           // Numerical tolerance for the Phase 0 WNS Pareto guard.
    double phase0_tns_tolerance = 1e-9;           // Numerical tolerance for the Phase 0 TNS Pareto guard.

    // Repair/Reclaim cycles.
    bool enable_repair_reclaim_cycles = true;     // Alternate repair pulses and area reclaim.
    double repair_reclaim_cycle_end_time_seconds = 360.0; // Absolute elapsed-time boundary for this stage.
    int repair_reclaim_max_cycles = 200;          // Maximum repair/reclaim cycles.
    double phase1a_pulse_delay_scale = 1.0;       // Base requested-delay multiplier for repair pulses.
    bool repair_pulse_split_on_fail = true;       // Split a rejected repair pulse and retry both halves.
    int repair_pulse_max_split_depth = 2;         // Stop retrying a rejected branch at this split depth.
    bool enable_repair_pulse_failure_recovery = true; // Retry an exhausted pulse with a lower scale and temporary target blacklist.
    int repair_pulse_max_consecutive_failures = 3; // Stop the stage after this many recoverable pulse failures in a row.
    double repair_pulse_recovery_scale_multiplier = 0.70; // Multiply pulse delay scale after each recoverable failure.
    double repair_pulse_recovery_min_delay_scale = 0.50; // Lower bound for the recovery pulse delay scale.
    int repair_pulse_recovery_blacklist_targets = 8; // Worst failed targets temporarily skipped per recovery.
    bool repair_pulse_require_both_tns_not_worse = false; // AND both corner TNS guards; false selects guarded OR (default).
    double repair_pulse_closed_tns_epsilon = 1e-9; // Treat a corner TNS within this magnitude as closed.
    double repair_pulse_required_tns_improvement = 1e-9; // Strict SS gain required after FF closes in guarded OR.
    bool enable_shared_path_repair = false;        // Try shared sibling-subtree repair inside Repair/Reclaim; disabled because it is isolated into the post-Phase0 cycle by default.
    bool enable_phase0_existing_buffer_shared_reclaim_cycle = true; // After Phase0, run one shared-buffer-only repair/reclaim cycle restricted to original buffer-to-buffer edges.
    bool enable_existing_buffer_shared_reclaim_alternation = true; // Repeat the post-Phase0 existing-buffer shared/reclaim cycle before every later Repair/Reclaim cycle.
    bool existing_shared_disable_on_large_case = true; // Skip the expensive shared stage/alternation on large path sets; E10's 530k run spent 42.8 s here without accepting a move.
    bool existing_shared_children_must_be_buffers = false; // In the post-Phase0 shared cycle, require selected original children to be buffers; false also permits original FF children.
    bool existing_shared_disable_alternation_if_initial_no_insertion = false; // Optional aggressive shortcut; false lets later tree changes make shared repair useful.
    int existing_shared_max_consecutive_no_insertion_cycles = 2; // Disable later existing-buffer shared cycles after this many consecutive zero-insertion results; <= 0 disables the streak guard.
    int repair_shared_max_consecutive_no_insertion_pulses = 2; // Disable shared-path repair for the rest of Repair/Reclaim after this many consecutive pulses accept no shared insertion; <= 0 disables this safeguard.
    int repair_shared_max_candidates_per_pulse = 16; // Highest-value shared sibling groups trialed in one pulse.
    int repair_shared_max_children = 0;            // Children grouped under one buffer; <= 0 uses the buf.lib fanout limit.
    int repair_shared_types_per_parent = 2;        // Buffer types retained per parent for shared-path trials.
    double repair_shared_min_edge_pressure = 1e-9; // Minimum positive signed subtree pressure for an included child.
    bool enable_pressure_guided_full_tree_reclaim = true; // Rank reclaim over the complete tree.
    double cycle_reclaim_time_budget_seconds = 120.0; // Per-cycle reclaim wall-clock cap.
    double reclaim_giveback_ratio = 0.20;         // Legacy fraction of pulse TNS gain reclaim may return.
    int reclaim_max_consecutive_rejects = 3000;   // Reclaim rejections allowed before ending one scan.
    int reclaim_max_trials_per_cycle = 0;         // Reclaim trials per cycle; <= 0 is unlimited.
    bool reclaim_allow_original_resize = true;    // Resize buffers from the input tree.
    bool reclaim_allow_newbuf_resize = true;      // Resize buffers inserted by the optimizer.
    bool reclaim_allow_newbuf_delete = true;      // Delete buffers inserted by the optimizer.
    bool reclaim_allow_area_increasing_moves = false; // Permit reclaim moves without strict area reduction.
    double reclaim_rank_area_weight = 100.0;      // Area-saving weight used only for candidate ordering.
    double reclaim_rank_timing_help_weight = 1.0; // Reward for helpful pressure-delay effects.
    double reclaim_rank_timing_harm_weight = 5.0; // Penalty for harmful pressure-delay effects.
    double reclaim_rank_delete_bonus = 25.0;      // Ordering bonus for deleting a NEW_BUF.
    double reclaim_allowed_wns_worsen = 0.001;    // Legacy reclaim WNS loss limit.
    double reclaim_allowed_violation_growth_ratio = 0.05; // Legacy violation-count growth limit.
    double area_comparison_epsilon = 1e-9;        // Numerical tolerance for area comparisons.
    bool enable_runtime_profiling = true;         // Collect stage and hot-loop runtime counters.
    bool enable_adaptive_repair_reclaim_params = true; // Select pulse parameters from case size.
    double adaptive_reclaim_giveback_ratio = 0.20; // Reclaim ratio selected by the adaptive policy.
    double small_case_phase1a_pulse_delay_scale = 0.5;  // Small-case repair delay scale.
    double medium_case_phase1a_pulse_delay_scale = 1.0; // Medium-case repair delay scale.
    double large_case_phase1a_pulse_delay_scale = 1.5;  // Large-case repair delay scale.
    int medium_case_min_ss_violations = 15000;     // SS violations classifying a case as medium.
    int large_case_min_ss_violations = 30000;      // SS violations classifying a case as large.
    int medium_case_min_pressure_candidates = 10000; // Pressure candidates classifying medium.
    int large_case_min_pressure_candidates = 20000;  // Pressure candidates classifying large.
    bool adaptive_use_phase0_batch_signal = true; // Treat Phase 0 batching as a large-case signal.
    bool enable_repair_reclaim_no_progress_stop = true; // Stop repeated motionless cycles.
    int repair_reclaim_no_progress_streak_limit = 100;  // Consecutive no-progress cycles allowed.
    double repair_reclaim_min_timing_cost_improvement = 1e-6; // Timing gain that resets the streak.
    double repair_reclaim_min_area_improvement = 1e-6; // Area gain that resets the streak.
    int repair_reclaim_no_progress_max_insertions = 2; // Maximum insertions for a cycle to count idle.

    // Strategy 8 deadline-aware portfolio. The fallback always runs first;
    // individual route families remain independently switchable for A/B tests.
    bool enable_post_phase0_full_pipeline_fork_experiment = true; // Fork the remaining pipeline from the shared Phase0 checkpoint; E15 evaluates this as a deadline-aware target-score portfolio.
    bool portfolio_experiment_disable_time_limits = false; // Experimental option to remove branch wall-clock limits while retaining iteration stops.
    bool portfolio_disable_on_large_case = false; // Keep false: scheduling is based on remaining time, not a fixed testcase-size exclusion.
    bool portfolio_enforce_shared_global_deadline = true; // Give every route only the outer optimizer's remaining wall-clock budget.
    bool portfolio_include_alternating_branch = false; // The old H/Score+delete branch had negligible gain for high runtime, so omit it from the production experiment.
    int portfolio_max_routes = 0; // Total route cap including full_score; <= 0 keeps launching restarts until the safe deadline.
    double portfolio_min_route_seconds = 2.0; // Smallest useful allocation for a new route after its case-specific finalization reserve.
    bool portfolio_fill_remaining_time_with_restarts = true; // After the fixed diverse routes, rotate deterministic restart profiles while safe time remains.
    int portfolio_max_restart_routes = 0; // Additional deadline-filling restart cap; <= 0 means deadline-controlled only.
    double portfolio_restart_min_remaining_seconds = 5.0; // Do not start another restart below this post-reserve allocation.
    size_t portfolio_medium_case_min_paths = 50000; // Path count requiring the medium finalization safety window.
    size_t portfolio_large_case_min_paths = 250000; // Path count requiring the large finalization safety window.
    double portfolio_small_finalization_reserve_seconds = 10.0; // Output/validation reserve for small routes.
    double portfolio_medium_finalization_reserve_seconds = 30.0; // Larger reserve for medium timing vectors and final validation.
    double portfolio_large_finalization_reserve_seconds = 60.0; // Largest reserve for 250k+ path cases.
    bool portfolio_include_midweight_branch = false; // Optional alternative-weight trajectory; disabled after both E16 and E17 lost target score.
    double portfolio_midweight_alpha = 0.35; // SS weight for the E17 area-oriented search trajectory.
    double portfolio_midweight_beta = 0.25; // FF weight for the E17 area-oriented search trajectory.
    double portfolio_midweight_gamma = 0.40; // Area weight for the E17 area-oriented search trajectory.
    bool portfolio_include_delay_scale_branch = false; // Optional pulse-scale trajectory; disabled after both 0.75x and 1.25x screens lost target score.
    double portfolio_delay_scale_multiplier = 1.25; // Multiplier applied to all adaptive Repair Pulse delay scales in the E19 branch.
    bool portfolio_include_cosine_schedule_branch = true; // E34: retain the normal objective as fallback, then rerun Phase0 under the E31 cosine schedule on a separate heuristic route.
    bool portfolio_include_seeded_resize_branch = false; // E28 random resize lost on testcase3 and testcase0_v2; retain only as an optional baseline profile.
    bool portfolio_include_pressure_directed_resize_branch = false; // E29 did not beat the unperturbed route on testcase3; retain as optional profile.
    bool portfolio_include_area_shedding_resize_branch = false; // E30 profile is retained for later comparison, but aggressive percentage perturbations take priority.
    bool portfolio_include_aggressive_postphase_branches = true; // E32: explore 5%/10% all-type perturbations after the shared Phase0 checkpoint.
    bool portfolio_include_aggressive_prephase_branches = true; // E32: after the fallback route, also restart Phase0 from 5%/10% perturbed original-tree states.
    bool portfolio_allow_prephase_routes_on_large_case = false; // Avoid retaining a second complete original-tree clone for 250k+ paths; post-Phase0 routes remain deadline-controlled.
    double portfolio_aggressive_perturb_fraction_small = 0.05; // First aggressive route changes this fraction of all existing buffers.
    double portfolio_aggressive_perturb_fraction_large = 0.10; // Second aggressive route changes this fraction of all existing buffers.
    double portfolio_aggressive_perturb_fraction_extra = 0.20; // Strong deadline-filling restart changes this fraction of all existing buffers.
    unsigned int portfolio_aggressive_perturb_seed = 20260829U; // Base seed; every percentage/stage receives a deterministic offset.
    int portfolio_seeded_resize_moves = 48; // Number of legal original-buffer type changes used to diversify the seeded branch.
    int portfolio_seeded_resize_max_type_distance = 2; // Maximum delay-rank distance of each seeded resize, keeping the perturbation recoverable.
    unsigned int portfolio_seeded_resize_random_seed = 20260828U; // Fixed seed makes the diversification branch reproducible.
    double portfolio_seeded_resize_min_remaining_seconds = 60.0; // Minimum post-reserve time for the initial fixed aggressive routes; shorter restarts use the separate restart threshold.
    double portfolio_target_alpha = 0.64; // SS weight used to select the winning portfolio branch.
    double portfolio_target_beta = 0.18; // FF weight used to select the winning portfolio branch.
    double portfolio_target_gamma = 0.18; // Area weight used to select the winning portfolio branch.
    std::string portfolio_experiment_branch_label; // Non-empty label recorded by one recursively evaluated portfolio branch.
    bool enable_repair_reclaim_alternating_experiment = false; // Alternate heuristic and score-based Repair/Reclaim cycles in one branch.
    int repair_reclaim_alternating_delete_recent_limit = 2; // Recently inserted legal buffers deleted before each score-based cycle.

    // Final Alternating Greedy.
    bool enable_final_alternating_greedy = true;  // Run the final repair/reclaim alternation.
    bool final_alt_replace_phase1b_and_phase2 = true; // Replace legacy Phase 1B and Phase 2.
    int final_alt_max_iters = 10000;               // Maximum FinalAlt outer iterations.
    int final_alt_repair_insertions_per_iter = 500; // Repair insertion attempts per iteration.
    int final_alt_blacklist_cooldown_iters = 3;    // Outer iterations before a rejected repair target is retried; <= 0 keeps the persistent blacklist.
    int final_alt_max_failures_per_target = 3;     // Permanently blacklist a repair target after this many failures; <= 0 is unlimited.
    int final_alt_repair_types_per_target = 3;     // Buffer types trialed for each repair target.
    bool final_alt_include_delay_overshoot_types = true; // Preserve legacy under-target types, then fill unused near-closure trial slots with the closest delay overshoots.
    int final_alt_reclaim_types_per_node = 3;      // Smaller buffer types generated for each reclaim node.
    double final_alt_time_budget_seconds = 0.0;  // FinalAlt local budget; <= 0 uses only global time.
    double final_alt_safety_margin_seconds = 0.0; // Time reserved before the global deadline.
    double final_alt_repair_ss_tns_threshold = -0.0; // SS TNS threshold requesting repair.
    double final_alt_repair_ss_wns_threshold = -0.000; // SS WNS threshold requesting repair.
    int final_alt_repair_ss_violation_threshold = 0; // SS violation-count threshold.
    double final_alt_repair_ff_tns_threshold = -0.00; // FF TNS threshold requesting repair.
    int final_alt_repair_ff_violation_threshold = 0; // FF violation-count threshold.
    double final_alt_reclaim_giveback_ratio = 0.05; // Legacy fraction of repair gain reclaim may return.
    bool final_alt_reclaim_area_decrease_only = false; // Require strict area decrease for reclaim.
    bool final_alt_reclaim_hard_wns_guard = false; // Legacy per-corner WNS preservation guard.
    bool final_alt_run_reclaim_when_no_repair_needed = true; // Run a final zero-budget reclaim scan.
    bool enable_final_alt_ranked_reclaim_candidates = true; // Rank reclaim candidates before trials.
    bool enable_final_alt_target_checkpoint = true; // Restore the best estimated contest-score state seen before/after FinalAlt repair and reclaim.
    int final_alt_reclaim_top_k_candidates = 50000; // Highest-ranked candidates retained; <= 0 keeps all.
    int final_alt_large_case_reclaim_top_k_candidates = 50000; // Large-case total reclaim cap; the E10 50k policy outperformed smaller stratified caps.
    int final_alt_reclaim_top_k_per_kind = 0;      // Optional retained-candidate cap per move kind.
    int final_alt_large_case_reclaim_top_k_per_kind = 0; // Optional large-case per-kind diversity cap; disabled after E12 underperformed.
    bool final_alt_reclaim_preserve_old_order_when_disabled = true; // Keep preorder fallback behavior.
    double final_alt_rank_area_weight = 100.0;     // Area-saving weight used for FinalAlt ordering.
    double final_alt_rank_timing_help_weight = 1.0; // Reward for helpful pressure-delay effects.
    double final_alt_rank_timing_harm_weight = 5.0; // Penalty for harmful pressure-delay effects.
    double final_alt_rank_delete_bonus = 25.0;     // Ordering bonus for deleting a NEW_BUF.
    double final_alt_progress_epsilon = 1e-9;      // Minimum timing or area progress per iteration.

    // Strategy 8: deterministic perturb-and-recover after FinalAlt.
    bool enable_perturb_recover = false;          // Disabled by default: E23 found no accepted recovery on the area-oriented pipeline while spending substantial runtime.
    bool enable_perturb_recover_score_based_repair_reclaim = true; // Use weighted-score acceptance only inside Strategy 8 recovery.
    bool enable_perturb_recover_target_checkpoint = true; // Select the final Strategy 8 checkpoint with separate contest-estimate weights.
    bool enable_perturb_recover_target_outer_acceptance = true; // Accept recovered outer states by the target checkpoint score while keeping recovery internals on pipeline score.
    double perturb_recover_checkpoint_alpha = 0.64; // SS weight used only to choose the state retained after Strategy 8.
    double perturb_recover_checkpoint_beta = 0.18;  // FF weight used only to choose the state retained after Strategy 8.
    double perturb_recover_checkpoint_gamma = 0.18; // Area weight used only to choose the state retained after Strategy 8.
    int perturb_recover_max_cycles_without_target_improvement = 6; // Stop Strategy 8 after this many cycles fail to improve its retained target checkpoint; <= 0 disables.
    double perturb_recover_time_budget_seconds = 150.0; // Local wall-clock budget for the aggressive Strategy 8 search.
    double perturb_recover_validation_reserve_seconds = 10.0; // Global time reserved for final validation/output.
    size_t perturb_recover_large_case_min_paths = 250000; // Path-count threshold for the larger finalization reserve.
    bool perturb_recover_disable_on_large_case = true; // Give expensive large cases' Strategy 8 reserve back to FinalAlt after large-case recovery proved unproductive.
    double perturb_recover_large_case_validation_reserve_seconds = 60.0; // Large-case final validation/output reserve; E13 showed that reducing this to 35 s only added a non-improving FinalAlt iteration.
    double perturb_recover_initial_recovery_estimate_seconds = 40.0; // Conservative first estimate for one deep recovery iteration.
    double perturb_recover_recovery_estimate_safety_multiplier = 1.25; // Extra headroom required before starting expensive recovery work.
    int perturb_recover_max_cycles = 20;          // Maximum outer perturb/recover cycles (paths in brutal mode, beam rounds otherwise).
    bool perturb_recover_use_brutal_area_path = true; // Strip deletable buffers from one maximum-area leaf-to-root path; leave all others unchanged.
    int perturb_recover_brutal_recovery_max_cycles = 200; // Full reclaim-first Repair/Reclaim cycles after one brutal path mutation.
    int perturb_recover_candidates_per_cycle = 4; // Profiled-beam candidates per round; brutal mode always uses one path.
    bool perturb_recover_enable_candidate_profiles = true; // Split beam slots into timing, area, balanced, and random search profiles.
    double perturb_recover_guided_target_ratio = 0.70; // Probability of drawing from timing/delete guided target pools.
    int perturb_recover_min_moves = 4;            // Initial perturbation moves per cycle.
    int perturb_recover_max_moves = 12;           // Maximum perturbation moves after repeated failures.
    int perturb_recover_intensity_step_streak = 2; // Rejections required before adding one perturb move.
    int perturb_recover_move_attempt_multiplier = 30; // Mutation attempts allowed per requested perturb move.
    int perturb_recover_quick_recovery_iters = 1; // Cheap recovery iterations used to screen each beam candidate.
    int perturb_recover_quick_repair_attempts = 50; // Repair attempts in one quick-recovery iteration.
    int perturb_recover_quick_reclaim_candidates = 1000; // Reclaim trials in one quick-recovery iteration.
    bool perturb_recover_use_cycle_deep_recovery = true; // Recover the selected beam candidate with reclaim-first Repair/Reclaim cycles.
    int perturb_recover_bootstrap_reclaim_candidates = 1000; // Zero-TNS-giveback reclaim trials before the first repair pulse.
    bool perturb_recover_bootstrap_protect_inserted = true; // Prevent bootstrap reclaim from immediately deleting this perturbation's inserted buffers.
    int perturb_recover_recovery_iters = 3;        // Deep-recovery iterations for the best screened candidate.
    int perturb_recover_repair_attempts = 250;     // Maximum Repair Pulse targets per cycle, or FinalAlt attempts in legacy deep recovery.
    int perturb_recover_reclaim_candidates = 10000; // Normal reclaim candidates tried after each repair pulse.
    unsigned int perturb_recover_random_seed = 7;  // Fixed seed for reproducible perturbations.
    double perturb_recover_violation_growth_ratio = 0.05; // Maximum NVP growth after recovery.
    bool perturb_recover_require_timing_not_worse = false; // Optional per-corner TNS/WNS guard; score-only mode leaves it off.
    bool perturb_recover_require_violation_guard = false; // Optional NVP-growth guard; score-only mode leaves it off.

    // Verification and optional acceleration experiments.
    bool enable_incremental_area_verify = false;  // Recompute full area after accepted local moves.
    bool enable_phase0_trial_full_validation_verify = false; // Validate every Phase 0 commit.
    bool enable_final_alt_trial_full_validation_verify = false; // Validate every FinalAlt commit.
    bool enable_timing_cache_verify = false;       // Compare cached and full timing after commits.
    bool enable_type_id_cache = false;             // Use integer IDs for library lookups.
    bool enable_indexed_timing_paths = false;      // Enable indexed timing-analysis experiments.
    bool enable_phase0_endpoint_delta_pressure = false; // Optional normalized indexed endpoint-delta pressure; E20 was equivalent but slower on the 530k case.
    bool enable_indexed_timing_verify = false;     // Compare indexed timing against normal timing.
    bool enable_phase0_pressure_verify = false;    // Optional normalized indexed-vs-legacy pressure verification; E20 buffer-candidate max diff was zero.
    bool enable_type_id_cache_verify = false;      // Verify type-ID mappings against the library.
    bool enable_reclaim_candidate_top_k = false;   // Apply an optional Repair/Reclaim top-K cap.
    int reclaim_candidate_top_k = 0;               // Repair/Reclaim top-K value when enabled.
    bool enable_fast_timing_engine = true;         // Use transactional resize/insert/delete timing.
    bool enable_fast_timing_verify = false;        // Compare fast timing with the full model.
    int fast_timing_verify_interval = 0;           // Verify every N commits; <= 0 means every commit.

    // Legacy fallback, used only when FinalAlt replacement is disabled.
    int legacy_phase1a_max_iterations = 100;       // Maximum coarse Phase 1A iterations.
    double legacy_phase1a_improvement_ratio = 0.001; // Relative TNS gain needed by legacy Phase 1A.
    int legacy_phase1b_max_attempts = 20000;       // Maximum oracle-driven Phase 1B attempts.
    double legacy_phase2_gap_threshold = 2.5;      // Clock-gap threshold for Phase 2 patterns.
};

struct BaselineSnapshot {
    TimingAnalysisResult timing;
    double area = 0.0;
    bool valid = false;
};

struct StageSnapshot {
    bool valid = false;
    std::string name;
    int added_buffers = 0;
    int removed_buffers = 0;
    int downsized_buffers = 0;
    double runtime_seconds = 0.0;
    TimingAnalysisResult timing;
    double area = 0.0;
    LegalityReport legality;
};

struct Phase0MoveRecord {
    std::string node_name;
    std::string old_type;
    std::string new_type;
    double pressure = 0.0;
    double old_timing_cost = 0.0;
    double new_timing_cost = 0.0;
    double old_area = 0.0;
    double new_area = 0.0;
    double area_delta = 0.0;
};

struct Phase0Summary {
    bool enabled = false;
    bool reset_based = false;
    std::string branch_name;
    std::string fastest_buffer_type;
    StageSnapshot before_phase0;
    StageSnapshot after_reset;
    StageSnapshot after_phase0;
    int reset_resizes = 0;
    int attempted_resizes = 0;
    int accepted_resizes = 0;
    int rejected_resizes = 0;
    int positive_pressure_candidates = 0;
    int negative_pressure_candidates = 0;
    int zero_pressure_candidates = 0;
    int max_trial_nodes = 0;
    int max_types_per_node = 0;
    int max_passes = 0;
    int max_consecutive_rejects = 0;
    int max_consecutive_failed_batches = 0;
    std::string node_ranking_method = "weighted_score_estimated_value";
    size_t ranked_candidates_available = 0;
    int candidates_scanned = 0;
    bool unlimited_by_count = false;
    bool early_stop_triggered = false;
    std::string early_stop_reason = "unknown";
    double node_fraction = 0.0;
    double wns_weight = 1.0;
    double area_tiebreak_penalty = 0.0;
    double time_budget_seconds = 0.0;
    double original_timing_cost = 0.0;
    double final_phase0_timing_cost = 0.0;
    double area_before_phase0 = 0.0;
    double area_after_phase0 = 0.0;
    double area_ratio_after_phase0 = 1.0;
    double runtime_seconds = 0.0;
    bool incremental_timing_enabled = false;
    bool incremental_verify_enabled = false;
    int incremental_verify_failures = 0;
    double total_trial_time_seconds = 0.0;
    double average_trial_time_seconds = 0.0;
    bool batch_mode_enabled = false;
    bool batch_manual_enabled = false;
    bool batch_auto_enabled = false;
    std::string batch_auto_reason = "disabled";
    int batch_size = 0;
    bool fast_batch_timing_enabled = false;
    bool fast_batch_timing_used = false;
    bool batch_split_on_fail = false;
    int batch_max_split_depth = 0;
    int batch_auto_min_ss_violations = 0;
    int batch_auto_min_pressure_candidates = 0;
    int batch_attempts = 0;
    int batch_accepted = 0;
    int batch_rejected = 0;
    int batch_split_count = 0;
    int batch_split_depth_limit_hits = 0;
    int batch_accepted_candidates = 0;
    int fallback_individual_accepted = 0;
    std::vector<std::string> applied_moves;
    std::vector<Phase0MoveRecord> move_records;
    long long full_legality_validations_phase0_trials = 0;
    long long local_legality_checks_phase0 = 0;
    long long incremental_area_updates = 0;
    long long full_area_recomputations = 0;
    long long library_lookup_cache_hits = 0;
    long long library_lookup_cache_misses = 0;
    double fast_engine_build_time_seconds = 0.0;
    double fast_engine_sync_time_seconds = 0.0;
    double fast_trial_time_seconds = 0.0;
    double fast_group_collection_time_seconds = 0.0;
    double fast_group_update_time_seconds = 0.0;
    long long fast_sync_count = 0;
    long long fast_trial_count = 0;
    long long fast_resize_trials = 0;
    long long fast_resize_batch_trials = 0;
    long long fast_resize_batch_candidates = 0;
    long long fast_insert_trials = 0;
    long long fast_delete_trials = 0;
    long long fast_commit_count = 0;
    long long fast_rollback_count = 0;
    long long fast_verify_count = 0;
    long long fast_fallback_count = 0;
    long long fast_arrival_snapshot_count = 0;
    long long fast_affected_group_count = 0;
    long long fast_max_arrival_snapshots_per_trial = 0;
    long long fast_max_affected_groups_per_trial = 0;
    std::string pressure_method = "old_lca";
    double pressure_time_seconds = 0.0;
    double pressure_verify_max_abs_diff = 0.0;
};

struct RepairReclaimCycleRecord {
    int cycle_index = 0;
    std::string branch_mode = "heuristic";
    double objective_alpha = 0.0;
    double objective_beta = 0.0;
    double objective_gamma = 0.0;
    int pre_cycle_delete_attempts = 0;
    int pre_cycle_deletes = 0;
    double pre_cycle_deleted_area = 0.0;
    double pre_cycle_score_before = 0.0;
    double pre_cycle_score_after = 0.0;
    double start_time_seconds = 0.0;
    double end_time_seconds = 0.0;
    TimingAnalysisResult before_pulse_timing;
    double before_pulse_area = 0.0;
    TimingAnalysisResult after_pulse_timing;
    double after_pulse_area = 0.0;
    int pulse_inserted_buffers = 0;
    int pulse_shared_candidates_tried = 0;
    int pulse_shared_insertions = 0;
    bool shared_path_repair_active = false;
    int shared_no_insertion_streak = 0;
    double phase1a_pulse_delay_scale = 1.0;
    double pulse_ss_tns_gain = 0.0;
    double pulse_ff_tns_gain = 0.0;
    double pulse_area_increase = 0.0;
    int pulse_batch_attempts = 0;
    int pulse_rejected_batches = 0;
    int pulse_batch_splits = 0;
    int pulse_split_depth_limit_hits = 0;
    int pulse_max_split_depth_reached = 0;
    bool pulse_recovery_attempt = false;
    int pulse_failure_streak = 0;
    int pulse_recovery_blacklist_size = 0;
    std::string pulse_stop_reason;
    double net_cycle_area_delta = 0.0;
    double reclaim_coverage = 0.0;
    double reclaim_runtime_seconds = 0.0;
    int reclaim_candidates_built = 0;
    int reclaim_candidates_tried = 0;
    int reclaim_candidates_accepted = 0;
    int reclaim_candidates_rejected = 0;
    double reclaim_accept_rate = 0.0;
    double reclaim_trials_per_second = 0.0;
    double reclaim_area_saved_per_second = 0.0;
    double reclaim_area_saved_per_accepted_move = 0.0;
    bool reclaim_consecutive_reject_stop = false;
    int original_resizes_accepted = 0;
    int newbuf_resizes_accepted = 0;
    int newbuf_deletes_accepted = 0;
    double reclaim_area_saved = 0.0;
    double original_resize_area_saved = 0.0;
    double newbuf_resize_area_saved = 0.0;
    double newbuf_delete_area_saved = 0.0;
    double allowed_ss_tns_giveback = 0.0;
    double allowed_ff_tns_giveback = 0.0;
    double ss_tns_giveback_used = 0.0;
    double ff_tns_giveback_used = 0.0;
    double ss_giveback_utilization = 0.0;
    double ff_giveback_utilization = 0.0;
    double ss_wns_worsen = 0.0;
    double ff_wns_worsen = 0.0;
    TimingAnalysisResult after_reclaim_timing;
    double after_reclaim_area = 0.0;
};

struct RepairReclaimSummary {
    bool enabled = false;
    bool reclaim_enabled = false;
    double cycle_end_time_seconds = 0.0;
    int max_cycles = 0;
    double phase1a_pulse_delay_scale = 1.0;
    double reclaim_giveback_ratio = 0.0;
    double cycle_reclaim_time_budget_seconds = 0.0;
    int total_cycles = 0;
    int total_pulse_inserted_buffers = 0;
    int total_shared_candidates_tried = 0;
    int total_shared_insertions = 0;
    bool shared_path_repair_enabled = false;
    int shared_max_consecutive_no_insertion_pulses = 0;
    bool shared_path_repair_disabled_by_streak = false;
    int shared_path_repair_disabled_after_cycle = -1;
    int final_shared_no_insertion_streak = 0;
    bool pulse_split_on_fail = false;
    int pulse_max_split_depth = 0;
    int total_pulse_batch_attempts = 0;
    int total_pulse_rejected_batches = 0;
    int total_pulse_batch_splits = 0;
    int total_pulse_split_depth_limit_hits = 0;
    int max_pulse_split_depth_reached = 0;
    bool pulse_failure_recovery_enabled = false;
    int pulse_max_consecutive_failures = 0;
    double pulse_recovery_scale_multiplier = 1.0;
    double pulse_recovery_min_delay_scale = 0.0;
    int pulse_recovery_blacklist_targets = 0;
    int total_pulse_recovery_retries = 0;
    int successful_pulse_recoveries = 0;
    int max_consecutive_pulse_failures = 0;
    int total_original_resizes = 0;
    int total_newbuf_resizes = 0;
    int total_newbuf_deletes = 0;
    int total_pre_cycle_deletes = 0;
    double total_pre_cycle_deleted_area = 0.0;
    double total_pulse_area_increase = 0.0;
    double total_reclaim_area_saved = 0.0;
    double total_net_cycle_area_delta = 0.0;
    double average_reclaim_coverage = 0.0;
    double weighted_reclaim_coverage = 0.0;
    double total_reclaim_runtime_seconds = 0.0;
    int total_reclaim_candidates_built = 0;
    int total_reclaim_candidates_tried = 0;
    int total_reclaim_candidates_accepted = 0;
    double total_reclaim_accept_rate = 0.0;
    double total_original_resize_area_saved = 0.0;
    double total_newbuf_resize_area_saved = 0.0;
    double total_newbuf_delete_area_saved = 0.0;
    std::string stop_reason = "not_run";
    bool no_progress_stop_enabled = false;
    bool no_progress_stop_triggered = false;
    int no_progress_streak_limit = 0;
    int final_no_progress_streak = 0;
    std::vector<RepairReclaimCycleRecord> cycles;
};

struct AdaptiveRepairReclaimParams {
    bool enabled = false;
    bool missing_statistics = false;
    size_t baseline_ss_violations = 0;
    size_t phase0_pressure_candidates = 0;
    bool phase0_batch_used = false;
    std::string classified_case_size = "manual";
    double manual_reclaim_giveback_ratio = 0.0;
    double manual_phase1a_pulse_delay_scale = 1.0;
    double selected_reclaim_giveback_ratio = 0.0;
    double selected_phase1a_pulse_delay_scale = 1.0;
};

struct RuntimeProfileSummary {
    bool enabled = false;
    double phase0_seconds = 0.0;
    double repair_reclaim_seconds = 0.0;
    double phase1a_pulse_seconds = 0.0;
    double pressure_seconds = 0.0;
    double reclaim_candidate_generation_seconds = 0.0;
    double reclaim_sorting_seconds = 0.0;
    double reclaim_trial_loop_seconds = 0.0;
    double phase1b_seconds = 0.0;
    double phase2_seconds = 0.0;
    double final_validation_seconds = 0.0;
    double report_generation_seconds = 0.0;
};

struct FinalAlternatingGreedySummary {
    bool enabled = false;
    bool replace_phase1b_and_phase2 = false;
    int max_iters = 0;
    int repair_insertions_per_iter = 0;
    double reclaim_giveback_ratio = 0.0;
    bool hard_wns_guard = false;
    bool area_decrease_only = false;
    bool target_checkpoint_enabled = false;
    bool target_checkpoint_restored = false;
    int target_checkpoint_best_iteration = -1;
    double target_score_before = 0.0;
    double target_score_after = 0.0;
    double runtime_seconds = 0.0;
    int iterations = 0;
    int repair_insertions = 0;
    int reclaim_moves_accepted = 0;
    int newbuf_deletes = 0;
    int resizes = 0;
    double area_before = 0.0;
    double area_after = 0.0;
    double area_saved = 0.0;
    double ss_tns_before = 0.0;
    double ss_tns_after = 0.0;
    double ss_wns_before = 0.0;
    double ss_wns_after = 0.0;
    size_t ss_violations_before = 0;
    size_t ss_violations_after = 0;
    double ff_tns_before = 0.0;
    double ff_tns_after = 0.0;
    size_t ff_violations_before = 0;
    size_t ff_violations_after = 0;
    std::string stop_reason = "disabled";
};

struct FinalAltIterationRecord {
    int iteration = 0;
    double start_time_seconds = 0.0;
    double end_time_seconds = 0.0;
    bool repair_needed = false;
    int repair_attempts = 0;
    int repair_insertions = 0;
    int repair_rejections = 0;
    int reclaim_candidates_tried = 0;
    int reclaim_moves_accepted = 0;
    int reclaim_moves_rejected = 0;
    int reclaim_resizes = 0;
    int reclaim_deletes = 0;
    double area_before = 0.0;
    double area_after_repair = 0.0;
    double area_after = 0.0;
    TimingAnalysisResult before_timing;
    TimingAnalysisResult after_repair_timing;
    TimingAnalysisResult after_timing;
    std::string stop_reason;
};

struct PerturbRecoverCycleRecord {
    int cycle_index = 0;
    int candidates_generated = 0;
    int candidates_quick_recovered = 0;
    int selected_candidate = -1;
    std::string selected_profile;
    std::string brutal_path_leaf;
    int brutal_path_nodes = 0;
    double brutal_path_area = 0.0;
    int requested_moves = 0;
    int perturb_attempts = 0;
    int guided_moves = 0;
    int random_moves = 0;
    int perturb_resizes = 0;
    int perturb_inserts = 0;
    int perturb_deletes = 0;
    int quick_recovery_iterations = 0;
    int deep_recovery_iterations = 0;
    int bootstrap_reclaim_tried = 0;
    int bootstrap_reclaim_accepted = 0;
    int cycle_pulse_batch_attempts = 0;
    int cycle_pulse_insertions = 0;
    int cycle_reclaim_tried = 0;
    int cycle_reclaim_accepted = 0;
    int recovery_iterations = 0;
    int recovery_insertions = 0;
    int recovery_reclaim_moves = 0;
    double score_before = 0.0;
    double score_after = 0.0;
    double area_before = 0.0;
    double area_after = 0.0;
    TimingAnalysisResult timing_before;
    TimingAnalysisResult timing_after;
    bool legal = false;
    bool timing_guard_passed = false;
    bool violation_guard_passed = false;
    bool accepted = false;
    std::string stop_reason;
};

struct PerturbRecoverSummary {
    bool enabled = false;
    bool recovery_score_based = false;
    bool target_checkpoint_enabled = false;
    bool target_outer_acceptance_enabled = false;
    bool target_checkpoint_restored = false;
    double target_checkpoint_alpha = 0.0;
    double target_checkpoint_beta = 0.0;
    double target_checkpoint_gamma = 0.0;
    double target_score_before = 0.0;
    double target_score_after = 0.0;
    int max_cycles_without_target_improvement = 0;
    unsigned int random_seed = 0;
    double time_budget_seconds = 0.0;
    double validation_reserve_seconds = 0.0;
    int max_cycles = 0;
    bool brutal_area_path_enabled = false;
    int brutal_recovery_max_cycles = 0;
    int candidates_per_cycle = 0;
    bool candidate_profiles_enabled = false;
    double guided_target_ratio = 0.0;
    int min_moves = 0;
    int max_moves = 0;
    int intensity_step_streak = 0;
    int move_attempt_multiplier = 0;
    int quick_recovery_iters = 0;
    int quick_repair_attempts = 0;
    int quick_reclaim_candidates = 0;
    bool cycle_deep_recovery_enabled = false;
    int bootstrap_reclaim_candidates = 0;
    bool bootstrap_protect_inserted = false;
    int recovery_iters = 0;
    int repair_attempts = 0;
    int reclaim_candidates = 0;
    double runtime_seconds = 0.0;
    int cycles = 0;
    int accepted_cycles = 0;
    int rejected_cycles = 0;
    int best_updates = 0;
    int perturb_attempts = 0;
    int perturb_resizes = 0;
    int perturb_inserts = 0;
    int perturb_deletes = 0;
    int recovery_iterations = 0;
    int recovery_insertions = 0;
    int recovery_reclaim_moves = 0;
    int bootstrap_reclaim_tried = 0;
    int bootstrap_reclaim_accepted = 0;
    int cycle_pulse_batch_attempts = 0;
    int cycle_pulse_insertions = 0;
    int cycle_reclaim_tried = 0;
    int cycle_reclaim_accepted = 0;
    int accepted_insertions = 0;
    int accepted_deletes = 0;
    int accepted_resizes = 0;
    double score_before = 0.0;
    double score_after = 0.0;
    double best_score = 0.0;
    double area_before = 0.0;
    double area_after = 0.0;
    TimingAnalysisResult timing_before;
    TimingAnalysisResult timing_after;
    std::string stop_reason = "disabled";
    std::vector<PerturbRecoverCycleRecord> cycle_records;
};

struct RuntimeValidationDiagnostics {
    bool incremental_area_enabled = true;
    bool incremental_area_verify = false;
    bool phase0_trial_full_validation_verify = false;
    bool final_alt_trial_full_validation_verify = false;
    long long full_legality_validations_total = 0;
    long long full_legality_validations_phase0_trials = 0;
    long long full_legality_validations_final_alt_trials = 0;
    long long local_legality_checks_phase0 = 0;
    long long local_legality_checks_final_alt = 0;
    long long incremental_area_updates = 0;
    long long full_area_recomputations = 0;
    long long library_lookup_cache_hits = 0;
    long long library_lookup_cache_misses = 0;
    long long repair_reclaim_no_progress_stops = 0;
};

struct SecondRoundRuntimeDiagnostics {
    bool type_id_cache_enabled = false;
    bool indexed_timing_enabled = false;
    bool phase0_endpoint_delta_pressure_enabled = false;
    bool final_alt_ranked_reclaim_enabled = false;
    long long library_cache_lookups = 0;
    long long library_cache_misses = 0;
    long long linear_library_scans_remaining = 0;
    long long tree_index_rebuilds = 0;
    double tree_index_rebuild_time_seconds = 0.0;
    double indexed_arrival_time_seconds = 0.0;
    double indexed_timing_analysis_time_seconds = 0.0;
    double old_timing_analysis_time_seconds = 0.0;
    std::string phase0_pressure_method = "old_lca";
    double phase0_pressure_time_seconds = 0.0;
    double phase0_pressure_verify_max_abs_diff = 0.0;
    int final_alt_candidates_built = 0;
    int final_alt_candidates_kept_after_topk = 0;
    int final_alt_candidates_tried = 0;
    int final_alt_candidates_accepted = 0;
    double final_alt_accept_rate = 0.0;
    int final_alt_topk_limit = 0;
    int final_alt_topk_per_kind_limit = 0;
    int reclaim_candidates_before_topk = 0;
    int reclaim_candidates_after_topk = 0;
};

struct FastTimingDiagnostics {
    bool enabled = false;
    bool verify_enabled = false;
    int verify_interval = 0;
    bool resize_trials_enabled = false;
    bool delete_trials_enabled = false;
    bool insert_trials_enabled = false;
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
};

struct BestCheckpointSummary {
    bool enabled = false;
    bool restored = false;
    int updates = 0;
    std::string best_stage = "initial";
    double initial_score = 0.0;
    double best_score = 0.0;
    double pre_restore_score = 0.0;
    double best_area = 0.0;
    TimingAnalysisResult best_timing;
};

struct PostPhase0PortfolioBranchRecord {
    std::string name;
    std::string policy;
    std::string initial_state_policy = "phase0_checkpoint";
    int initial_state_resizes = 0;
    unsigned int initial_state_seed = 0;
    double initial_state_perturb_fraction = 0.0;
    bool initial_state_reruns_phase0 = false;
    TimingAnalysisResult initial_state_timing;
    double initial_state_area = 0.0;
    bool repair_reclaim_score_based = false;
    bool alternating_repair_reclaim = false;
    bool objective_weight_scheduler = false;
    bool success = false;
    bool legal = false;
    double runtime_seconds = 0.0;
    TimingAnalysisResult final_timing;
    double final_area = 0.0;
    ScoreMetrics final_score;
    double target_score = 0.0;
    std::string repair_reclaim_stop_reason;
    std::string final_alt_stop_reason;
    std::string perturb_recover_stop_reason;
    RepairReclaimSummary repair_reclaim;
    FinalAlternatingGreedySummary final_alt;
    std::vector<FinalAltIterationRecord> final_alt_iterations;
    PerturbRecoverSummary perturb_recover;
};

struct PostPhase0PortfolioSummary {
    bool enabled = false;
    bool time_limits_disabled = false;
    double runtime_seconds = 0.0;
    TimingAnalysisResult phase0_timing;
    double phase0_area = 0.0;
    double phase0_score = 0.0;
    double target_alpha = 0.0;
    double target_beta = 0.0;
    double target_gamma = 0.0;
    std::string full_fork_winner;
    std::string overall_winner;
    std::vector<PostPhase0PortfolioBranchRecord> branches;
};

struct OptimizationSummary {
    bool success = false;
    bool early_stopped = false;
    bool phase0_enabled = false;
    bool phase0_reset_experiment_enabled = false;
    std::string message;
    std::string phase1a_stop_reason = "unknown";
    TimingAnalysisResult final_timing;
    ScoreMetrics final_score;
    LegalityReport final_legality;
    double final_area = 0.0;
    double runtime_seconds = 0.0;
    int iterations = 0;
    int phase1a_insertions = 0;
    int phase1b_insertions = 0;
    int phase2_removals = 0;
    int phase2_downsizes = 0;
    std::vector<StageSnapshot> phase1a_iteration_snapshots;
    std::vector<StageSnapshot> phase2_iteration_snapshots;
    StageSnapshot after_existing_buffer_shared_reclaim;
    bool existing_buffer_shared_reclaim_enabled = false;
    bool existing_buffer_shared_reclaim_skipped_for_large_case = false;
    bool existing_buffer_shared_reclaim_disabled_by_no_insertion = false;
    int existing_buffer_shared_reclaim_disabled_after_cycle = -1;
    int existing_buffer_shared_final_no_insertion_streak = 0;
    RepairReclaimCycleRecord existing_buffer_shared_reclaim_cycle;
    std::vector<RepairReclaimCycleRecord> existing_buffer_shared_reclaim_cycles;
    StageSnapshot after_phase1a;
    StageSnapshot after_phase1b;
    StageSnapshot after_phase2;
    StageSnapshot after_final_alt;
    StageSnapshot after_perturb_recover;
    Phase0Summary phase0;
    RepairReclaimSummary repair_reclaim;
    AdaptiveRepairReclaimParams adaptive_repair_reclaim;
    RuntimeProfileSummary runtime_profile;
    FinalAlternatingGreedySummary final_alt;
    std::vector<FinalAltIterationRecord> final_alt_iterations;
    PerturbRecoverSummary perturb_recover;
    RuntimeValidationDiagnostics diagnostics;
    SecondRoundRuntimeDiagnostics second_round;
    FastTimingDiagnostics fast_timing;
    BestCheckpointSummary checkpoint;
    PostPhase0PortfolioSummary post_phase0_portfolio;
    std::vector<std::string> applied_moves;
};

class Optimizer {
public:
    Optimizer(const OptimizerConfig& c) : cfg(c) {}
    Optimizer(const OptimizerConfig& c,
              const BaselineSnapshot& normalization_baseline)
        : cfg(c),
          baseline(normalization_baseline),
          external_baseline(true) {}
    OptimizationSummary optimize(ClockTree& tree,
                                 const std::vector<BufSpec>& libs,
                                 const std::vector<PathInfo>& ss_paths,
                                 const std::vector<PathInfo>& ff_paths,
                                 double clock_period,
                                 bool reset_phase0 = false,
                                 const std::string& phase0_branch_name = "normal");
    Phase0Summary run_phase0_timing_conditioning(ClockTree& tree,
                                                 const std::vector<BufSpec>& libs,
                                                 const std::vector<PathInfo>& ss_paths,
                                                 const std::vector<PathInfo>& ff_paths,
                                                 double clock_period,
                                                 bool reset_based,
                                                 const std::string& branch_name,
                                                 const std::chrono::steady_clock::time_point& global_start);
    void analyze(const ClockTree& tree,
                 const std::vector<BufSpec>& libs,
                 const std::vector<PathInfo>& ss_paths,
                 const std::vector<PathInfo>& ff_paths,
                 double clock_period,
                 const std::string& report_path,
                 const std::string& testcase_name,
                 const OptimizationSummary* optimization = nullptr);
    bool insert_buffer(ClockTree& tree, const std::string& parent_name,
                       const std::string& child_name, const std::string& buffer_type,
                       std::string& out_buffer_name);
private:
    OptimizerConfig cfg;
    BaselineSnapshot baseline;
    bool external_baseline = false;
};
