#!/usr/bin/env python3
"""Replay the fixed food-web benchmark against matched predation-off controls."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from functools import partial
from pathlib import Path
import subprocess


def fields(line):
    return dict(field.split("=", 1) for field in line.split())


def lineage_counts(value):
    if value == "-":
        return {}
    return {
        int(founder): int(count)
        for founder, count in (entry.split(":") for entry in value.split(","))
    }


def verify_run(binary, case):
    cohort, seed, predation, observations = case
    command = [
        str(binary), "--food-web", "--seed", str(seed), "--nutrients", "80",
        "--ticks", str(observations[-1]), "--lineage-every", "10000",
    ]
    if predation:
        command.append("--predation")
    output = subprocess.check_output(command, text=True).splitlines()
    summary = fields(output[-1])
    samples = [fields(line) for line in output[:-1]]
    late = [sample for sample in samples if int(sample["sample_tick"]) in observations]
    if len(late) != len(observations) or int(summary["tick"]) != observations[-1]:
        raise ValueError(f"Missing observations for {cohort} seed {seed}")

    recurring = []
    above_founder_range = set()
    for sample in late:
        live = lineage_counts(sample["live_specialists"])
        births = lineage_counts(sample["specialist_births"])
        highest = lineage_counts(sample["specialist_meat_gene_max"])
        generations = lineage_counts(sample["specialist_generation_max"])
        present = {founder for founder in live.keys() & births.keys() if generations[founder] > 0}
        recurring.append(present if int(sample["plant_feeders"]) > 0 else set())
        above_founder_range.update(founder for founder in present if highest[founder] > 11)

    persistent = sorted(set.intersection(*recurring) & above_founder_range)
    return case, summary, persistent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the compiled native petri CLI")
    args = parser.parse_args()
    trials = (
        ("development", (1, 2, 3, 42, 101, 2026), (80000, 90000, 100000)),
        ("validation", tuple(range(4, 16)), (160000, 170000, 180000, 190000, 200000)),
    )
    cases = [
        (cohort, seed, predation, observations)
        for cohort, seeds, observations in trials
        for seed in seeds
        for predation in (False, True)
    ]
    counts = {cohort: 0 for cohort, _, _ in trials}
    with ThreadPoolExecutor(max_workers=4) as pool:
        for case, summary, persistent in pool.map(partial(verify_run, args.binary.resolve()), cases):
            cohort, seed, predation, _ = case
            peak = int(summary["peak_population"])
            blocked = int(summary["capacity_blocks"])
            if peak >= 160 or blocked:
                raise SystemExit(f"Pool pressure invalidated {cohort} seed {seed}")
            if summary["initial_carrion"] != "0":
                raise SystemExit(f"Seeded carrion invalidated {cohort} seed {seed}")
            if not predation and (persistent or int(summary["attacks"])):
                raise SystemExit(f"Unexpected control result for {cohort} seed {seed}")
            if predation and persistent:
                counts[cohort] += 1
            print(f"{cohort:11} seed={seed:4} predation={int(predation)} peak={peak:3} "
                  f"blocked={blocked} lineages={persistent}", flush=True)
    print(f"Qualifying predation runs: {counts}")
    if any(counts[cohort] < 2 for cohort, _, _ in trials):
        raise SystemExit("Meat-specialist coexistence did not replicate")


if __name__ == "__main__":
    main()
