# The code each requirement takes in format 3

**Question:** what is the simplest alfs that still meets the requirements,
and what does each requirement cost in code?

**Method.** `ablate.py` takes `alfs.c` and, for each variant, removes the
code that serves one requirement. It builds each variant as `make target`
does (Cortex-M0+, Thumb, `-Os`; warnings about the unused code left behind
are off) and reports text plus data. With `--test` it also runs the core's
44 tests on seeds 1 to 16 against each variant and lists the tests that
fail, with the number of seeds on which each fails.

```
python3 experiments/v3_requirement_cost/ablate.py --test
```

The variants are measurements, not code alfs uses. `results.txt` is the
script's output.

## Where the core's code goes

From a build without inlining (7,472 bytes), each function counted once,
by the work it serves:

| Area | Bytes |
|---|---|
| create, write, sync, close | 1,756 |
| names, versions, list, delete | 1,618 |
| mount and sector states | 1,528 |
| erase and format | 1,422 |
| open and read | 796 |
| bytes, CRC, status | 352 |

No area holds most of the code. Each is the work of several requirements.

## Each requirement's code removed

Bytes are the variant's size; "saves" is against alfs as it is (7,101).

| Variant | Code removed | Bytes | Saves | Tests that fail (seeds of 16) |
|---|---|---|---|---|
| full | none | 7,101 | 0 | none |
| A | LIFE-07: the large erase | 6,809 | 292 | life_large_erase (16) |
| B | LIFE-05: erase counts kept in the stamps | 6,925 | 176 | life_erase_count_persists (16), obs_status (16), if_endian (16) |
| C | SAFE-06: retry in a fresh sector, and the read-back of each program | 6,945 | 156 | safe_prog_failure (16) |
| D | SAFE-14: rebuilding a torn close record; the delete is refused instead | 6,917 | 184 | safe_config_replace (5) |
| E | IF-02: sector size, page size and erase value configured; fixed at 4,096, 256 and FFh | 6,809 | 292 | none |
| F | LIFE-06: the cap on programs into one page | 7,017 | 84 | life_partial_program_cap (16) |
| ABCEF | A, B, C, E and F | 6,149 | 952 | the tests of those five, as above |
| ABCDEF | all six | 5,973 | 1,128 | the tests of those five, and safe_config_replace (5) |
| P1 | SAFE-11: the dead mark programmed again, and each sector read again, before an erase | 6,817 | 284 | safe_delete_reclaim (8) |
| P2 | SAFE-14: the newer version's close record programmed again before a delete | 6,689 | 412 | safe_config_replace (9) |
| P3 | SAFE-04 rule 3: the fresh sector | 6,885 | 216 | safe_powerloss_durable (16), safe_powerloss_bound (16), safe_no_program_after_torn (16), if_version_never_reused (16) |
| P4 | IF-07: the sequence floor, and three past a torn claim | 7,057 | 44 | if_version_never_reused (16), safe_create_atomic (5), if_endian (16) |
| P | P1 to P4 | 6,105 | 996 | all of P1 to P4's |
| ALL | ABCEF and P1 to P4 | 5,217 | 1,884 | all of the above |

## What the results show

- **Every power-loss rule is needed.** Without any one of P1 to P4, a
  power-loss test fails on at least 5 of the 16 seeds: a synced file is
  lost, a deleted file returns in part, a version is given twice, or no
  whole configuration is left.
- **A, B, C, E and F each fail only their own requirement's tests.** Taken
  together they are 952 bytes, and the variant without them (6,149 bytes)
  passes every other test on all 16 seeds. `if_endian` fails in B and P4
  because it checks the stamp's bytes, which then hold zero where the
  erase count and the floor were.
- **D does not meet SAFE-14.** Refusing the delete keeps a whole
  configuration, but the application's tidy-up at boot gets `ALFS_EOPEN`
  and the older versions stay: the delete SAFE-14 requires does not happen.
- **No test exercises IF-02's configurability.** Every test runs 4,096-byte
  sectors, 256-byte pages and the erase value FFh; `if_geometry` checks
  only that wrong geometries are refused. E fails nothing for that reason.

## The simplest variant

The simplest variant that passes every test of the requirements it keeps
is ABCEF: 6,149 bytes. It drops five requirements:
- LIFE-07: the large erase;
- LIFE-05: erase counts;
- SAFE-06: retrying a failed program;
- IF-02: configurable geometry;
- LIFE-06: the per-page program cap.

None of the five is a behaviour the user's statements in
`docs/REQUIREMENTS.md` section 1 ask for. What each one gives:
- **LIFE-07:** a pre-flight erase of a whole region is 2.6 to 4.8 times
  faster with 64 KB erases (typical times, requirements section 4.3).
- **LIFE-05:** wear is reported.
- **SAFE-06:** a failed program loses no bytes, where without it the chunk
  is dropped and counted.
- **IF-02:** a part with other sizes needs no rebuild.
- **LIFE-06:** a part that limits partial programs is safe.

Every other requirement, the four power-loss rules included, stays.
