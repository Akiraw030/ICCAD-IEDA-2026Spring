#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DEFAULT_TESTCASES=(testcase0 testcase1 testcase2 testcase3 testcase4 testcase0_v2 testcase1_v2)
if [[ -n "${CADD0045_TESTCASES:-}" ]]; then
    read -r -a TESTCASES <<< "${CADD0045_TESTCASES}"
else
    TESTCASES=("${DEFAULT_TESTCASES[@]}")
fi

TESTCASE_ROOT="${1:-}"
RUN_TAG="$(date +%Y%m%d_%H%M%S)"
OUT_ROOT="${2:-${ROOT_DIR}/report/run_${RUN_TAG}}"
TMP_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

configure_build() {
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_OUTPUT=ON
}

run_case() {
    local testcase="$1"
    local testcase_dir="${TESTCASE_ROOT}/${testcase}"
    local tmp_case_dir="${TMP_DIR}/${testcase}"
    local output_tree="${tmp_case_dir}/modified_clk_tree.structure"
    local auto_report="${tmp_case_dir}/timing_report_${testcase}.txt"
    local final_report="${OUT_ROOT}/timing_report_${testcase}.txt"

    mkdir -p "${tmp_case_dir}"
    "${BUILD_DIR}/cadd0045" "${testcase_dir}" "${output_tree}" >/dev/null

    if [[ ! -f "${auto_report}" ]]; then
        echo "Missing timing report for ${testcase}: ${auto_report}" >&2
        return 1
    fi

    mv "${auto_report}" "${final_report}"
}

main() {
    if (( $# < 1 || $# > 2 )); then
        echo "Usage: $0 <testcase_root> [output_directory]" >&2
        exit 2
    fi
    [[ -d "${TESTCASE_ROOT}" ]] || {
        echo "Missing testcase root: ${TESTCASE_ROOT}" >&2
        exit 2
    }

    mkdir -p "${OUT_ROOT}"

    echo "Configuring build with report output enabled..."
    configure_build

    echo "Building optimizer..."
    cmake --build "${BUILD_DIR}"

    echo "Saving timing reports to: ${OUT_ROOT}"
    for testcase in "${TESTCASES[@]}"; do
        [[ -d "${TESTCASE_ROOT}/${testcase}" ]] || {
            echo "Missing testcase directory: ${TESTCASE_ROOT}/${testcase}" >&2
            exit 2
        }
        echo "  - ${testcase}"
        run_case "${testcase}"
    done

    echo "Done."
}

main "$@"
