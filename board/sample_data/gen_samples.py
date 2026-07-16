#!/usr/bin/env python3
"""Deterministic sample-dataset generator for NullTorch-Board (SPEC.md §8).

Writes three datasets next to this script, all conforming to
../schema/results.schema.json:

  empty.json        no runs (degenerate)
  one_model.json    1 model x 3 languages x 3 conditions   (9 runs)
  many_models.json  12 models x 3 languages x 2-3 conditions

Stdlib only. Fully deterministic: single random.Random(42), no clocks,
no os.urandom, dict insertion order fixed, json dumped with sort_keys.

Structural validation is built in (validate_results below) and runs on
every dataset before it is written; the script exits nonzero on any
violation. If the `jsonschema` package happens to be installed, the
datasets are additionally validated against the schema file, but that
is a bonus, not a requirement.
"""

import json
import os
import re
import sys
import random
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA_PATH = os.path.join(HERE, "..", "schema", "results.schema.json")

LANGUAGES = ["go", "rust", "cpp"]
CONDITIONS = ["open_book", "closed_book", "delta"]
TIERS = ["T1", "T2", "T3", "T4", "T5", "T6"]

# The RVC engine-fidelity track (docs/RVC_TRACK.md) is NOT a difficulty tier.
# It mirrors pop-maker-studio's pth_reader.cpp end-to-end — config extraction
# + f16->f32 widening — a distinct output contract graded by its own converter.
# It shares the tierResult shape and is reported alongside T1-T6, but:
#   * it has NO delta variant (present only in open_book / closed_book runs), and
#   * it never enters the memorization-gap derivation, which stays T1-T6.
RVC = "RVC"

# Fixture-category composition per tier (names echo SPEC.md §5); RVC categories
# echo docs/RVC_TRACK.md. Values are fixture counts; they sum to the total.
TIER_CATEGORIES = {
    "T1": {"flat_f32": 6, "flat_f16": 6},
    "T2": {"contiguous_stored": 8, "protocol2_graphs": 6},
    "T3": {
        "shared_storage": 4,
        "offset_views": 4,
        "transposed_strides": 4,
        "zero_dim_scalars": 2,
        "zero_size": 2,
    },
    "T4": {
        "wide_dtypes": 4,
        "fp8": 2,
        "qtensor": 3,
        "protocol4": 3,
        "zip64": 2,
        "deflate_rezipped": 3,
        "legacy_inline": 3,
        "module_pickle_skip": 4,
    },
    "T5": {"huge_contiguous": 2, "huge_transposed_view": 2},
    "T6": {
        "truncated": 6,
        "memo_cycles": 3,
        "deep_nesting": 3,
        "pickle_bombs": 3,
        "exec_opcodes": 3,
    },
    RVC: {"config_extraction": 4, "f32_tensors": 4},
}
TIER_TOTALS = {t: sum(c.values()) for t, c in TIER_CATEGORIES.items()}

# How late in the run each tier tends to go green (fraction of wall spent).
TIER_RAMP_START = {"T1": 0.00, "T2": 0.10, "T3": 0.22, "T4": 0.38,
                   "T5": 0.55, "T6": 0.60, RVC: 0.30}

BUDGET = {"wall_seconds": 14400, "tokens": 6_000_000}

MODEL_IDS = [
    "nova-2-pro", "quasar-72b", "citrine-mini", "helios-4", "argon-chat",
    "meridian-1", "peregrine-x", "basalt-9b", "tundra-max", "vermilion-2",
    "kestrel-lite", "onyx-preview",
]

SHA_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
CAT_RE = re.compile(r"^[a-z0-9][a-z0-9_]*$")


def fake_sha256(rng):
    return "sha256:" + "".join(rng.choice("0123456789abcdef") for _ in range(64))


def clamp(x, lo, hi):
    return max(lo, min(hi, x))


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------

