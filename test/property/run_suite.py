#!/usr/bin/env python3
"""Run the 200-case property suite and the 50-file differential suite."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CORPUS = HERE / "corpus"
BIN = HERE / "bin"
RESULTS = HERE / "results"


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def ensure_corpus() -> None:
    if (CORPUS / "cases_manifest.json").is_file() and (CORPUS / "valid_manifest.json").is_file():
        return
    subprocess.run([sys.executable, str(HERE / "generate_corpus.py")], check=True)


def ensure_binaries() -> None:
    if (BIN / "module_property_runner_asan").is_file() and (BIN / "module_property_runner_ref").is_file():
        return
    subprocess.run(["bash", str(HERE / "build.sh")], check=True)


def sanitizer_env() -> dict[str, str]:
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = (
        "detect_leaks=1:halt_on_error=1:abort_on_error=1:"
        "strict_string_checks=1:allocator_may_return_null=0"
    )
    env["LSAN_OPTIONS"] = "exitcode=23:report_objects=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1:print_stacktrace=1"
    return env


def first_error_line(stderr: str) -> str:
    for line in stderr.splitlines():
        lowered = line.lower()
        if "error:" in lowered or "runtime error" in lowered or "leak" in lowered or "sanitizer" in lowered:
            return line.strip()
    lines = [line.strip() for line in stderr.splitlines() if line.strip()]
    return lines[0] if lines else ""


def run_case(meta: dict) -> dict:
    case_path = CORPUS / "cases" / f"{meta['id']}.bin"
    started = time.monotonic()
    try:
        proc = subprocess.run(
            [str(BIN / "module_property_runner_asan"), "--probe", str(case_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=12.0,
            env=sanitizer_env(),
        )
        timed_out = False
    except subprocess.TimeoutExpired as exc:
        proc = None
        timed_out = True
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", "replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", "replace")

    elapsed_ms = round((time.monotonic() - started) * 1000.0, 3)
    if timed_out:
        return {
            **meta,
            "status": "fail",
            "failure_kind": "timeout",
            "failure_offset": meta["mutation_offset"],
            "failure_report": f"{meta['id']} offset={meta['mutation_offset']} timeout",
            "returncode": None,
            "elapsed_ms": elapsed_ms,
            "stderr_first": "timeout after 12 seconds",
        }

    assert proc is not None
    parsed = None
    if proc.stdout.strip():
        try:
            parsed = json.loads(proc.stdout.strip().splitlines()[-1])
        except json.JSONDecodeError:
            parsed = None

    if proc.returncode != 0:
        if proc.returncode == 23:
            kind = "leak"
        elif "AddressSanitizer" in proc.stderr:
            kind = "crash_or_oob"
        elif "runtime error:" in proc.stderr:
            kind = "undefined_behavior"
        else:
            kind = "process_failure"
        return {
            **meta,
            "status": "fail",
            "failure_kind": kind,
            "failure_offset": meta["mutation_offset"],
            "failure_report": f"{meta['id']} offset={meta['mutation_offset']} {kind}",
            "returncode": proc.returncode,
            "elapsed_ms": elapsed_ms,
            "stderr_first": first_error_line(proc.stderr),
            "stderr": proc.stderr[-12000:],
            "stdout": proc.stdout[-4000:],
        }

    if parsed is None:
        return {
            **meta,
            "status": "fail",
            "failure_kind": "bad_runner_output",
            "failure_offset": meta["mutation_offset"],
            "failure_report": f"{meta['id']} offset={meta['mutation_offset']} bad runner output",
            "returncode": proc.returncode,
            "elapsed_ms": elapsed_ms,
            "stdout": proc.stdout[-4000:],
            "stderr": proc.stderr[-12000:],
        }
    if not parsed.get("success", False):
        return {
            **meta,
            "status": "fail",
            "failure_kind": "property_violation",
            "failure_offset": meta["mutation_offset"],
            "failure_report": f"{meta['id']} offset={meta['mutation_offset']} property violation",
            "returncode": proc.returncode,
            "elapsed_ms": elapsed_ms,
            "stderr_first": first_error_line(proc.stderr),
            "runner": parsed,
            "stderr": proc.stderr[-12000:],
        }

    return {
        **meta,
        "status": "pass",
        "returncode": proc.returncode,
        "elapsed_ms": elapsed_ms,
        "runner": parsed,
    }


def run_property_suite(jobs: int) -> dict:
    manifest = load_json(CORPUS / "cases_manifest.json")
    RESULTS.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(run_case, manifest))
    results.sort(key=lambda item: item["id"])

    failures = [item for item in results if item["status"] == "fail"]
    for item in failures:
        print(f"FAIL {item['failure_report']} :: {item.get('stderr_first', '')}", file=sys.stderr)
    by_category = {}
    for category in ("truncation", "invalid_pattern", "huge_sample_count", "cycle_reference"):
        subset = [item for item in results if item["category"] == category]
        by_category[category] = {
            "cases": len(subset),
            "passed": sum(item["status"] == "pass" for item in subset),
            "failed": sum(item["status"] == "fail" for item in subset),
            "loaded": sum(bool(item.get("runner", {}).get("loaded")) for item in subset),
            "rejected": sum(not bool(item.get("runner", {}).get("loaded")) for item in subset),
        }

    summary = {
        "cases": len(results),
        "passed": len(results) - len(failures),
        "failed": len(failures),
        "crashes": sum(item.get("failure_kind") == "crash_or_oob" for item in failures),
        "leaks": sum(item.get("failure_kind") == "leak" for item in failures),
        "undefined_behavior": sum(item.get("failure_kind") == "undefined_behavior" for item in failures),
        "timeouts": sum(item.get("failure_kind") == "timeout" for item in failures),
        "property_violations": sum(item.get("failure_kind") == "property_violation" for item in failures),
        "loaded": sum(bool(item.get("runner", {}).get("loaded")) for item in results),
        "rejected": sum(not bool(item.get("runner", {}).get("loaded")) for item in results),
        "by_category": by_category,
        "total_rendered_frames": sum(int(item.get("runner", {}).get("frames", 0)) for item in results),
        "max_rendered_frames_per_case": max((int(item.get("runner", {}).get("frames", 0)) for item in results), default=0),
        "error_callback_events": sum(int(item.get("runner", {}).get("error_events", 0)) for item in results),
        "log_callback_events": sum(int(item.get("runner", {}).get("log_events", 0)) for item in results),
        "guards_intact": all(bool(item.get("runner", {}).get("guard_ok", False)) for item in results if item.get("runner", {}).get("loaded")),
    }

    write_json(RESULTS / "property_results.json", results)
    (RESULTS / "property_results.jsonl").write_text(
        "".join(json.dumps(item, sort_keys=True) + "\n" for item in results),
        encoding="utf-8",
    )
    write_json(RESULTS / "property_summary.json", summary)

    counter_dir = RESULTS / "counterexamples"
    if counter_dir.exists():
        shutil.rmtree(counter_dir)
    counter_dir.mkdir(parents=True, exist_ok=True)
    for item in failures:
        stem = item["id"]
        shutil.copy2(CORPUS / "cases" / f"{stem}.bin", counter_dir / f"{stem}.bin")
        write_json(counter_dir / f"{stem}.json", item)
    if not failures:
        (counter_dir / "README.md").write_text(
            "No property-test counterexamples were found in the 200-case run.\n"
            "Rejected malformed inputs are recorded in ../property_results.json.\n",
            encoding="utf-8",
        )
    return summary


def run_render(binary: Path, source: Path, output: Path, frames: int) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(binary), "--render", str(source), str(output), str(frames)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=60.0,
    )


def compare_pcm(candidate: Path, reference: Path) -> dict:
    cand = candidate.read_bytes()
    ref = reference.read_bytes()
    common_bytes = min(len(cand), len(ref))
    common_samples = common_bytes // 2
    mismatch = None
    compared = 0
    for sample_index in range(common_samples):
        c_value = int.from_bytes(cand[sample_index * 2 : sample_index * 2 + 2], "little", signed=True)
        r_value = int.from_bytes(ref[sample_index * 2 : sample_index * 2 + 2], "little", signed=True)
        compared += 1
        if c_value != r_value:
            mismatch = {
                "sample_index": sample_index,
                "byte_offset": sample_index * 2,
                "frame": sample_index // 2,
                "channel": sample_index % 2,
                "candidate": c_value,
                "reference": r_value,
                "delta": c_value - r_value,
            }
            break
    if mismatch is None and len(cand) != len(ref):
        mismatch = {
            "sample_index": common_samples,
            "byte_offset": common_bytes,
            "candidate_bytes": len(cand),
            "reference_bytes": len(ref),
            "reason": "length_mismatch",
        }
    return {
        "matched": mismatch is None,
        "candidate_bytes": len(cand),
        "reference_bytes": len(ref),
        "compared_samples": compared,
        "first_mismatch": mismatch,
        "candidate_sha256": hashlib.sha256(cand).hexdigest(),
        "reference_sha256": hashlib.sha256(ref).hexdigest(),
    }


def run_differential_suite(frames: int) -> dict:
    valid_manifest = load_json(CORPUS / "valid_manifest.json")
    render_dir = RESULTS / "pcm"
    render_dir.mkdir(parents=True, exist_ok=True)
    results = []
    total_samples = 0
    for item in valid_manifest:
        source = ROOT / item["path"]
        cand_pcm = render_dir / f"{item['valid_id']}.candidate.pcm"
        ref_pcm = render_dir / f"{item['valid_id']}.reference.pcm"
        cand_proc = run_render(BIN / "module_property_runner_asan", source, cand_pcm, frames)
        ref_proc = run_render(BIN / "module_property_runner_ref", source, ref_pcm, frames)
        if cand_proc.returncode != 0 or ref_proc.returncode != 0:
            results.append(
                {
                    **item,
                    "matched": False,
                    "candidate_returncode": cand_proc.returncode,
                    "reference_returncode": ref_proc.returncode,
                    "candidate_stderr": cand_proc.stderr[-4000:],
                    "reference_stderr": ref_proc.stderr[-4000:],
                }
            )
            continue
        comparison = compare_pcm(cand_pcm, ref_pcm)
        total_samples += comparison["compared_samples"]
        results.append({**item, **comparison})
        if comparison["matched"]:
            cand_pcm.unlink(missing_ok=True)
            ref_pcm.unlink(missing_ok=True)

    mismatches = [item for item in results if not item["matched"]]
    summary = {
        "files": len(results),
        "matched": len(results) - len(mismatches),
        "mismatched": len(mismatches),
        "target_frames_per_file": frames,
        "compared_samples": total_samples,
        "sample_rate": 48000,
        "channels": 2,
        "sample_format": "s16le",
        "candidate": str(BIN / "module_property_runner_asan"),
        "reference": "/usr/lib/x86_64-linux-gnu/libopenmpt.so.0",
    }
    write_json(RESULTS / "differential_results.json", results)
    write_json(RESULTS / "differential_summary.json", summary)

    if mismatches:
        counter_dir = RESULTS / "counterexamples"
        counter_dir.mkdir(parents=True, exist_ok=True)
        for item in mismatches:
            stem = item["valid_id"]
            shutil.copy2(ROOT / item["path"], counter_dir / f"{stem}.mod")
            write_json(counter_dir / f"{stem}.differential.json", item)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--frames", type=int, default=50000)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-property", action="store_true")
    parser.add_argument("--skip-differential", action="store_true")
    args = parser.parse_args()

    ensure_corpus()
    if not args.skip_build:
        ensure_binaries()
    elif not (BIN / "module_property_runner_asan").is_file() or not (BIN / "module_property_runner_ref").is_file():
        raise SystemExit("runner binaries are missing; run build.sh first")

    if not args.skip_property:
        property_summary = run_property_suite(args.jobs)
        print(json.dumps(property_summary, indent=2, sort_keys=True))
    if not args.skip_differential:
        differential_summary = run_differential_suite(args.frames)
        print(json.dumps(differential_summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
