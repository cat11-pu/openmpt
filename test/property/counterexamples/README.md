# Adversarial counterexamples

The 200-case run found no implementation failure, so there is no
crash/leak/OOB counterexample to report.  The files below are the
representative adversarial inputs that exercise every category and
format.  `mutation_offset` is the first byte that was removed or
changed; it is the offset printed by a failing case.

| case | category | format | offset | size | outcome |
| --- | --- | --- | ---: | ---: | --- |
| `case-001-invalid_pattern` | `invalid_pattern` | `mod` | 1084 | 5400 | bounded render |
| `case-002-huge_sample_count` | `huge_sample_count` | `mod` | 42 | 5400 | bounded render |
| `case-003-cycle_reference` | `cycle_reference` | `mod` | 2094 | 5400 | bounded render |
| `case-005-invalid_pattern` | `invalid_pattern` | `xm` | 70 | 6279 | safely rejected |
| `case-006-huge_sample_count` | `huge_sample_count` | `xm` | 72 | 6279 | safely rejected |
| `case-007-cycle_reference` | `cycle_reference` | `xm` | 64 | 6279 | bounded render |
| `case-008-truncation` | `truncation` | `s3m` | 48 | 48 | safely rejected |
| `case-009-invalid_pattern` | `invalid_pattern` | `s3m` | 36 | 880 | safely rejected |
| `case-010-huge_sample_count` | `huge_sample_count` | `s3m` | 34 | 880 | safely rejected |
| `case-011-cycle_reference` | `cycle_reference` | `s3m` | 32 | 880 | bounded render |
| `case-012-truncation` | `truncation` | `it` | 38 | 38 | safely rejected |
| `case-013-invalid_pattern` | `invalid_pattern` | `it` | 38 | 12323 | safely rejected |
| `case-014-huge_sample_count` | `huge_sample_count` | `it` | 36 | 12323 | safely rejected |
| `case-015-cycle_reference` | `cycle_reference` | `it` | 32 | 12323 | bounded render |
| `case-020-truncation` | `truncation` | `mod` | 952 | 952 | safely rejected |