def header(rng):
    return {
        "benchmark_version": "1.0.0",
        "fixture_set_hash": fake_sha256(rng),
        "harness_version": "0.4.2",
        "container_digests": {
            "%s-%s" % (lang, cond): fake_sha256(rng)
            for lang in LANGUAGES
            for cond in CONDITIONS
        },
    }


def tier_rates(rng, skill):
    """Per-tier hidden-set pass rates for a stock (open_book) run.

    Target spreads (SPEC §10 calibration): T1 near-saturated ~95%,
    T3/T4 discriminating 30-80%, T5/T6 low 0-40%.
    """
    return {
        "T1": clamp(0.90 + 0.09 * skill + rng.gauss(0, 0.02), 0.80, 1.0),
        "T2": clamp(0.55 + 0.40 * skill + rng.gauss(0, 0.04), 0.30, 1.0),
        "T3": clamp(0.28 + 0.52 * skill + rng.gauss(0, 0.05), 0.10, 0.90),
        "T4": clamp(0.22 + 0.55 * skill + rng.gauss(0, 0.05), 0.05, 0.85),
        "T5": clamp(0.00 + 0.42 * skill + rng.gauss(0, 0.05), 0.00, 0.45),
        "T6": clamp(0.00 + 0.40 * skill + rng.gauss(0, 0.06), 0.00, 0.45),
        # Engine-fidelity: readers that handle f16 + config do well. Moderate-
        # high, skill-correlated, no low cap (docs/RVC_TRACK.md).
        RVC: clamp(0.35 + 0.55 * skill + rng.gauss(0, 0.04), 0.10, 0.95),
    }


def apply_language(rates, language, rng):
    """Small per-language jitter; Go gets a T4 bump (stdlib DEFLATE, SPEC §3).

    RVC gets no Go bump (its stdlib f16->f32 widening is uniform across
    languages) and stays in its moderate-high engine-fidelity band.
    """
    out = {}
    for tier, r in rates.items():
        jitter = rng.gauss(0, 0.02)
        bump = 0.06 if (tier == "T4" and language == "go") else 0.0
        if tier in ("T5", "T6"):
            lo, hi = 0.0, 0.45
        elif tier == RVC:
            lo, hi = 0.10, 0.95
        else:
            lo, hi = 0.0, 1.0
        out[tier] = clamp(r + jitter + bump, lo, hi)
    return out


def category_offset(name):
    """Fixed per-category difficulty offset in [-0.08, +0.08].

    Derived from crc32 (never the built-in str hash, which varies per
    process) so it is identical for stock and delta runs of the same
    cell — category difficulty cancels out of the memorization gap
    instead of drowning it in noise.
    """
    return ((zlib.crc32(name.encode("ascii")) % 17) - 8) / 100.0


def build_tiers(rng, rates, tier_names):
    """Materialize per-category pass counts; tier pass = sum of categories.

    `tier_names` is the tiers this run carries: T1-T6 always, plus RVC for
    non-delta runs.
    """
    tiers = {}
    for tier in tier_names:
        rate = rates[tier]
        cats = {}
        for name, total in TIER_CATEGORIES[tier].items():
            jittered = clamp(rate + category_offset(name) + rng.gauss(0, 0.03), 0.0, 1.0)
            cats[name] = {"pass": int(round(jittered * total)), "total": total}
        tiers[tier] = {
            "pass": sum(c["pass"] for c in cats.values()),
            "total": TIER_TOTALS[tier],
            "categories": cats,
        }
    return tiers


def build_iterations(rng, tiers, wall_spent, tier_names):
    """Monotone iterations-to-green time series ending at the final counts.

    Emits the same tier set the run carries (`tier_names`), so RVC appears in
    the time series exactly when it appears in the run's `tiers`.
    """
    n = rng.randint(5, 12)
    # Strictly increasing sample times ending at wall_spent.
    times = sorted(rng.sample(range(120, max(wall_spent, 240)), n - 1)) + [wall_spent]
    iterations = []
    prev = {t: 0 for t in tier_names}
    for i, t_seconds in enumerate(times):
        frac = t_seconds / float(wall_spent)
        passes = {}
        for tier in tier_names:
            final = tiers[tier]["pass"]
            start = TIER_RAMP_START[tier]
            if frac <= start:
                target = 0
            else:
                ramp = ((frac - start) / (1.0 - start)) ** 0.7
                target = int(round(final * clamp(ramp + rng.gauss(0, 0.05), 0.0, 1.0)))
            if i == len(times) - 1:
                target = final  # last public self-grade == final state
            passes[tier] = max(prev[tier], min(target, final))
        prev = passes
        iterations.append({"t_seconds": t_seconds, "tier_passes": passes})
    return iterations


