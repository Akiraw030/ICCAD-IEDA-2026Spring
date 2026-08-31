#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
TESTCASE_ROOT="${1:-}"
CHECKER_BIN="${2:-}"
OUT_ROOT="${3:-${ROOT_DIR}/report/final}"
REPORT_ROOT="${OUT_ROOT}/report_enabled"
RELEASE_ROOT="${OUT_ROOT}/release"

# These are the seven official public workloads used by run_reports.sh.
# testcase2_v2, testcase3_v2, and testcase4_v2 differ only by checker-friendly
# buf.lib text, so the checker normalization below avoids running them twice.
DEFAULT_TESTCASES=(
    testcase0
    testcase1
    testcase2
    testcase3
    testcase4
    testcase0_v2
    testcase1_v2
)

if [[ -n "${CADD0045_TESTCASES:-}" ]]; then
    read -r -a TESTCASES <<< "${CADD0045_TESTCASES}"
else
    TESTCASES=("${DEFAULT_TESTCASES[@]}")
fi

SUMMARY_FILE="${OUT_ROOT}/summary.tsv"
FAILURE_COUNT=0

die() {
    echo "ERROR: $*" >&2
    exit 1
}

usage() {
    cat >&2 <<EOF
Usage: $0 <testcase_root> <checker_binary> [output_directory]

  testcase_root   Directory containing testcase0, testcase1, ..., testcase1_v2
  checker_binary  Path to the official checker executable
  output_directory  Defaults to ./report/final

Use CADD0045_TESTCASES to select a whitespace-separated subset.
EOF
    exit 2
}

require_inputs() {
    [[ -x "${CHECKER_BIN}" ]] ||
        die "Official checker is missing or not executable: ${CHECKER_BIN}"

    local testcase testcase_dir required_file
    for testcase in "${TESTCASES[@]}"; do
        testcase_dir="${TESTCASE_ROOT}/${testcase}"
        [[ -d "${testcase_dir}" ]] ||
            die "Missing testcase directory: ${testcase_dir}"
        for required_file in \
            clk_tree.structure buf.lib SS_delay.rpt FF_delay.rpt; do
            [[ -f "${testcase_dir}/${required_file}" ]] ||
                die "Missing ${testcase_dir}/${required_file}"
        done
    done
}

