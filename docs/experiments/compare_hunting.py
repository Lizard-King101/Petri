#!/usr/bin/env python3

import argparse
import csv
from concurrent.futures import ThreadPoolExecutor
from itertools import product
from pathlib import Path
import subprocess


TREATMENTS = {
    "baseline": (),
    "passive": ("--adaptive-hunts", "--passive-prey"),
    "evasion": ("--adaptive-hunts",),
}
ENERGY_TREATMENTS = {
    "reference": ("--adaptive-hunts",),
    "threshold_30": ("--adaptive-hunts", "--hunt-threshold", "30"),
    "threshold_60": ("--adaptive-hunts", "--hunt-threshold", "60"),
    "threshold_80": ("--adaptive-hunts", "--hunt-threshold", "80"),
    "chase_0": ("--adaptive-hunts", "--chase-cost", "0"),
    "chase_1": ("--adaptive-hunts", "--chase-cost", "1"),
    "chase_4": ("--adaptive-hunts", "--chase-cost", "4"),
}
KIN_TREATMENTS = {
    "control": ENERGY_TREATMENTS["chase_1"],
    "kin_avoidance": ENERGY_TREATMENTS["chase_1"] + ("--kin-avoidance",),
}
KIN_STRENGTHS = (0, 2, 7, 14)
KIN_SWEEP = {
    f"kin_{kin}_resemblance_{resemblance}": KIN_TREATMENTS["kin_avoidance"] + (
        "--kin-strength", str(kin), "--resemblance-strength", str(resemblance))
    for kin, resemblance in product(KIN_STRENGTHS, repeat=2)
}
KIN_SWEEP["kin_0_resemblance_0"] = KIN_TREATMENTS["control"]
KIN_SWEEP.update({
    f"protect_similar_kin_{kin}": KIN_TREATMENTS["kin_avoidance"] + (
        "--kin-strength", str(kin), "--resemblance-strength", "0", "--protect-similar-prey")
    for kin in (0, 14)
})
FACTION_TREATMENTS = {
    "marked_control": ENERGY_TREATMENTS["chase_1"] + ("--faction-markers",),
    "cross_faction": ENERGY_TREATMENTS["chase_1"] + ("--cross-faction-hunts",),
}
SEEDS = (5, 6, 7, 8, 9, 10, 11, 12)
TOTAL_TICKS = 100_000
INTERVAL = 10_000
DECISIONS = ("sated", "low_energy", "no_prey", "blocked", "forage_or_rest",
             "strike", "pursue", "flee", "cover", "kin_reluctance", "faction_protected")
RELATIONSHIP_FIELDS = ("same_founder_kills", "close_kin_kills", "parent_offspring_kills", "similar_prey_kills")
ENERGY_FIELDS = ("chase_energy", "strike_energy", "hunt_upkeep", "prey_reserve",
                 "kill_meal_energy", "kill_cap_loss", "other_meal_energy", "hunt_cost", "hunt_return")


def run(binary, world, seed, treatment, options):
    nutrients = 24 if world == "wide" else 80
    command = [str(binary), "--seed", str(seed), "--nutrients", str(nutrients),
               "--food-web", "--predation", "--ticks", str(TOTAL_TICKS),
               "--lineage-every", str(INTERVAL), "--hunt-every", str(INTERVAL)]
    if world == "wide":
        command.extend(("--connected-biomes", "--irregular-terrain", "--sexual",
                        "--nests", "--guarding", "--provisioning"))
    output = subprocess.check_output(command + list(options), text=True)
    records = [dict(word.split("=", 1) for word in line.split()) for line in output.splitlines()]
    state = records[-1]
    samples = {int(row["sample_tick"]): row for row in records if "sample_tick" in row}
    hunts = {int(row["hunt_tick"]): row for row in records if "hunt_tick" in row}
    ticks = set(range(INTERVAL, TOTAL_TICKS + 1, INTERVAL))
    if samples.keys() != ticks or hunts.keys() != ticks:
        raise ValueError(f"Incomplete samples for {world} seed {seed} in {treatment}")
    if (sum(int(row["kills"]) for row in hunts.values()) != int(state["kills"]) or
            sum(int(row["strike"]) for row in hunts.values()) != int(state["attacks"])):
        raise ValueError(f"Hunt counters disagreed for {world} seed {seed} in {treatment}")

    rows = []
    for tick in sorted(ticks):
        sample, hunt = samples[tick], hunts[tick]
        row = {"world": world, "nutrients": nutrients, "seed": seed, "treatment": treatment,
               "hunt_threshold": state.get("hunt_threshold", ""), "chase_cost": state.get("chase_cost", ""),
               "kin_strength": int(state.get("kin_strength", 0)),
               "resemblance_strength": int(state.get("resemblance_strength", 0)),
               "protect_similar_prey": int(state.get("protect_similar_prey", 0)),
               "faction_markers": int(state.get("faction_markers", 0)),
               "cross_faction_hunts": int(state.get("cross_faction_hunts", 0)),
               "tick": tick}
        for key in ("population", "capacity_blocks", "live_hunters", "plant_feeders",
                    "hunter_births", "grazer_births"):
            row[key] = int(sample[key])
        row.update(window_kills=int(hunt["kills"]), peak_population=int(state["peak_population"]),
                   end_population=int(state["population"]))
        row.update((key, int(hunt[key])) for key in DECISIONS)
        row.update((key, int(hunt[key])) for key in RELATIONSHIP_FIELDS)
        if (max(row["same_founder_kills"], row["similar_prey_kills"]) > row["window_kills"] or
                not row["parent_offspring_kills"] <= row["close_kin_kills"] <= row["window_kills"]):
            raise ValueError(f"Prey relationship counters disagreed for {world} seed {seed} in {treatment} at {tick}")
        if row["protect_similar_prey"] and row["similar_prey_kills"]:
            raise ValueError(f"Protected prey killed for {world} seed {seed} in {treatment} at {tick}")
        row.update(same_faction_kills=int(hunt.get("same_faction_kills", 0)),
                   cross_faction_kills=int(hunt.get("cross_faction_kills", 0)))
        faction_population = sample.get("faction_population", "-")
        sizes = []
        if faction_population != "-":
            sizes = [int(count.split(":", 1)[1]) for count in faction_population.split(",")]
        row.update(faction_population=faction_population, live_factions=len(sizes), largest_faction=max(sizes, default=0))
        if (sum(sizes) != row["population"] * row["faction_markers"] or
                row["same_faction_kills"] + row["cross_faction_kills"] != row["window_kills"] * row["faction_markers"]):
            raise ValueError(f"Faction counts disagreed for {world} seed {seed} in {treatment} at {tick}")
        if row["cross_faction_hunts"] and row["same_faction_kills"]:
            raise ValueError(f"Same-faction prey killed for {world} seed {seed} in {treatment} at {tick}")
        row.update((key, int(hunt[key])) for key in ENERGY_FIELDS)
        if (row["hunt_cost"] != row["chase_energy"] + row["strike_energy"] or
                row["hunt_return"] != row["prey_reserve"] + row["kill_meal_energy"] - row["kill_cap_loss"]):
            raise ValueError(f"Hunt energy did not balance for {world} seed {seed} in {treatment} at {tick}")
        row["net_hunt_energy"] = row["hunt_return"] - row["hunt_cost"]
        row["net_hunt_turn_energy"] = row["net_hunt_energy"] - row["hunt_upkeep"] + row["other_meal_energy"]
        rows.append(row)
    return rows


