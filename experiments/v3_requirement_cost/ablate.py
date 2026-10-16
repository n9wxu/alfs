#!/usr/bin/env python3
"""
What each requirement costs in alfs's code: alfs.c built without the code
that serves it, on a Cortex-M0+ (-Os, as `make target` measures), and with
--test, the test suite run on each variant to see which tests notice.

Run from the repository's root:  python3 experiments/v3_requirement_cost/ablate.py [--test]
Variants and logs go to build/experiments/v3_requirement_cost/.
"""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.getcwd()
OUT = os.path.join(ROOT, "build", "experiments", "v3_requirement_cost")
SRC = open(os.path.join(ROOT, "alfs.c")).read()
SEEDS = range(1, 17)


def cut_func(code, start, sig):
    """Removes from `start` through the end of the function `sig` begins."""
    a = code.find(start)
    b = code.find(sig, a)
    if a < 0 or b < 0:
        sys.exit(f"not found: {start!r} / {sig!r}")
    return code[:a] + code[code.index("\n}\n", b) + 3:]


def cut_between(code, start, end):
    """Removes from `start` up to, not including, `end`."""
    a = code.find(start)
    b = code.find(end, a)
    if a < 0 or b < 0:
        sys.exit(f"not found: {start!r} .. {end!r}")
    return code[:a] + code[b:]


def rep(old, new, optional=False):
    def f(code):
        if old not in code:
            if optional:
                return code
            sys.exit(f"not found:\n{old}")
        return code.replace(old, new)
    return f


# LIFE-07: one large erase for a whole aligned block of dirty sectors.
A = [
    lambda c: cut_func(c, "static uint32_t large_sectors", "static uint32_t large_sectors"),
    lambda c: cut_func(c, "/* A whole aligned block", "static int large_here"),
    rep("        bool large;\n", ""),
    rep("""        if (large_here(fs, k, h, &large))
            return ALFS_EIO;
        uint32_t n = large ? large_sectors(fs) : 1u;
""", "        uint32_t n = 1u;\n"),
    rep("""    if (n > 1u ? fs->drv->erase_large(fs->drv->ctx, a) : fs->drv->erase(fs->drv->ctx, a)) {
        if (n > 1u)
            fs->no_large = 1; /* go sector by sector; the bad one shows itself */
        else
            mark_bad(fs, k);
        return ALFS_OK;
    }""", """    if (fs->drv->erase(fs->drv->ctx, a)) {
        mark_bad(fs, k);
        return ALFS_OK;
    }"""),
    rep("""    if (c->large_size && (!pow2(c->large_size) || c->large_size < s || c->large_size / s > 64u))
        return ALFS_EGEOMETRY;
""", ""),
]

# LIFE-05 and OBS-01's wear figures: each sector's erase count kept in its stamp.
B = [
    lambda c: cut_func(c, "/* The erase count sectors", "static int wear_for"),
    lambda c: cut_func(c, "static void see_wear", "static void see_wear"),
    rep("""    uint32_t w, a = addr_of(fs, k);
    if (wear_for(fs, k, n, &w))
        return ALFS_EIO;""", "    uint32_t w = 0, a = addr_of(fs, k);"),
    rep("        see_wear(fs, w);\n", ""),
    rep("            see_wear(fs, get32(h->stamp + 14));\n", ""),
    rep("    fs->min_wear = UINT32_MAX;\n", ""),
    rep("""    st->min_wear = fs->min_wear == UINT32_MAX ? 0 : fs->min_wear;
    st->max_wear = fs->max_wear;
""", ""),
]

