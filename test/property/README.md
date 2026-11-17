# libopenmpt property and differential test suite

This directory contains a self-contained harness for the requested
200-case malformed-module property run and the 50-file sample-by-sample
differential run.

## Contents

- `generate_corpus.py`: deterministic generator for 50 valid 4-channel MOD
  files and 200 malformed cases (50 each of truncation, invalid pattern,
  huge sample span and cyclic references, strictly alternating).
- `module_property_runner.cpp`: public-C-API runner. It probes the header,
  loads from memory, exercises metadata/name/pattern/position APIs, renders
  mono/stereo/quad with guard regions, and repeats create/destroy cycles.
- `build.sh`: builds the ASan+UBSan candidate runner and a reference runner
  linked against the system libopenmpt.
- `run_suite.py`: runs every malformed case in its own process, captures
  sanitizer output and timeouts, then renders the 50 valid files through both
  implementations and compares every PCM sample.
- `collect_counterexamples.py`: collects representative adversarial inputs,
  including the exact mutation offsets.
- `counterexamples/`: 15 representative adversarial files with JSON sidecars.
- `results/`: machine-readable property and differential results.

## Reproduce

```bash
bash test/property/build.sh
python3 test/property/generate_corpus.py
python3 test/property/run_suite.py --skip-build --jobs 8
python3 test/property/collect_counterexamples.py
```

The sanitizer run uses:

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1:strict_string_checks=1
UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1
LSAN_OPTIONS=exitcode=23:report_objects=1
```

Each malformed case runs in a separate process with a 12 second timeout, so a
hang cannot be hidden by a finite read loop.

## Corpus

The 50 valid files are deterministic, hand-written ProTracker MODs. They use
four channels, 1-2 patterns, four looping sample waveforms and a rotating mix
of valid arpeggio, vibrato, tremolo, volume-slide and extended effects.

The 200 property cases cover 200 seed instances: 184 independent generated
player-style MODs, plus each of the four in-tree
`test/test.{mod,xm,s3m,mptm}` files used once in each category. Categories
alternate by case number:

```text
case N % 4 == 0: truncation
case N % 4 == 1: invalid pattern / invalid pattern reference
case N % 4 == 2: huge sample count or sample span
case N % 4 == 3: cyclic order/pattern reference
```

Every case sidecar contains `mutation_offset`, `source`, `format`,
`description`, size and SHA-256. A failing sanitizer/guard/timeout result
therefore always has a concrete byte offset attached to it.

## Results

Property run (`results/property_summary.json`):

| metric | result |
| --- | ---: |
| cases | 200 |
| passed | 200 |
| crashes / ASan OOB | 0 |
| leaks / LSan | 0 |
| UBSan errors | 0 |
| timeouts | 0 |
| output guard violations | 0 |
| loaded and bounded-rendered | 155 |
| safely rejected | 45 |
| rendered frames | 5,481,072 |

Category breakdown:

| category | cases | loaded | rejected | failed |
| --- | ---: | ---: | ---: | ---: |
| truncation | 50 | 11 | 39 | 0 |
| invalid_pattern | 50 | 47 | 3 | 0 |
| huge_sample_count | 50 | 47 | 3 | 0 |
| cycle_reference | 50 | 50 | 0 | 0 |

Differential run (`results/differential_summary.json`):

| metric | result |
| --- | ---: |
| valid files | 50 |
| matching files | 50 |
| mismatching files | 0 |
| frames per file | 50,000 |
| sample rate | 48,000 Hz |
| format | stereo s16le |
| compared samples | 5,000,000 |

Candidate: this checkout, libopenmpt 0.9.0-pre.13, ASan+UBSan build.

Reference: distribution libopenmpt 0.8.4
(`/usr/lib/x86_64-linux-gnu/libopenmpt.so.0`).

Both sides use identical controls: no dither, no Amiga resampler, no
interpolation, no volume ramping, stereo separation 100%, repeat count 0.

If a sample differs, `results/differential_results.json` records
`sample_index`, `byte_offset`, `frame`, `channel`, both values and the delta.
No such mismatch occurred in this run.

## Counterexamples

`counterexamples/README.md` lists 15 representative inputs. They are
adversarial inputs, not implementation failures: the 200-case run produced no
crash, OOB, leak or timeout counterexample. The smallest examples include a
48-byte truncated S3M, a 38-byte truncated IT/MPTM, XM/S3M/IT headers claiming
65535 samples, and MOD/XM/S3M/IT order cycles.

This harness uses generated player-style modules rather than downloading a
third-party ModArchive collection, so the corpus is reproducible offline.
The reference is an independently built distribution libopenmpt, not an
unrelated tracker implementation.
