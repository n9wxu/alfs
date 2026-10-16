# The code each of format 2's safety rules takes

**Question:** what do format 2's safety rules cost in code size?

**Method.** `ablate.py` takes this branch's `alfs.c` (format 2) and, for
each variant, removes the code of one rule or group of rules. It builds
each variant as `make target` does (Cortex-M0+, Thumb, `-Os`; warnings
about the unused code left behind are off) and reports text plus data.
With `--test` it runs the test suite against each variant, on the suite's
one simulator seed, and lists the tests that fail. It also builds format 1's
`alfs.c` (commit 27d1be3) the same way.

```
python3 experiments/v2_safety_rule_cost/ablate.py --test
```

The variants are measurements, not code alfs uses. `results.txt` is the
script's output.

## Sizes

Format 2 is 6,845 bytes and format 1 5,301; format 2 is 1,544 bytes more.

| Variant | Code removed | Bytes | Saves | Tests that fail |
|---|---|---|---|---|
| A | SAFE-04 rules 1 and 2: resuming in the last sector, firmed, two pages on | 6,557 | 288 | life_resume_waste |
| B | SAFE-04 rule 3: clearing claims ahead | 6,577 | 268 | safe_resume_torn_claim |
| C | SAFE-04 rule 4: each claim programmed from byte 0, the stamp again | 6,785 | 60 | safe_resume_torn_claim |
| D | SAFE-04 rule 5: the fresh sector | 6,601 | 244 | safe_powerloss_durable, safe_powerloss_bound, safe_no_program_after_torn, safe_resume_torn_claim, life_resume_waste |
| E | the sequence floor in each stamp | 6,813 | 32 | none |
| F | SAFE-11: the delete mark programmed again, and each sector read again before an erase | 6,377 | 468 | none |
| A to F | the six power-loss rules | 5,337 | 1,508 | those of A to D |
| H | the read-back of each program | 6,769 | 76 | safe_prog_failure |
| I | the blank check after an erase | 6,749 | 96 | safe_bad_sector |
| J | the check that a claim's page is erased before a sector is taken | 6,681 | 164 | none |
| I, J | both blank checks | 6,613 | 232 | safe_bad_sector |
| K | the bad-sector list | 6,717 | 128 | safe_bad_sector, safe_append_only_verified_ready |
| L | the retry cap | 6,809 | 36 | safe_prog_failure |
| H to L | the five checks and limits | 6,377 | 468 | those of H, I, K and L |
| N | LIFE-07: the large erase | 6,537 | 308 | life_large_erase |
| A to L | every safety rule above | 4,881 | 1,964 | all of the above |

The safety rules take 1,964 of format 2's 6,845 bytes; the six power-loss
rules 1,508 of them.

## Fitting 6 KiB

Format 2 is 701 bytes over a 6,144-byte budget. No single rule's removal
fits it; these pairs do:

| Pair | Bytes | Saves |
|---|---|---|
| F and N | 6,085 | 760 |
| F and A | 6,085 | 760 |
| F and B | 6,109 | 736 |
| F, J and H | 6,133 | 712 |

Every pair that fits includes F, the delete mark programmed again.

## What the results show

- On the suite's one seed, removing E, F or J fails no test. That one seed
  does not exercise them.
- D, the fresh sector, is the rule whose removal fails the most tests.