# SAFE-06: a failed program's bytes retried in a fresh sector, and every
# program read back to find one that silently failed.
C = [
    rep("""    uint8_t b[PIECE];
    fs->n.programs++;
    if (fs->drv->prog(fs->drv->ctx, addr, p, n))
        return ALFS_EPROG;
    while (n) {
        uint32_t k = n < PIECE ? n : PIECE;
        if (rd(fs, addr, b, k))
            return ALFS_OK; /* unknown, not wrong: the chunk's check decides on read */
        if (memcmp(b, p, k))
            return ALFS_EPROG;
        addr += k;
        p += k;
        n -= k;
    }
    return ALFS_OK;""", """    fs->n.programs++;
    return fs->drv->prog(fs->drv->ctx, addr, p, n) ? ALFS_EPROG : ALFS_OK;"""),
    rep("""    uint32_t max = fs->cfg->max_prog_retries ? fs->cfg->max_prog_retries : 3u;
    for (;;) {
        int32_t m = prog_chunk(fs, f, p, n);
        if (m != ALFS_EPROG) {
            if (m > 0) {
                f->fails = 0;
                f->size += (uint32_t)m;
            }
            return m;
        }
        fs->n.prog_failures++;
        f->flags |= F_NEW;
        if (++f->fails >= max) {
            f->fails = 0;
            fs->n.bytes_lost += n;
            return ALFS_ELOST;
        }
    }""", """    int32_t m = prog_chunk(fs, f, p, n);
    if (m > 0)
        f->size += (uint32_t)m;
    if (m != ALFS_EPROG)
        return m;
    fs->n.prog_failures++;
    fs->n.bytes_lost += n;
    f->flags |= F_NEW;
    return ALFS_ELOST;"""),
    rep("uint32_t n = name_len(name), max, k;", "uint32_t n = name_len(name), k;"),
    rep("""    max = fs->cfg->max_prog_retries ? fs->cfg->max_prog_retries : 3u;
    for (uint32_t tries = 0;;) {""", "    {"),
    rep("""        fs->n.prog_failures++;
        if (++tries >= max)
            return ALFS_EPROG;""", """        fs->n.prog_failures++;
        return ALFS_EPROG;"""),
]

# SAFE-14's rebuild of a torn close record from the file's chunks: the
# delete is refused instead, which keeps the older version.
D = [
    rep("""        if (!whole) { /* torn: the record it was writing (SPEC 5) */
            uint32_t size;
            if (size_of(fs, newer, nfirst, &size))
                return ALFS_EIO;
            put32(rec, size);
            put32(rec + 4, crc_of(newer, rec, 4));
        }""", """        if (!whole)
            return ALFS_EOPEN;"""),
    lambda c: cut_func(c, "/* File id's size", "static int size_of"),
]

# IF-02: sector, page and erase value fixed at 4,096, 256 and FFh, the
# geometry of every part pyro uses; the region stays configured.
E = [
    rep("#define NONE 0xFFFFu\n", "#define NONE 0xFFFFu\n#define PG 256u\n#define SEC 4096u\n#define ERASED 0xFFu\n"),
    rep("""    if (c->erase_value != 0xFFu && c->erase_value != 0x00u)
        return ALFS_EINVAL;
    uint32_t s = c->sector_size, p = c->page_size;
    if (!pow2(s) || !pow2(p) || p < 128u || s < 4u * p || s > 32768u)
        return ALFS_EGEOMETRY;""", """    uint32_t s = SEC;
    if (c->sector_size != SEC || c->page_size != PG || c->erase_value != ERASED)
        return ALFS_EGEOMETRY;"""),
    rep("""    const alfs_config_t *c = fs->cfg;
    return st[5] == lg2(c->sector_size) && st[6] == lg2(c->page_size) && st[7] == 0 &&""",
        "    return st[5] == 12u && st[6] == 8u && st[7] == 0 &&"),
    rep("st[5] = (uint8_t)lg2(fs->cfg->sector_size);", "st[5] = 12u;"),
    rep("st[6] = (uint8_t)lg2(fs->cfg->page_size);", "st[6] = 8u;"),
    rep("cfg->region_size / cfg->sector_size", "cfg->region_size / SEC"),
    rep("fs->cfg->page_size", "PG"),
    rep("fs->cfg->sector_size", "SEC"),
    rep("fs->cfg->erase_value", "ERASED"),
]

