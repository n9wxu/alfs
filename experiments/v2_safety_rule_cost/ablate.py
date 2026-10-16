#!/usr/bin/env python3
"""
What each of format 2's safety rules costs in code: alfs.c built without
the code of each rule, on a Cortex-M0+ (-Os, as `make target` measures),
and with --test, the test suite run on each variant to see which tests
notice. Format 1's code (commit 27d1be3) is built the same way.

Run from the repository's root:  python3 experiments/v2_safety_rule_cost/ablate.py [--test]
Variants and logs go to build/experiments/v2_safety_rule_cost/.
"""
import os, re, subprocess, sys

ALFS = os.getcwd()
OUT = os.path.join(ALFS, "build", "experiments", "v2_safety_rule_cost")
os.makedirs(os.path.join(OUT, "v1inc"), exist_ok=True)
SRC = open(os.path.join(ALFS, "alfs.c")).read()
TEST = "--test" in sys.argv


def git_show(rev, path):
    return subprocess.run(["git", "show", f"{rev}:{path}"], capture_output=True, text=True, check=True).stdout


def failing(code, name):
    """The tests that fail with this variant, on the suite's one seed."""
    c = os.path.join(OUT, f"ab_{name}_host.c")
    exe = c[:-2]
    open(c, "w").write(code)
    subprocess.run(["cc", "-std=c99", "-O1", "-w", "-I" + ALFS, "-I" + os.path.join(ALFS, "tests"), "-o", exe, c,
                    "tests/test_alfs.c", "tests/sim.c", "tests/tf.c"], check=True)
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    open(exe + ".log", "w").write(out)
    return re.findall(r"^FAIL\s+(\S+)", out, re.M)