prepare_output_directory() {
    if [[ -e "${OUT_ROOT}" ]] &&
       [[ -n "$(find "${OUT_ROOT}" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]]; then
        die "${OUT_ROOT} is not empty. Move or remove the previous final run first."
    fi
    mkdir -p "${REPORT_ROOT}" "${RELEASE_ROOT}"
}

configure_and_build() {
    local output_mode="$1"
    local log_prefix="$2"

    echo "Configuring Release/O3 with ENABLE_OUTPUT=${output_mode}..."
    if ! cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_OUTPUT="${output_mode}" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" \
        -DCMAKE_EXE_LINKER_FLAGS_RELEASE="" \
        >"${OUT_ROOT}/${log_prefix}_configure.log" 2>&1; then
        die "CMake configure failed; see ${OUT_ROOT}/${log_prefix}_configure.log"
    fi

    echo "Building optimizer..."
    if ! cmake --build "${BUILD_DIR}" --parallel \
        >"${OUT_ROOT}/${log_prefix}_build.log" 2>&1; then
        die "Build failed; see ${OUT_ROOT}/${log_prefix}_build.log"
    fi

    cp "${BUILD_DIR}/CMakeCache.txt" \
       "${OUT_ROOT}/${log_prefix}_CMakeCache.txt"
    cp "${BUILD_DIR}/CMakeFiles/cadd0045.dir/flags.make" \
       "${OUT_ROOT}/${log_prefix}_flags.make"
    sha256sum "${BUILD_DIR}/cadd0045" \
        >"${OUT_ROOT}/${log_prefix}_binary.sha256"
}

run_optimizer() {
    local testcase="$1"
    local testcase_dir="$2"
    local output_tree="$3"
    local stdout_log="$4"
    local stderr_log="$5"
    local runtime_file="$6"

    /usr/bin/time \
        -f 'wall_seconds=%e\nuser_seconds=%U\nsystem_seconds=%S\nmax_rss_kb=%M\nexit_status=%x' \
        -o "${runtime_file}" \
        "${BUILD_DIR}/cadd0045" \
        "${testcase_dir}" \
        "${output_tree}" \
        >"${stdout_log}" 2>"${stderr_log}"
}

run_report_pass() {
    local testcase testcase_dir case_dir output_tree auto_report status

    echo "Running report-enabled pass..."
    for testcase in "${TESTCASES[@]}"; do
        echo "  [report] ${testcase}"
        testcase_dir="${TESTCASE_ROOT}/${testcase}"
        case_dir="${REPORT_ROOT}/${testcase}"
        output_tree="${case_dir}/modified_clk_tree.structure"
        auto_report="${case_dir}/timing_report_${testcase}.txt"
        mkdir -p "${case_dir}"

        run_optimizer \
            "${testcase}" \
            "${testcase_dir}" \
            "${output_tree}" \
            "${case_dir}/stdout.log" \
            "${case_dir}/stderr.log" \
            "${case_dir}/runtime.txt"
        status=$?
        printf '%s\n' "${status}" >"${case_dir}/optimizer_exit_status.txt"

        if (( status != 0 )); then
            echo "    FAILED: optimizer exit ${status}" >&2
            ((FAILURE_COUNT += 1))
        elif [[ ! -s "${auto_report}" ]]; then
            echo "    FAILED: missing timing report ${auto_report}" >&2
            ((FAILURE_COUNT += 1))
        elif [[ ! -s "${output_tree}" ]]; then
            echo "    FAILED: missing output tree ${output_tree}" >&2
            ((FAILURE_COUNT += 1))
        fi
    done
}

make_checker_library() {
    local source_lib="$1"
    local checker_lib="$2"

    # The official checker rejects `cell(NAME)` but accepts `cell (NAME)`.
    sed -E 's/^([[:space:]]*)cell\(/\1cell (/' \
        "${source_lib}" >"${checker_lib}"
}

run_release_and_checker_pass() {
    local testcase testcase_dir case_dir output_tree optimizer_status
    local checker_status checker_lib

    echo "Running quiet release pass and checker..."
    for testcase in "${TESTCASES[@]}"; do
        echo "  [release] ${testcase}"
        testcase_dir="${TESTCASE_ROOT}/${testcase}"
        case_dir="${RELEASE_ROOT}/${testcase}"
        output_tree="${case_dir}/modified_clk_tree.structure"
        checker_lib="${case_dir}/checker_buf.lib"
        mkdir -p "${case_dir}"

        run_optimizer \
            "${testcase}" \
            "${testcase_dir}" \
            "${output_tree}" \
            "${case_dir}/stdout.log" \
            "${case_dir}/stderr.log" \
            "${case_dir}/runtime.txt"
        optimizer_status=$?
        printf '%s\n' "${optimizer_status}" \
            >"${case_dir}/optimizer_exit_status.txt"

        checker_status=125
        if (( optimizer_status == 0 )) && [[ -s "${output_tree}" ]]; then
            make_checker_library "${testcase_dir}/buf.lib" "${checker_lib}"
            "${CHECKER_BIN}" \
                "${testcase_dir}/clk_tree.structure" \
                "${output_tree}" \
                "${checker_lib}" \
                "${testcase_dir}/FF_delay.rpt" \
                "${testcase_dir}/SS_delay.rpt" \
                >"${case_dir}/checker_result.txt" 2>&1
            checker_status=$?
        else
            printf '%s\n' \
                "Checker skipped because optimizer failed or produced no tree." \
                >"${case_dir}/checker_result.txt"
        fi
        printf '%s\n' "${checker_status}" \
            >"${case_dir}/checker_exit_status.txt"

        if (( optimizer_status != 0 )); then
            echo "    FAILED: optimizer exit ${optimizer_status}" >&2
            ((FAILURE_COUNT += 1))
        elif (( checker_status != 0 )); then
            echo "    FAILED: checker exit ${checker_status}" >&2
            ((FAILURE_COUNT += 1))
        fi
    done
}

runtime_value() {
    local runtime_file="$1"
    if [[ -f "${runtime_file}" ]]; then
        awk -F= '$1 == "wall_seconds" { print $2; exit }' "${runtime_file}"
    fi
}

status_value() {
    local status_file="$1"
    if [[ -f "${status_file}" ]]; then
        head -n 1 "${status_file}"
    else
        printf '%s' 'missing'
    fi
}

checker_corner_metric() {
    local checker_file="$1"
    local corner="$2"
    local field="$3"
    if [[ ! -f "${checker_file}" ]]; then
        return
    fi
    awk -v wanted_corner="${corner}" -v wanted_field="${field}" '
        $0 ~ "^[[:space:]]*" wanted_corner "[[:space:]]*\\|" {
            split($0, part, "|")
            for (i = 2; i <= 4; ++i) {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", part[i])
            }
            wns = part[2]
            tns = part[3]
            nvp = part[4]
        }
        END {
            if (wanted_field == "wns") print wns
            else if (wanted_field == "tns") print tns
            else if (wanted_field == "nvp") print nvp
        }
    ' "${checker_file}"
}

checker_area_metric() {
    local checker_file="$1"
    if [[ ! -f "${checker_file}" ]]; then
        return
    fi
    awk '
        /newTotalBufArea/ {
            split($0, equal_part, "=")
            gsub(/^[[:space:]]+/, "", equal_part[2])
            split(equal_part[2], value_part, /[[:space:]]+/)
            area = value_part[1]
        }
        END { print area }
    ' "${checker_file}"
}

write_summary() {
    local testcase checker_result
    printf 'testcase\treport_exit\treport_wall_s\trelease_exit\trelease_wall_s\tchecker_exit\tss_wns\tss_tns\tss_nvp\tff_wns\tff_tns\tff_nvp\tarea\n' \
        >"${SUMMARY_FILE}"
    for testcase in "${TESTCASES[@]}"; do
        checker_result="${RELEASE_ROOT}/${testcase}/checker_result.txt"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "${testcase}" \
            "$(status_value "${REPORT_ROOT}/${testcase}/optimizer_exit_status.txt")" \
            "$(runtime_value "${REPORT_ROOT}/${testcase}/runtime.txt")" \
            "$(status_value "${RELEASE_ROOT}/${testcase}/optimizer_exit_status.txt")" \
            "$(runtime_value "${RELEASE_ROOT}/${testcase}/runtime.txt")" \
            "$(status_value "${RELEASE_ROOT}/${testcase}/checker_exit_status.txt")" \
            "$(checker_corner_metric "${checker_result}" SS wns)" \
            "$(checker_corner_metric "${checker_result}" SS tns)" \
            "$(checker_corner_metric "${checker_result}" SS nvp)" \
            "$(checker_corner_metric "${checker_result}" FF wns)" \
            "$(checker_corner_metric "${checker_result}" FF tns)" \
            "$(checker_corner_metric "${checker_result}" FF nvp)" \
            "$(checker_area_metric "${checker_result}")" \
            >>"${SUMMARY_FILE}"
    done
}

write_manifest() {
    {
        printf '%s\n\n' '# Final full-suite run'
        printf '%s\n\n' "Generated: $(date --iso-8601=seconds)"
        printf '%s\n\n' '## Testcases'
        printf -- '- `%s`\n' "${TESTCASES[@]}"
        printf '\n%s\n\n' '## Layout'
        printf '%s\n' '- `report_enabled/<testcase>/`: O3 report-enabled tree, timing report, logs, and runtime.'
        printf '%s\n' '- `release/<testcase>/`: O3 quiet-release tree, checker result, logs, and runtime.'
        printf '%s\n' '- `summary.tsv`: exit status and wall time for both passes.'
        printf '%s\n' '- `*_flags.make`, `*_CMakeCache.txt`, `*_binary.sha256`: exact build records.'
        printf '\n%s\n' 'The report-enabled and quiet binaries use the same optimizer parameters; only ENABLE_OUTPUT differs.'
    } >"${OUT_ROOT}/README.md"
}

main() {
    (( $# >= 2 && $# <= 3 )) || usage
    require_inputs
    prepare_output_directory
    write_manifest

    configure_and_build ON report_enabled
    run_report_pass

    configure_and_build OFF release
    run_release_and_checker_pass

    write_summary

    echo
    echo "Final records: ${OUT_ROOT}"
    echo "Summary:       ${SUMMARY_FILE}"
    if (( FAILURE_COUNT != 0 )); then
        echo "Completed with ${FAILURE_COUNT} failed optimizer/checker step(s)." >&2
        exit 1
    fi
    echo "All optimizer and checker steps completed successfully."
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
