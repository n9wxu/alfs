# alfs

The append-log flash store: a small filesystem for append-only logs on
serial NOR flash, built for systems that run code from the same chip and
cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop. In flight, nothing erases:
every append programs flash that was erased ahead of time, and the caller
schedules erases in steps.

**Status:** requirements, draft 1 (`docs/REQUIREMENTS.md`). Tests come next
(`docs/TEST_PLAN.md`), then the code.

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/datasheets/` | the flash parts the figures come from |

The use cases come from the pyro flight computer
(github.com/n9wxu/pyro_fw); the flash fault model from the littlefs v3 work
(github.com/n9wxu/littlefs). alfs shares no code with either.
