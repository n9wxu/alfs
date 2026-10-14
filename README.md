# alfs

The append-log flash store: a small filesystem for append-only logs on
serial NOR flash, built for systems that run code from the same chip and
cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop. In flight, nothing erases:
every append programs flash that was erased ahead of time, and the caller
schedules erases in steps.

**Status:** draft 1, 2026-10-14. Requirements, tests and code; every test
passes (`make test`), and the Cortex-M0+ budgets hold (`make target`):
5.3 KB of code against littlefs's 15.5 KB on the same build. On littlefs's
10-minute logging workload at 50 rows a second it erases 168 sectors, all
in erase steps the caller schedules, where littlefs 2.11.3 erases 807 in
its append path. Not yet run on hardware.

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/TEST_PLAN.md` | how each requirement is tested |
| `docs/SPEC.md` | every byte on the flash |
| `alfs.h`, `alfs.c` | the interface and the code |
| `tests/` | the simulated flash, the harness and the tests |
| `docs/datasheets/` | the flash parts the figures come from |

The use cases come from the pyro flight computer
(github.com/n9wxu/pyro_fw); the flash fault model from the littlefs v3 work
(github.com/n9wxu/littlefs). alfs shares no code with either.