# LIFE-06: a cap on the programs one page takes between erases.
F = [
    rep("    uint32_t lim = fs->cfg->max_page_programs ? fs->cfg->max_page_programs : 16u;\n", ""),
    rep("""        if (np - f->off < HDR_LEN + 1u || f->page_progs >= lim) {
            f->off = (uint16_t)np;
            f->page_progs = 0;
            continue;
        }""", """        if (np - f->off < HDR_LEN + 1u) {
            f->off = (uint16_t)np;
            continue;
        }"""),
    rep("uint32_t k = f->sector, at = f->off, pg = fs->cfg->page_size;", "uint32_t k = f->sector, at = f->off;"),
    rep("        f->page_progs = 2; /* the stamp, then the claim */\n", ""),
    rep("        f->page_progs = f->off / pg == at / pg ? (uint8_t)(f->page_progs + 1u) : 0u;\n", ""),
]

# SAFE-11: a deleted file's dead mark programmed again, and each sector
# read again, before its sectors are erased.
P1 = [
    lambda c: cut_between(c, "        for (uint32_t j = 0; j < n; j++) {\n            uint32_t first = 0;",
                          "        return wipe(fs, k, n);"),
    lambda c: cut_func(c, "/*\n * What sector k is now", "static int reclaim_kind"),
]

# SAFE-14: the newest newer version's close record programmed again before
# an older version is deleted.
P2 = [
    lambda c: cut_between(c, "    rc = name_is(fs, first, version, NULL, &key);", "    uint8_t mark = (uint8_t)~"),
    lambda c: cut_func(c, "/* File id's size", "static int size_of") if "static int size_of" in c else c,
]

# SAFE-04 rule 3: the fresh sector, zeroed rather than taken after a mount.
P3 = [
    rep("""            if (g == fs->gen && claim_valid(fs, h->claim) && (!newest_any || get32(h->claim + 4) > newest)) {
                newest = get32(h->claim + 4);
                newest_any = true;
                fresh_after = NONE;
            }
            /* a sector a claim program could have been taking: erased or torn,
             * however its bits read now, and not one alfs zeroed */
            if (g == fs->gen && !claim_valid(fs, h->claim) && !zeroed(fs, h->claim)) {
                if (fresh_any == NONE)
                    fresh_any = k;
                if (fresh_after == NONE && newest_any)
                    fresh_after = k;
                torn |= !is_erased(fs, h->claim, CLAIM_LEN);
            }""", """            if (g == fs->gen && !claim_valid(fs, h->claim))
                torn |= !is_erased(fs, h->claim, CLAIM_LEN);"""),
    rep("""                live_any = newest_any = torn = false;
                fresh_after = fresh_any = NONE;
""", "                live_any = torn = false;\n"),
    rep("    uint32_t newest = 0, fresh_after = NONE, fresh_any = NONE; /* SAFE-04 rule 3 */\n", ""),
    rep("live_any = false, newest_any = false, torn = false;", "live_any = false, torn = false;"),
    rep("    fs->fresh = (uint16_t)(fresh_after != NONE ? fresh_after : fresh_any);\n", ""),
    rep("        if (ok && k != fs->fresh) {", "        if (ok) {"),
    rep("""        fs->dirty++; /* touched (SAFE-05), or the fresh sector (SAFE-04 rule 3) */
        if (ok) {
            fs->fresh = NONE;
            zero_claim(fs, k);
        }""", "        fs->dirty++;"),
    rep("    fs->fresh = NONE;\n", ""),
    lambda c: cut_func(c, "/* Zero over sector k's claim", "static void zero_claim"),
    lambda c: cut_func(c, "/* A claim alfs programmed zero over", "static bool zeroed"),
]

