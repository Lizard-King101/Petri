#!/usr/bin/env python3
"""Matched-seed test of whether morphology genes are selected.

Each seed runs twice with the same terrain, placements, founders, and
ecological random stream: once with `--morphology` (the genes act) and once
with `--morphology --neutral-morphology` (the genes are inherited and mutate
but do nothing). A trait is selected when the treatment moves it consistently
away from the control across seeds.

Usage:
  compare_morphology.py BUILD/petri --world compact|compact-no-predation|wide [--ticks N] [--seeds 5-12]
                        [--output runs.csv]
"""

import argparse
import concurrent.futures
import csv
import os
import statistics
import subprocess
import sys

ECOLOGIES = {
    # The 64x32 food web with adaptive hunting, where predation is frequent.
    "compact": ["--nutrients", "80", "--food-web", "--predation", "--adaptive-hunts"],
    # The same food web without predation, where camouflage cannot pay.
    "compact-no-predation": ["--nutrients", "80", "--food-web"],
    # The observatory world that the board runs.
    "wide": ["--nutrients", "24", "--predation", "--food-web", "--connected-biomes", "--irregular-terrain",
             "--faction-markers", "--fruiting-plants", "--dynamic-environment"],
}

TRAITS = ["mean_body_size", "mean_pigment", "mean_pattern", "mean_limbs", "mean_pigment_mismatch"]
COUNTS = ["population", "peak_population", "births", "kills", "capacity_blocks", "pigment_matched"]


def seeds(text):
    first, _, last = text.partition("-")
    return list(range(int(first), int(last or first) + 1))


def run(binary, flags, seed, ticks, treatment):
    command = [binary, "--seed", str(seed), "--ticks", str(ticks), *flags, "--morphology"]
    if treatment == "neutral":
        command.append("--neutral-morphology")
    output = subprocess.run(command, check=True, capture_output=True, text=True).stdout.split("\n")[-2]
    fields = dict(item.split("=", 1) for item in output.split() if "=" in item)
    return {"seed": seed, "treatment": treatment, **{key: fields.get(key, "NA") for key in COUNTS + TRAITS}}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary")
    parser.add_argument("--world", choices=sorted(ECOLOGIES), default="compact")
    parser.add_argument("--ticks", type=int, default=100000)
    parser.add_argument("--seeds", type=seeds, default=seeds("5-12"))
    parser.add_argument("--output")
    args = parser.parse_args()

    jobs = [(seed, treatment) for seed in args.seeds for treatment in ("morphology", "neutral")]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        rows = list(pool.map(lambda job: run(args.binary, ECOLOGIES[args.world], job[0], args.ticks, job[1]), jobs))
    if args.output:
        with open(args.output, "w", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    by = {(row["seed"], row["treatment"]): row for row in rows}
    print(f"world={args.world} ticks={args.ticks} seeds={args.seeds[0]}-{args.seeds[-1]}")
    for key in COUNTS + TRAITS:
        cells = []
        differences = []
        for seed in args.seeds:
            treated, neutral = by[(seed, "morphology")][key], by[(seed, "neutral")][key]
            cells.append(f"{treated}/{neutral}")
            if "NA" not in (treated, neutral):
                differences.append(float(treated) - float(neutral))
        if not differences:
            print(f"{key:24} no living populations")
            continue
        above = sum(d > 0 for d in differences)
        below = sum(d < 0 for d in differences)
        print(f"{key:24} mean difference {statistics.mean(differences):+8.3f}  "
              f"higher in {above}/{len(differences)}, lower in {below}/{len(differences)}")
    survived = sum(by[(seed, "morphology")]["population"] not in ("0", "NA") for seed in args.seeds)
    print(f"treatment populations alive at the end: {survived}/{len(args.seeds)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