def size_of(code, name, inc=ALFS):
    c = os.path.join(OUT, f"ab_{name}.c")
    o = c[:-2] + ".o"
    open(c, "w").write(code)
    r = subprocess.run(["arm-none-eabi-gcc", "-std=c99", "-Wall", "-Wextra", "-pedantic", "-Wno-unused-function",
                        "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-I" + inc, "-c", c, "-o", o],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{name}: compile failed\n{r.stderr}")
    s = subprocess.run(["arm-none-eabi-size", o], capture_output=True, text=True).stdout
    t, d = s.splitlines()[1].split()[:2]
    return int(t) + int(d)


def apply(code, edits, name):
    for old, new, *opt in edits:
        n = code.count(old)
        if n == 0 and opt:
            continue
        if n == 0:
            sys.exit(f"{name}: not found:\n{old}")
        code = code.replace(old, new)
    return code


A = [  # SAFE-04 rules 1-2: resume in the last sector (firm, then two pages on)
    ("""    uint32_t p = fs->cfg->page_size, x = off_data(fs), l = 0, page = off_claim(fs) / p;
    for (;;) {
        rc = scan_chunk(fs, last, &x, &l, NULL, false);
        if (rc < 0)
            return rc;
        if (!rc)
            break;
        page = x / p;
        x = ru(fs, x + HDR_LEN + l);
    }
    uint32_t at = (page + 2u) * p;
    s->flags = F_NEW | F_CLEAR;
    if (at + HDR_LEN + 1u <= fs->cfg->sector_size) {
        s->off = (uint16_t)at;
        s->flags = F_FIRM | F_CLEAR;
    }
    return ALFS_OK;""", """    s->flags = F_NEW | F_CLEAR;
    return ALFS_OK;"""),
    ("""    if (s->flags & F_FIRM)
        return firm(fs, s);
""", ""),
]

B = [  # SAFE-04 rule 3: clear ahead
    ("""    if (s->flags & F_CLEAR) {
        int rc = clear_ahead(fs, s);
        if (rc)
            return rc;
    }
""", ""),
    ("F_NEW | F_CLEAR;", "F_NEW;"),
    ("F_FIRM | F_CLEAR;", "F_FIRM;", "optional"),
]

C = [  # SAFE-04 rule 4: claim program from byte 0, stamp again
    ("bool whole = fs->cfg->prog_unit == 1u; /* SAFE-04 rule 4: from byte 0 */", "bool whole = false;"),
]

D = [  # SAFE-04 rule 5: the fresh sector
    ("""            /* a sector a claim program could have been taking: erased or torn,
             * however its bits read now, and not one alfs zeroed */
            if (g == fs->gen && !claim_valid(fs, h.claim) && !zeroed(fs, h.claim)) {
                if (fresh_any == NONE)
                    fresh_any = k;
                if (fresh_after == NONE && newest_any)
                    fresh_after = k;
            }
""", ""),
    ("""            if (g == fs->gen && claim_valid(fs, h.claim) && (!newest_any || get32(h.claim + 4) > newest)) {
                newest = get32(h.claim + 4);
                newest_any = true;
                fresh_after = NONE;
            }
""", ""),
    ("""    if (cfg->prog_unit == 1u) /* where the allocation cursor stood: SAFE-04 rule 5 */
        fs->fresh = (uint16_t)(fresh_after != NONE ? fresh_after : fresh_any);
""", ""),
    ("live_any = newest_any = false;", "live_any = false;"),
    ("                fresh_after = fresh_any = NONE;\n", ""),
    ("    uint32_t newest = 0, fresh_after = NONE, fresh_any = NONE; /* SAFE-04 rule 5 */\n", ""),
    ("live_any = false, newest_any = false;", "live_any = false;"),
    ("""        if (ok && k == fs->fresh) { /* SAFE-04 rule 5 */
            fs->fresh = NONE;
            fs->ready--;
            fs->dirty++;
            zero_claim(fs, k);
            return AGAIN;
        }
""", ""),
    ("    fs->fresh = NONE;\n", ""),
]

E = [  # SPEC 1.1: the sequence floor
    ("""            if (get32(h.stamp + 18) > seq_max)
                seq_max = get32(h.stamp + 18);
""", ""),
    ("        put32(st + 18, fs->next_seq);\n", ""),
    ("fs->next_seq = seq_max + 2u; /* past a torn claim's: SPEC 1.1 */", "fs->next_seq = seq_max + 1u;"),
]

F = [  # SAFE-11 with prog unit 1: re-read before erase, delete mark programmed again
    ("""        if (fs->cfg->prog_unit == 1u) {
            fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | E_FIRM);
            fs->estamped = large ? 0x80u : 0u;
        } else {
            fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | (large ? E_LARGE : E_ERASE));
        }
""", """        fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | (large ? E_LARGE : E_ERASE));
"""),
    ("""    if (st == E_FIRM) {
        int rc = firm_marks(fs);
        if (rc <= 0)
            return rc;
        st = (fs->estamped & 0x80u) ? E_LARGE : E_ERASE;
        fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | st);
        fs->estamped = 0;
    }
""", ""),
    ("    fs->firmed = NONE;\n", ""),
]

H = [  # program read-back
    ("""    while (n) {
        uint32_t k = n < PIECE ? n : PIECE;
        if (rd(fs, addr, b, k))
            return ALFS_OK; /* unknown, not wrong: the chunk's check decides on read */
        if (memcmp(b, p, k))
            return ALFS_EPROG;
        addr += k;
        p += k;
        n -= k;
    }
    return ALFS_OK;
}""", """    (void)b;
    return ALFS_OK;
}"""),
]

I = [  # SAFE-05: every byte read erased after an erase
    ("""    if (blank(fs, a, fs->cfg->sector_size, &ok))
        return ALFS_EIO;
""", "    ok = true;\n"),
]

J = [  # SAFE-05: the claim's page read erased before a sector is taken
    ("""        if (blank(fs, addr_of(fs, k) + from, next_page(fs, c) - from, &ok))
            return ALFS_EIO;
""", "        ok = true;\n"),
]

K = [  # SAFE-07: bad sectors
    ("""static bool is_bad(const alfs_t *fs, uint32_t k) {
    for (uint32_t i = 0; i < fs->nbad; i++)
        if (fs->bad[i] == k)
            return true;
    return false;
}""", """static bool is_bad(const alfs_t *fs, uint32_t k) {
    (void)fs;
    (void)k;
    return false;
}"""),
    ("""    if (!is_bad(fs, k) && fs->nbad < ALFS_BAD_MAX)
        fs->bad[fs->nbad++] = (uint16_t)k;
""", "    (void)k;\n"),
]

L = [  # SAFE-06: retry cap, then the bytes given up
    ("""    uint32_t max = fs->cfg->max_prog_retries ? fs->cfg->max_prog_retries : 3u;
    fs->n.prog_failures++;
    s->flags |= F_NEW;
    if (++s->fails < max)
        return ALFS_EPROG;
    fs->n.bytes_lost += s->used;
    s->used = 0;
    s->fails = 0;
    return ALFS_ELOST;""", """    fs->n.prog_failures++;
    s->flags |= F_NEW;
    return ALFS_EPROG;"""),
]

N = [  # LIFE-07: the large erase
    ("""static uint32_t large_sectors(const alfs_t *fs) {
    if (!fs->cfg->large_size || !fs->drv->erase_large || (fs->estate & E_NO_LARGE))
        return 0;
    return fs->cfg->large_size / fs->cfg->sector_size;
}""", """static uint32_t large_sectors(const alfs_t *fs) {
    (void)fs;
    return 0;
}"""),
    ("int rc = st == E_LARGE ? fs->drv->erase_large(fs->drv->ctx, a) : fs->drv->erase(fs->drv->ctx, a);",
     "int rc = fs->drv->erase(fs->drv->ctx, a);"),
    ("""        if (st == E_LARGE) { /* go sector by sector; the bad one shows itself */
            fs->estate = E_NO_LARGE | E_IDLE;
            return ALFS_OK;
        }
""", ""),
    ("fs->estamped = st == E_LARGE ? 0x80u : 0u;", "fs->estamped = 0;"),
    ("""    if (c->large_size && (!pow2(c->large_size) || c->large_size < s || c->large_size / s > 64u))
        return ALFS_EGEOMETRY;
""", ""),
]

full = size_of(SRC, "full")
open(os.path.join(OUT, "v1inc", "alfs.h"), "w").write(git_show("27d1be3", "alfs.h"))
v1 = size_of(git_show("27d1be3", "alfs.c"), "v1", inc=os.path.join(OUT, "v1inc"))
print(f"format 2: {full} bytes; format 1: {v1} bytes; format 2 less format 1: {full - v1}")
print()
rows = [("A rules 1-2 resume in last sector", [A]), ("B rule 3 clear ahead", [B]),
        ("C rule 4 claim from byte 0", [C]), ("D rule 5 fresh sector", [D]),
        ("E sequence floor", [E]), ("F delete mark again + re-read", [F]),
        ("ALL v2 power-loss rules A-F", [A, B, C, D, E, F]),
        ("H program read-back", [H]), ("I blank check after erase", [I]),
        ("J claim page check", [J]), ("I+J both blank checks", [I, J]),
        ("K bad sectors", [K]), ("L retry cap", [L]),
        ("v1-era H+I+J+K+L", [H, I, J, K, L]),
        ("N large erase", [N]),
        ("everything A-L", [A, B, C, D, E, F, H, I, J, K, L])]
for name, groups in rows:
    code = SRC
    for g in groups:
        code = apply(code, g, name)
    tag = "".join(c for c in name.split()[0] if c.isalnum()) + str(len(groups))
    s = size_of(code, tag)
    line = f"{name:38s} {s:5d}  saves {full - s:5d}"
    if TEST:
        line += "  " + (", ".join(failing(code, tag)) or "no test fails")
    print(line, flush=True)

print()
print("pairs, against a 6,144-byte budget:")
for name, keys in [("F+N", "FN"), ("A+N", "AN"), ("B+N", "BN"), ("D+N", "DN"), ("J+N", "JN"), ("F+A", "FA"),
                   ("F+J", "FJ"), ("F+B", "FB"), ("F+J+H", "FJH")]:
    code = SRC
    for k in keys:
        code = apply(code, globals()[k], name)
    s = size_of(code, "combo_" + keys)
    print(f"{name:8s} {s:5d}  saves {full - s:4d}  {'fits 6 KiB' if s <= 6144 else 'over by ' + str(s - 6144)}")
