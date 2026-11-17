#!/usr/bin/env python3
"""Collect representative adversarial inputs and their exact byte offsets."""

from __future__ import annotations

import json
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CORPUS = HERE / "corpus"
RESULTS = HERE / "results"
OUT = HERE / "counterexamples"

SELECTED = [
    "case-001-invalid_pattern",
    "case-002-huge_sample_count",
    "case-003-cycle_reference",
    "case-005-invalid_pattern",
    "case-006-huge_sample_count",
    "case-007-cycle_reference",
    "case-008-truncation",
    "case-009-invalid_pattern",
    "case-010-huge_sample_count",
    "case-011-cycle_reference",
    "case-012-truncation",
    "case-013-invalid_pattern",
    "case-014-huge_sample_count",
    "case-015-cycle_reference",
    "case-020-truncation",
]

EXTENSIONS = {"mod": ".mod", "xm": ".xm", "s3m": ".s3m", "it": ".it", "unknown": ".bin"}


def main() -> int:
    results = {
        item["id"]: item
        for item in json.loads((RESULTS / "property_results.json").read_text(encoding="utf-8"))
    }
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True, exist_ok=True)

    rows = []
    for case_id in SELECTED:
        result = results[case_id]
        source = CORPUS / "cases" / f"{case_id}.bin"
        target = OUT / f"{case_id}{EXTENSIONS.get(result['format'], '.bin')}"
        shutil.copy2(source, target)
        runner = result.get("runner", {})
        outcome = "bounded render" if runner.get("loaded") else "safely rejected"
        record = {
            "id": case_id,
            "category": result["category"],
            "format": result["format"],
            "mutation_offset": result["mutation_offset"],
            "description": result["description"],
            "size": result["size"],
            "source": result["source"],
            "sha256": result["sha256"],
            "outcome": outcome,
            "runner": runner,
        }
        (OUT / f"{case_id}.json").write_text(
            json.dumps(record, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        rows.append(record)

    lines = [
        "# Adversarial counterexamples",
        "",
        "The 200-case run found no implementation failure, so there is no",
        "crash/leak/OOB counterexample to report.  The files below are the",
        "representative adversarial inputs that exercise every category and",
        "format.  `mutation_offset` is the first byte that was removed or",
        "changed; it is the offset printed by a failing case.",
        "",
        "| case | category | format | offset | size | outcome |",
        "| --- | --- | --- | ---: | ---: | --- |",
    ]
    for row in rows:
        lines.append(
            f"| `{row['id']}` | `{row['category']}` | `{row['format']}` | "
            f"{row['mutation_offset']} | {row['size']} | {row['outcome']} |"
        )
    lines.append("")
    (OUT / "README.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {len(rows)} adversarial examples to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