# IF-07: the sequence floor in each stamp, and three past a torn claim.
P4 = [
    rep("        put32(st + 18, fs->next_seq);\n", "        put32(st + 18, 0);\n"),
    rep("""            if (get32(h->stamp + 18) > seq_max)
                seq_max = get32(h->stamp + 18);
""", ""),
    rep("fs->next_seq = seq_max + (torn ? 3u : 2u); /* past a torn claim's: SPEC 1.1 */",
        "fs->next_seq = seq_max + 1u;"),
]

ROWS = [
    ("full", "alfs v3 as it is", []),
    ("A", "LIFE-07 large erase", [A]),
    ("B", "LIFE-05 erase counts kept", [B]),
    ("C", "SAFE-06 retry in a fresh sector, read-back", [C]),
    ("D", "SAFE-14 rebuild a torn close record (refuse instead)", [D]),
    ("E", "IF-02 sector, page, erase value configured", [E]),
    ("F", "LIFE-06 cap on programs a page", [F]),
    ("ABCEF", "A, B, C, E and F: no behaviour the user asked for", [A, B, C, F, E]),
    ("ABCDEF", "all six above", [A, B, C, D, F, E]),
    ("P1", "SAFE-11 dead mark again, re-read before erase", [P1]),
    ("P2", "SAFE-14 close record again before delete", [P2]),
    ("P3", "SAFE-04 rule 3 the fresh sector", [P3]),
    ("P4", "IF-07 sequence floor, three past a torn claim", [P4]),
    ("P", "all four power-loss rules above", [P1, P2, P3, P4]),
    ("ALL", "all six and all four", [A, B, C, F, P1, P2, P3, P4, E]),
]


def build(tag, groups):
    code = SRC
    for g in groups:
        for edit in g:
            code = edit(code)
    c = os.path.join(OUT, f"alfs_{tag}.c")
    o = c[:-2] + ".o"
    open(c, "w").write(code)
    r = subprocess.run(["arm-none-eabi-gcc", "-std=c99", "-Wall", "-Wextra", "-pedantic", "-Wno-unused-function",
                        "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-parameter",
                        "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-I" + ROOT, "-c", c, "-o", o],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{tag}: compile failed\n{r.stderr}")
    t, d = subprocess.run(["arm-none-eabi-size", o], capture_output=True, text=True).stdout.splitlines()[1].split()[:2]
    return c, int(t) + int(d)


def failing(tag, c):
    exe = os.path.join(OUT, f"test_{tag}")
    r = subprocess.run(["cc", "-std=c99", "-O2", "-w", "-I" + ROOT, "-I" + os.path.join(ROOT, "tests"), "-o", exe, c,
                        "tests/test_alfs.c", "tests/sim.c", "tests/tf.c"], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{tag}: host build failed\n{r.stderr}")

    def run(seed):
        p = subprocess.run([exe], env=dict(os.environ, ALFS_SEED=str(seed)), capture_output=True, text=True)
        open(os.path.join(OUT, f"test_{tag}_seed{seed}.log"), "w").write(p.stdout + p.stderr)
        return set(re.findall(r"^FAIL\s+(\S+)", p.stdout, re.M))

    fails = {}
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        for seed, names in zip(SEEDS, ex.map(run, SEEDS)):
            for n in names:
                fails.setdefault(n, []).append(seed)
    return fails


def main():
    os.makedirs(OUT, exist_ok=True)
    test = "--test" in sys.argv
    full = None
    for tag, what, groups in ROWS:
        c, size = build(tag, groups)
        full = size if full is None else full
        line = f"{tag:7s} {what:54s} {size:5d}  saves {full - size:5d}"
        if test:
            fails = failing(tag, c)
            line += "  " + (", ".join(f"{n} ({len(s)}/{len(SEEDS)} seeds)" for n, s in sorted(fails.items()))
                            or f"no test fails on {len(SEEDS)} seeds")
        print(line, flush=True)


main()