def build_run(rng, model_id, language, condition, lang_rates, gap_pp, cb_drop):
    """One (model, language, condition) run.

    `lang_rates` are the per-(model, language) stock rates, computed ONCE
    per cell pair by the caller so that conditions of the same cell differ
    only by the intended systematic offsets (cb_drop / gap_pp), not by
    resampled jitter.
    """
    rates = dict(lang_rates)
    if condition == "closed_book":
        rates = {t: clamp(r - cb_drop, 0.0, 1.0) for t, r in rates.items()}
    elif condition == "delta":
        # Memorization gap: delta systematically below stock by gap_pp points.
        rates = {t: clamp(r - gap_pp, 0.0, 1.0) for t, r in rates.items()}

    # RVC (engine-fidelity track) has no delta variant: present only in
    # open_book / closed_book runs, and never folded into the gap (T1-T6).
    tier_names = list(TIERS)
    if condition != "delta":
        tier_names.append(RVC)

    tiers = build_tiers(rng, rates, tier_names)

    skill_proxy = lang_rates["T3"]  # weaker models burn more budget
    wall_frac = clamp(rng.uniform(0.55, 1.0) + (0.5 - skill_proxy) * 0.3, 0.40, 1.0)
    tok_frac = clamp(wall_frac + rng.gauss(0, 0.08), 0.30, 1.0)
    spent = {
        "wall_seconds": int(BUDGET["wall_seconds"] * wall_frac),
        "tokens": int(BUDGET["tokens"] * tok_frac),
    }

    t6_fail = tiers["T6"]["total"] - tiers["T6"]["pass"]
    incidents = {
        "crashes": rng.randint(0, max(0, t6_fail // 4)),
        "hangs": rng.randint(0, max(0, t6_fail // 6)),
        "exec_attempts": rng.randint(0, 1) if t6_fail > 8 else 0,
    }

    attempted_t5 = tiers["T5"]["pass"] > 0 or rng.random() < 0.7
    return {
        "model_id": model_id,
        "language": language,
        "condition": condition,
        "budget": dict(BUDGET),
        "spent": spent,
        "tiers": tiers,
        "iterations": build_iterations(rng, tiers, spent["wall_seconds"], tier_names),
        "determinism_violations": rng.choice([0, 0, 0, 0, 1, 2]) if skill_proxy < 0.5 else 0,
        "t5_peak_rss_bytes": rng.randint(1_500_000_000, 5_600_000_000) if attempted_t5 else 0,
        "t6_incidents": incidents,
    }


def gen_empty(rng):
    doc = header(rng)
    doc["runs"] = []
    return doc


def gen_one_model(rng):
    doc = header(rng)
    model_id = "nova-2-pro"
    stock = tier_rates(rng, skill=0.72)
    gap_pp = rng.uniform(0.05, 0.40)
    cb_drop = rng.uniform(0.02, 0.10)
    runs = []
    for lang in LANGUAGES:
        lang_rates = apply_language(stock, lang, rng)
        for cond in CONDITIONS:
            runs.append(build_run(rng, model_id, lang, cond, lang_rates, gap_pp, cb_drop))
    doc["runs"] = runs
    return doc


def gen_many_models(rng):
    doc = header(rng)
    runs = []
    for i, model_id in enumerate(MODEL_IDS):
        skill = clamp(0.25 + 0.70 * (i / (len(MODEL_IDS) - 1)) + rng.gauss(0, 0.05), 0.10, 0.95)
        stock = tier_rates(rng, skill)
        gap_pp = rng.uniform(0.05, 0.40)  # per-model memorization gap, 5-40pp
        cb_drop = rng.uniform(0.02, 0.10)
        # 2-3 conditions: everyone has open_book; some models lack one condition.
        conditions = list(CONDITIONS)
        if rng.random() < 0.4:
            conditions.remove(rng.choice(["closed_book", "delta"]))
        for lang in LANGUAGES:
            lang_rates = apply_language(stock, lang, rng)
            for cond in conditions:
                runs.append(build_run(rng, model_id, lang, cond, lang_rates, gap_pp, cb_drop))
    doc["runs"] = runs
    return doc


# ---------------------------------------------------------------------------
# Structural validation (mirrors schema/results.schema.json + invariants
# the schema cannot express)
# ---------------------------------------------------------------------------

def validate_results(doc, name):
    def check(cond, msg):
        assert cond, "%s: %s" % (name, msg)

    check(set(doc) == {"benchmark_version", "fixture_set_hash", "harness_version",
                       "container_digests", "runs"}, "bad top-level keys: %s" % sorted(doc))
    check(isinstance(doc["benchmark_version"], str) and doc["benchmark_version"], "benchmark_version")
    check(SHA_RE.match(doc["fixture_set_hash"]), "fixture_set_hash format")
    check(isinstance(doc["harness_version"], str) and doc["harness_version"], "harness_version")
    check(isinstance(doc["container_digests"], dict), "container_digests type")
    for key, digest in doc["container_digests"].items():
        lang, _, cond = key.partition("-")
        check(lang in LANGUAGES and cond in CONDITIONS, "container key %r" % key)
        check(SHA_RE.match(digest), "container digest %r" % key)
    check(isinstance(doc["runs"], list), "runs type")

    seen_cells = set()
    for i, run in enumerate(doc["runs"]):
        where = "runs[%d]" % i
        check(set(run) == {"model_id", "language", "condition", "budget", "spent",
                           "tiers", "iterations", "determinism_violations",
                           "t5_peak_rss_bytes", "t6_incidents"},
              "%s keys" % where)
        check(isinstance(run["model_id"], str) and run["model_id"], "%s model_id" % where)
        check(run["language"] in LANGUAGES, "%s language" % where)
        check(run["condition"] in CONDITIONS, "%s condition" % where)
        cell = (run["model_id"], run["language"], run["condition"])
        check(cell not in seen_cells, "%s duplicate cell %r" % (where, cell))
        seen_cells.add(cell)

        for env_name in ("budget", "spent"):
            env = run[env_name]
            check(set(env) == {"wall_seconds", "tokens"}, "%s %s keys" % (where, env_name))
            for k, v in env.items():
                check(isinstance(v, int) and v >= 0, "%s %s.%s" % (where, env_name, k))
        check(run["spent"]["wall_seconds"] <= run["budget"]["wall_seconds"],
              "%s wall spent > budget" % where)
        check(run["spent"]["tokens"] <= run["budget"]["tokens"],
              "%s tokens spent > budget" % where)

        # T1-T6 always; RVC present iff the run is non-delta (engine-fidelity
        # track has no delta variant — docs/RVC_TRACK.md).
        expected_tiers = set(TIERS)
        if run["condition"] != "delta":
            expected_tiers.add(RVC)
        check(set(run["tiers"]) == expected_tiers,
              "%s tiers keys (RVC iff non-delta): got %s" % (where, sorted(run["tiers"])))
        run_tiers = set(run["tiers"])
        for tier, tr in run["tiers"].items():
            tw = "%s.tiers.%s" % (where, tier)
            check(set(tr) == {"pass", "total", "categories"}, "%s keys" % tw)
            check(isinstance(tr["pass"], int) and isinstance(tr["total"], int), "%s int" % tw)
            check(0 <= tr["pass"] <= tr["total"], "%s pass<=total" % tw)
            cat_pass = cat_total = 0
            for cname, c in tr["categories"].items():
                check(CAT_RE.match(cname), "%s category name %r" % (tw, cname))
                check(set(c) == {"pass", "total"}, "%s.%s keys" % (tw, cname))
                check(isinstance(c["pass"], int) and isinstance(c["total"], int),
                      "%s.%s int" % (tw, cname))
                check(0 <= c["pass"] <= c["total"] and c["total"] >= 1,
                      "%s.%s pass<=total" % (tw, cname))
                cat_pass += c["pass"]
                cat_total += c["total"]
            check(cat_pass == tr["pass"], "%s category passes sum != tier pass" % tw)
            check(cat_total == tr["total"], "%s category totals sum != tier total" % tw)

        prev_t = -1
        prev_passes = {t: 0 for t in run_tiers}
        for j, it in enumerate(run["iterations"]):
            iw = "%s.iterations[%d]" % (where, j)
            check(set(it) == {"t_seconds", "tier_passes"}, "%s keys" % iw)
            check(isinstance(it["t_seconds"], int) and it["t_seconds"] >= 0, "%s t_seconds" % iw)
            check(it["t_seconds"] > prev_t, "%s t_seconds not increasing" % iw)
            prev_t = it["t_seconds"]
            # tier_passes carries the same tier set as the run (RVC iff non-delta).
            check(set(it["tier_passes"]) == run_tiers, "%s tier_passes keys" % iw)
            for tier, p in it["tier_passes"].items():
                check(isinstance(p, int) and p >= 0, "%s %s" % (iw, tier))
                check(p >= prev_passes[tier], "%s %s decreased" % (iw, tier))
                check(p <= run["tiers"][tier]["total"], "%s %s > tier total" % (iw, tier))
            prev_passes = it["tier_passes"]
        if run["iterations"]:
            last = run["iterations"][-1]
            check(last["t_seconds"] <= run["spent"]["wall_seconds"],
                  "%s last iteration after wall spent" % where)
            for tier in run_tiers:
                check(last["tier_passes"][tier] == run["tiers"][tier]["pass"],
                      "%s final iteration %s != tier pass" % (where, tier))

        check(isinstance(run["determinism_violations"], int)
              and run["determinism_violations"] >= 0, "%s determinism_violations" % where)
        check(isinstance(run["t5_peak_rss_bytes"], int)
              and run["t5_peak_rss_bytes"] >= 0, "%s t5_peak_rss_bytes" % where)
        inc = run["t6_incidents"]
        check(set(inc) == {"crashes", "hangs", "exec_attempts"}, "%s t6_incidents keys" % where)
        for k, v in inc.items():
            check(isinstance(v, int) and v >= 0, "%s t6_incidents.%s" % (where, k))


def maybe_jsonschema_validate(doc, name):
    try:
        import jsonschema  # optional; structural asserts above are authoritative
    except ImportError:
        return False
    with open(SCHEMA_PATH) as f:
        schema = json.load(f)
    jsonschema.validate(instance=doc, schema=schema)
    return True


def main():
    rng = random.Random(42)
    datasets = [
        ("empty.json", gen_empty),
        ("one_model.json", gen_one_model),
        ("many_models.json", gen_many_models),
    ]
    used_jsonschema = False
    for filename, gen in datasets:
        doc = gen(rng)
        validate_results(doc, filename)
        used_jsonschema = maybe_jsonschema_validate(doc, filename) or used_jsonschema
        path = os.path.join(HERE, filename)
        with open(path, "w") as f:
            json.dump(doc, f, indent=2, sort_keys=True)
            f.write("\n")
        print("wrote %-17s %5d runs  (structural asserts OK%s)" % (
            filename, len(doc["runs"]),
            ", jsonschema OK" if used_jsonschema else ""))
    if not used_jsonschema:
        print("note: `jsonschema` not installed; skipped schema-file validation "
              "(structural asserts cover the same invariants and more)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