def summarize(results, treatments):
    for treatment in treatments:
        runs = [rows for rows in results if rows[0]["treatment"] == treatment]
        coexistence = []
        for rows in runs:
            if rows[-1]["capacity_blocks"] == 0 and all(
                    row["live_hunters"] and row["plant_feeders"] and row["hunter_births"]
                    and row["grazer_births"] for row in rows[-5:]):
                coexistence.append(rows[0]["seed"])
        windows = [window for rows in runs for window in rows]
        count = lambda key: sum(row[key] for row in windows)
        extinct = [rows[0]["seed"] for rows in runs if rows[-1]["end_population"] == 0]
        full = [rows[0]["seed"] for rows in runs if rows[-1]["capacity_blocks"]]
        print(f"treatment={treatment} coexistence={coexistence} extinct={extinct} blocked_births={full} "
              f"kills={count('window_kills')} retreats={count('flee')} cover_moves={count('cover')} "
              f"close_kin_kills={count('close_kin_kills')} same_founder_kills={count('same_founder_kills')} "
              f"similar_prey_kills={count('similar_prey_kills')} "
              f"same_faction_kills={count('same_faction_kills')} cross_faction_kills={count('cross_faction_kills')} "
              f"faction_protected={count('faction_protected')} "
              f"late_hunter_births={sum(row['hunter_births'] for rows in runs for row in rows[-5:])} "
              f"kin_reluctance={count('kin_reluctance')} "
              f"peak={max(rows[-1]['peak_population'] for rows in runs)} "
              f"chase_energy={count('chase_energy')} strike_energy={count('strike_energy')} "
              f"hunt_return={count('hunt_return')} net_hunt_energy={count('net_hunt_energy')}")


def main():
    parser = argparse.ArgumentParser(description="Compare inherited hunting and evasion across matched seeds.")
    parser.add_argument("binary", type=Path, help="Path to the corresponding petri binary")
    parser.add_argument("--world", choices=("wide", "compact"), default="wide")
    study = parser.add_mutually_exclusive_group()
    study.add_argument("--energy-sweep", action="store_true", help="Vary hunt threshold and chase cost separately")
    study.add_argument("--kin-test", action="store_true", help="Compare kin avoidance on seed 10 with chase cost 1")
    study.add_argument("--kin-sweep", action="store_true", help="Sweep both strengths and test strict prey protection")
    study.add_argument("--faction-test", action="store_true", help="Compare marked worlds with and without same-faction protection")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    treatments, seeds = TREATMENTS, SEEDS
    if args.energy_sweep:
        treatments = ENERGY_TREATMENTS
    elif args.kin_test:
        treatments, seeds = KIN_TREATMENTS, (10,)
    elif args.kin_sweep:
        treatments = KIN_SWEEP
    elif args.faction_test:
        treatments = FACTION_TREATMENTS
    with ThreadPoolExecutor(max_workers=4) as pool:
        jobs = [pool.submit(run, binary, args.world, seed, treatment, treatments[treatment])
                for seed, treatment in product(seeds, treatments)]
        results = [job.result() for job in jobs]
    with args.output.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(results[0][0]))
        writer.writeheader()
        for rows in results:
            writer.writerows(rows)
    summarize(results, treatments)


if __name__ == "__main__":
    main()
