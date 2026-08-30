#!/usr/bin/env python3
"""Generate a deterministic >500k-FF stress testcase from an existing case.

Each copy receives a unique node/path prefix.  Copies are connected below a
balanced two-level buffer supertree whose fanout never exceeds five, matching
the number of delay-table entries in the supplied buffer library.
"""

from __future__ import annotations

import argparse
import re
import shutil
from pathlib import Path


NODE_RE = re.compile(r"^(\t*)\[(\d+)\]\s+(\S+)(.*)$")
PATH_RE = re.compile(
    r"^\s*Path\S*\s*:\s*(\S+)\s*->\s*(\S+)\s+([-+0-9.eE]+)\s*$"
)


def inspect_template(source: Path) -> tuple[int, int, int, int]:
    node_names: set[str] = set()
    sink_names: set[str] = set()
    buffer_count = 0
    max_top_fanout = 0

    with (source / "clk_tree.structure").open(encoding="utf-8") as stream:
        for line in stream:
            match = NODE_RE.match(line.rstrip("\r\n"))
            if not match:
                continue
            depth = int(match.group(2))
            name = match.group(3)
            suffix = match.group(4)
            if name in node_names:
                raise ValueError(f"duplicate template node: {name}")
            node_names.add(name)
            if depth == 1:
                max_top_fanout += 1
            if "(SINK)" in suffix:
                sink_names.add(name)
            else:
                buffer_count += 1

    path_count = 0
    with (source / "SS_delay.rpt").open(encoding="utf-8") as stream:
        for line in stream:
            match = PATH_RE.match(line)
            if not match:
                continue
            launch, capture = match.group(1), match.group(2)
            if launch not in sink_names or capture not in sink_names:
                raise ValueError(
                    f"timing path references a non-sink node: {launch} -> {capture}"
                )
            path_count += 1

    return len(sink_names), buffer_count, path_count, max_top_fanout


def write_structure(source: Path, destination: Path, copies: int) -> None:
    # Five leaves per aggregation buffer keeps every new fanout within the
    # five-entry delay tables used by the provided library.
    wrappers_per_group = 5
    group_count = (copies + wrappers_per_group - 1) // wrappers_per_group
    if group_count > 5:
        raise ValueError("at most 25 copies are supported by the two-level supertree")

    template_lines = (source / "clk_tree.structure").read_text(
        encoding="utf-8"
    ).splitlines()
    body = template_lines[1:]

    with (destination / "clk_tree.structure").open("w", encoding="utf-8", newline="\n") as out:
        out.write("Root: ROOT_CLK\n")
        for group in range(group_count):
            out.write(f"\t[1] HUGE_AGG_{group} (REALBUF_X16)\n")
            first = group * wrappers_per_group
            last = min(copies, first + wrappers_per_group)
            for copy_index in range(first, last):
                prefix = f"C{copy_index:02d}_"
                out.write(
                    f"\t\t[2] HUGE_COPY_{copy_index:02d} (REALBUF_X16)\n"
                )
                for line in body:
                    match = NODE_RE.match(line)
                    if not match:
                        if line.strip():
                            raise ValueError(f"unrecognized structure line: {line}")
                        continue
                    old_depth = int(match.group(2))
                    name = match.group(3)
                    suffix = match.group(4)
                    new_depth = old_depth + 2
                    indentation = "\t" * new_depth
                    out.write(
                        f"{indentation}[{new_depth}] {prefix}{name}{suffix}\n"
                    )


def write_report(source_file: Path, destination_file: Path, copies: int) -> int:
    header: list[str] = []
    paths: list[tuple[str, str, str]] = []
    with source_file.open(encoding="utf-8") as stream:
        for line in stream:
            match = PATH_RE.match(line)
            if match:
                paths.append((match.group(1), match.group(2), match.group(3)))
            elif not paths:
                header.append(line.rstrip("\r\n"))

    with destination_file.open("w", encoding="utf-8", newline="\n") as out:
        for line in header:
            out.write(line + "\n")
        path_number = 1
        for copy_index in range(copies):
            prefix = f"C{copy_index:02d}_"
            for launch, capture, delay in paths:
                out.write(
                    f"Path{path_number} : {prefix}{launch} -> "
                    f"{prefix}{capture} {delay}\n"
                )
                path_number += 1
    return len(paths)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("testcase0_v2"))
    parser.add_argument("--output", type=Path, default=Path("testcase_huge_530k"))
    parser.add_argument("--copies", type=int, default=20)
    args = parser.parse_args()

    if not 1 <= args.copies <= 25:
        parser.error("--copies must be between 1 and 25")
    required = ("clk_tree.structure", "buf.lib", "SS_delay.rpt", "FF_delay.rpt")
    for filename in required:
        if not (args.source / filename).is_file():
            parser.error(f"missing template file: {args.source / filename}")

    ff_count, buffer_count, ss_path_count, top_fanout = inspect_template(args.source)
    if top_fanout > 5:
        parser.error(f"template root fanout {top_fanout} exceeds library limit 5")

    args.output.mkdir(parents=True, exist_ok=True)
    write_structure(args.source, args.output, args.copies)
    actual_ss_paths = write_report(
        args.source / "SS_delay.rpt", args.output / "SS_delay.rpt", args.copies
    )
    actual_ff_paths = write_report(
        args.source / "FF_delay.rpt", args.output / "FF_delay.rpt", args.copies
    )
    shutil.copyfile(args.source / "buf.lib", args.output / "buf.lib")

    group_count = (args.copies + 4) // 5
    generated_ff = ff_count * args.copies
    generated_buffers = buffer_count * args.copies + args.copies + group_count
    print(f"source FFs       : {ff_count}")
    print(f"copies           : {args.copies}")
    print(f"generated FFs    : {generated_ff}")
    print(f"generated buffers: {generated_buffers}")
    print(f"SS paths         : {actual_ss_paths * args.copies}")
    print(f"FF paths         : {actual_ff_paths * args.copies}")
    print(f"output           : {args.output}")
    if actual_ss_paths != ss_path_count:
        raise RuntimeError("SS path count changed between inspection and generation")
    if generated_ff <= 500_000:
        print("warning: generated testcase does not exceed 500,000 FFs")


if __name__ == "__main__":
    main()
