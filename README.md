# alfs

The append-log flash store: a small filesystem for append-only logs on
serial NOR flash, built for systems that run code from the same chip and
cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop. In flight, nothing erases:
every append programs flash that was erased ahead of time, and the caller
schedules erases in steps.

**Status:** requirements and tests written; the code is next. `make test`
runs the host suite once `alfs.c` exists.

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/TEST_PLAN.md` | how each requirement is tested |
| `alfs.h` | the interface |
| `tests/` | the simulated flash, the harness and the tests |
| `docs/datasheets/` | the flash parts the figures come from |

The use cases come from the pyro flight computer
(github.com/n9wxu/pyro_fw); the flash fault model from the littlefs v3 work
(github.com/n9wxu/littlefs). alfs shares no code with either.
