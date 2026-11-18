#include "life.hpp"
#include "world.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void lifecycle() {
    petri::World world(42);
    unsigned mutated_births = 0;
    for (int tick = 0; tick < 1000; ++tick) {
        const auto previous = world.creatures();
        const auto before = world.metrics();
        world.step();

        std::array<bool, petri::width * petri::height> occupied{};
        unsigned counted = 0;
        for (const auto& creature : world.creatures()) {
            if (!creature.alive()) {
                continue;
            }
            ++counted;
            check(creature.cell < occupied.size(), "Creature left the world");
            check(!occupied[creature.cell], "Two creatures share a cell");
            occupied[creature.cell] = true;
            check(creature.energy > 0 && creature.energy <= 160, "Invalid energy");
            for (const auto trait : {creature.genes.digestion, creature.genes.thrift}) {
                check(trait >= petri::Genome::minimum && trait <= petri::Genome::maximum,
                      "Trait escaped mutation bounds");
            }
            if (creature.id > 32 + before.births) {
                bool found = false;
                for (const auto& parent : previous) {
                    if (parent.id != creature.parent) {
                        continue;
                    }
                    found = true;
                    check(creature.founder == parent.founder &&
                          creature.generation == parent.generation + 1, "Broken ancestry");
                    const int digestion = creature.genes.digestion - parent.genes.digestion;
                    const int thrift = creature.genes.thrift - parent.genes.thrift;
                    check(std::abs(digestion) <= 1 && std::abs(thrift) <= 1, "Broken inheritance");
                    mutated_births += digestion != 0 || thrift != 0;
                    check(creature.energy == 40, "Newborn acted in its birth tick");
                    for (const auto& current : world.creatures()) {
                        if (current.id == parent.id) {
                            check(current.energy <= int(parent.energy) + 13 - 50,
                                  "Reproduction failed to debit energy");
                        }
                    }
                    break;
                }
                check(found, "Newborn has no parent from the preceding tick");
            }
        }
        for (const auto& cell : world.cells()) {
            check(cell.plants + cell.nutrients + cell.detritus == 48, "Nutrients were lost or created");
        }
        const auto& metrics = world.metrics();
        check(counted == metrics.population &&
              counted + metrics.deaths == 32 + metrics.births, "Population counter diverged");
    }
    check(mutated_births > 0, "No mutated offspring observed");
    check(world.metrics().deaths > 0, "No deaths observed");
}

void carrying_capacity() {
    for (const auto nutrients : {std::uint8_t{7}, std::uint8_t{48}, std::uint8_t{255}}) {
        petri::World world(101, {nutrients});
        std::uint64_t midrun_births = 0;
        for (int tick = 0; tick < 20000; ++tick) {
            world.step();
            check(world.metrics().population < petri::capacity, "Population reached pool capacity");
            if (tick == 9999) {
                midrun_births = world.metrics().births;
            }
        }
        check(world.metrics().capacity_blocks == 0, "A birth was blocked by pool capacity");
        check(world.metrics().births > midrun_births, "Population stopped reproducing");
    }
}

void replay() {
    petri::World first(0);
    petri::World second(0);
    petri::World scarce(0, {0});
    check(first.fingerprint() != petri::World(1).fingerprint(), "Seed had no effect");
    for (int batch = 0; batch < 125; ++batch) {
        for (int tick = 0; tick < 8; ++tick) {
            first.step();
            scarce.step();
        }
        for (int tick = 0; tick < 8; ++tick) {
            second.step();
        }
        check(first.fingerprint() == second.fingerprint(), "Same-seed replay diverged");
    }
    check(scarce.metrics().births == 0 && scarce.metrics().population == 0,
          "A world without food sustained creatures");
}

std::uint64_t total_material(const petri::World& world) {
    std::uint64_t total = world.metrics().population * petri::body_mass;
    for (std::size_t index = 0; index < world.cells().size(); ++index) {
        const auto& cell = world.cells()[index];
        total += cell.nutrients + cell.plants + cell.animal + cell.detritus + world.fruit_at(index);
    }
    for (const auto& creature : world.creatures()) {
        if (creature.alive()) {
            total += creature.nutrients_carried + creature.provisions();
        }
    }
    return total;
}

void carcasses_nourish_plants() {
    petri::World decomposing(42, {0, true, 1});
    petri::World control(42, {0});
    const auto budget = total_material(decomposing);
    bool produced_carrion = false;
    for (int tick = 0; tick < 512; ++tick) {
        decomposing.step();
        control.step();
        check(total_material(decomposing) == budget, "Decomposition changed the nutrient budget");
        produced_carrion |= std::any_of(decomposing.cells().begin(), decomposing.cells().end(),
            [](const auto& cell) { return cell.animal > 0; });
    }
    const auto has_plants = [](const auto& world) {
        return std::any_of(world.cells().begin(), world.cells().end(),
            [](const auto& cell) { return cell.plants > 0; });
    };
    check(produced_carrion && decomposing.metrics().population == 0 && has_plants(decomposing),
          "Founder carcasses failed to grow vegetation in soil that started with zero nutrients");
    check(!has_plants(control), "Vegetation grew in the zero-nutrient control");
}

void sexual_reproduction() {
    petri::World world(42, {48, true, 1, 0, true, true, false, false, true});
    check(world.sexual_reproduction(), "Two-parent mode was not enabled");
    struct Births {
        const petri::World& world;
        unsigned count = 0;
        bool from_second_parent = false;
    } births{world};
    const petri::StepObserver observer{&births,
        [](void* context, const petri::Creature& parent, const petri::Creature& child) {
            auto& events = *static_cast<Births*>(context);
            const auto& creatures = events.world.creatures();
            auto mate = std::find_if(creatures.begin(), creatures.end(),
                [&](const auto& candidate) { return candidate.id == child.other_parent; });
            check(mate != creatures.end() && mate->alive() && mate->id != parent.id &&
                  child.parent == parent.id && child.generation == std::max(parent.generation, mate->generation) + 1,
                  "Invalid two-parent pedigree");
            check(parent.energy <= 135 && mate->energy <= 135 && parent.nutrients_carried <= 4 &&
                  mate->nutrients_carried <= 4 && parent.breeding_cooldown == petri::Creature::recovery_ticks &&
                  mate->breeding_cooldown == petri::Creature::recovery_ticks,
                  "Both parents must pay for the birth and enter recovery");
            check(child.energy == 40 && child.breeding_cooldown == petri::Creature::maturity_ticks,
                  "Offspring began with incorrect energy or maturity");
            for (const auto gene : {&petri::Genome::digestion, &petri::Genome::thrift,
                                    &petri::Genome::mate_drive, &petri::Genome::receptivity}) {
                const int allele = child.genes.*gene;
                const bool primary = std::abs(allele - parent.genes.*gene) <= 1;
                const bool secondary = std::abs(allele - mate->genes.*gene) <= 1;
                check(primary || secondary, "An offspring allele came from neither parent");
                events.from_second_parent |= !primary && secondary;
            }
            ++events.count;
        }};
    const auto budget = total_material(world);
    for (int tick = 0; tick < 2000; ++tick) {
        world.step(observer);
        check(total_material(world) == budget, "Two-parent reproduction changed the nutrient budget");
    }
    check(births.count == world.metrics().births && births.count > 0 && births.from_second_parent,
          "Expected observed births with alleles from both parents");
}

void nest_care() {
    for (unsigned care = 0; care < 4; ++care) {
        petri::World world(42, {48, true, 1, 0, true, true, false, false, true, true,
                               bool(care & 1), bool(care & 2)});
        const auto budget = total_material(world);
        bool carried = false;
        for (int tick = 0; tick < 2500; ++tick) {
            const auto before = world.creatures();
            world.step();
            check(total_material(world) == budget, "Carrying food lost or created material");
            unsigned active = 0;
            for (std::size_t slot = 0; slot < before.size(); ++slot) {
                const auto& young = world.creatures()[slot];
                if (young.id == before[slot].id && young.nest_ticks) {
                    check(young.cell == before[slot].cell && young.nest_ticks + 1 == before[slot].nest_ticks,
                          "Dependent young moved or aged incorrectly in the nest");
                }
                active += young.nest_ticks > 0;
                carried |= young.packed_plants || young.packed_animal;
            }
            const auto& report = world.metrics().nests;
            check(active + report.fledged + report.starved + report.killed == world.metrics().births,
                  "Nest outcomes did not account for every birth");
            check(report.repeated_mates <= report.previous_mate_alive &&
                  report.previous_mate_alive <= report.subsequent_matings && report.both_near <= report.both_alive &&
                  report.after_nest_near <= report.after_nest_alive,
                  "Pair observations could not be reconciled");
        }
        const auto& report = world.metrics().nests;
        check(report.fledged > 0 && report.starved + report.killed > 0,
              "Young did not survive and die through the vulnerable stage");
        check((report.guard_ticks > 0) == bool(care & 1) && (report.deliveries > 0) == bool(care & 2) &&
              carried == bool(care & 2), "Care actions did not match their experimental switch");
    }
}

void adaptive_hunting() {
    petri::Config config{80, true, 1, 0, true, true, false, false, true, true, true, true, true, false};
    petri::World active(42, config);
    auto control = config;
    control.passive_prey = true;
    petri::World passive(42, control);
    control.adaptive_hunts = control.passive_prey = false;
    const petri::World baseline(42, control);
    for (std::size_t i = 0; i < active.cells().size(); ++i) {
        const auto& left = active.cells()[i];
        const auto& right = baseline.cells()[i];
        check(left.moisture == right.moisture && left.plants == right.plants && left.nutrients == right.nutrients,
              "Adaptive hunts changed the seeded environment");
    }
    for (std::size_t i = 0; i < petri::founder_count; ++i) {
        const auto& left = active.creatures()[i];
        const auto& right = passive.creatures()[i];
        check(left.cell == baseline.creatures()[i].cell && left.genes.digestion == baseline.creatures()[i].genes.digestion &&
              left.genes.hunting == right.genes.hunting && left.genes.evasion == right.genes.evasion,
              "The hunting treatments changed the matched founders");
    }

    struct Events {
        const petri::World& world;
        std::array<unsigned, static_cast<unsigned>(petri::HuntDecision::count)> choices{};
        unsigned hunts = 0, births = 0, energy_events = 0, kill_pairs = 0;
        unsigned inherited_mate_marker = 0, new_markers = 0;
    } active_events{active}, passive_events{passive};
    petri::StepObserver observer{&active_events,
        [](void* context, const petri::Creature& parent, const petri::Creature& child) {
            auto& events = *static_cast<Events*>(context);
            const auto& creatures = events.world.creatures();
            const auto mate = std::find_if(creatures.begin(), creatures.end(), [&](const auto& candidate) {
                return candidate.id == child.other_parent;
            });
            check(mate != creatures.end(), "The hunting test saw a birth without two parents");
            for (const auto gene : {&petri::Genome::hunting, &petri::Genome::evasion}) {
                const int allele = child.genes.*gene;
                check(allele >= petri::Genome::minimum && allele <= petri::Genome::maximum &&
                      (std::abs(allele - parent.genes.*gene) <= 1 || std::abs(allele - mate->genes.*gene) <= 1),
                      "A hunting or evasion allele failed bounded inheritance");
            }
            const int reluctance = child.genes.kin_reluctance;
            if (events.world.kin_avoidance()) {
                check(reluctance >= petri::Genome::minimum && reluctance <= petri::Genome::maximum &&
                      (std::abs(reluctance - parent.genes.kin_reluctance) <= 1 ||
                       std::abs(reluctance - mate->genes.kin_reluctance) <= 1),
                      "Reluctance failed bounded inheritance");
            } else {
                check(reluctance == 0, "The control inherited the new trait");
            }
            const auto faction = child.genes.faction;
            if (events.world.faction_markers()) {
                check(faction >= 1 && faction <= petri::faction_count, "Offspring have an invalid faction marker");
                events.inherited_mate_marker += faction != parent.genes.faction && faction == mate->genes.faction;
                events.new_markers += faction != parent.genes.faction && faction != mate->genes.faction;
            } else {
                check(faction == 0, "An unmarked run produced a faction marker");
            }
            ++events.births;
        },
        [](void* context, std::uint16_t hunter, std::uint16_t prey) {
            check(std::abs(int(hunter % petri::width) - int(prey % petri::width)) +
                  std::abs(int(hunter / petri::width) - int(prey / petri::width)) == 1,
                  "A strike landed outside reach");
            ++static_cast<Events*>(context)->hunts;
        }, nullptr,
        [](void* context, petri::HuntDecision decision) {
            ++static_cast<Events*>(context)->choices[static_cast<unsigned>(decision)];
        },
        [](void* context, const petri::HuntEnergy& action) {
            auto& events = *static_cast<Events*>(context);
            const auto chase_cost = events.world.hunt_tuning().chase_cost;
            check(action.strike == 0 || action.strike == 3, "The reported strike cost is incorrect");
            check(action.chase == 0 || action.chase == chase_cost ||
                  (!action.strike && action.chase == 2u * chase_cost), "The reported chase cost is incorrect");
            check(action.kill_cap_loss <= action.prey_reserve + action.kill_meal && !action.other_meal,
                  "The adaptive hunt included unearned energy");
            check(action.strike || (!action.prey_reserve && !action.kill_meal), "A chase was credited with a kill meal");
            ++events.energy_events;
        },
        [](void* context, const petri::Creature& hunter, const petri::Creature& prey) {
            check(hunter.alive() && prey.alive() && hunter.id != prey.id, "The kill event lost its participants");
            auto& events = *static_cast<Events*>(context);
            if (events.world.kin_tuning().protect_similar_prey) {
                check(hunter.genes.prey_resemblance(prey.genes) == 0, "A protected creature was killed");
            }
            if (events.world.cross_faction_hunts()) {
                check(hunter.genes.faction != prey.genes.faction, "A hunter killed prey from its own faction");
            }
            ++events.kill_pairs;
        }};

    const auto budget = total_material(active);
    bool replayed_chase = false;
    for (int tick = 0; tick < 3000; ++tick) {
        observer.context = &active_events;
        active.step(observer);
        observer.context = &passive_events;
        passive.step(observer);
        check(total_material(active) == budget && total_material(passive) == budget,
              "Adaptive hunting changed the total material budget");
        std::array<bool, petri::width * petri::height> occupied{};
        for (const auto& living : active.creatures()) {
            if (living.alive()) {
                check(living.cell < occupied.size() && !occupied[living.cell], "Evasion corrupted occupancy");
                occupied[living.cell] = true;
            }
        }
        if (!replayed_chase && std::any_of(active.creatures().begin(), active.creatures().end(),
                [](const auto& creature) { return creature.hunt_target != 0; })) {
            petri::Checkpoint checkpoint{};
            active.save_checkpoint(checkpoint);
            petri::World restored(0);
            check(restored.load_checkpoint(checkpoint), "A checkpoint with a chase was rejected");
            auto original = active;
            for (int replay = 0; replay < 32; ++replay) {
                original.step();
                restored.step();
                check(restored.fingerprint() == original.fingerprint(), "Replaying a chase changed the outcome");
            }
            replayed_chase = true;
        }
    }
    const auto count = [](const Events& events, petri::HuntDecision type) {
        return events.choices[static_cast<unsigned>(type)];
    };
    check(replayed_chase && active_events.hunts == active.metrics().kills && active_events.hunts > 0 &&
          active_events.births == active.metrics().births && active_events.births > 0,
          "The adaptive hunting test did not encounter successful kills, births and a replayable chase");
    check(count(active_events, petri::HuntDecision::strike) == active.metrics().attacks &&
          count(active_events, petri::HuntDecision::sated) && count(active_events, petri::HuntDecision::no_prey) &&
          count(active_events, petri::HuntDecision::pursue) && count(active_events, petri::HuntDecision::flee) &&
          count(active_events, petri::HuntDecision::cover), "The active treatment missed hunt or evasion events");
    check(!count(passive_events, petri::HuntDecision::flee) && !count(passive_events, petri::HuntDecision::cover) &&
          passive.metrics().attacks > 0, "The passive treatment performed evasive actions or disabled hunting");
    check(active_events.energy_events == count(active_events, petri::HuntDecision::strike) +
          count(active_events, petri::HuntDecision::pursue), "A charged hunting turn was missing from the ledger");

    config.hunt_tuning.chase_cost = 0;
    petri::World free_chase(42, config);
    Events free_events{free_chase};
    observer.context = &free_events;
    for (int tick = 0; tick < 2000; ++tick) {
        free_chase.step(observer);
    }
    check(count(free_events, petri::HuntDecision::pursue) > 0 &&
          free_events.energy_events == count(free_events, petri::HuntDecision::strike) +
          count(free_events, petri::HuntDecision::pursue), "A zero-cost chase was missing from the ledger");

    config.hunt_tuning.chase_cost = 1;
    config.kin_avoidance = true;
    petri::World avoiding(42, config);
    Events avoiding_events{avoiding};
    observer.context = &avoiding_events;
    for (int tick = 0; tick < 3000; ++tick) {
        avoiding.step(observer);
        check(total_material(avoiding) == budget, "Avoiding kin changed the total material budget");
    }
    check(avoiding_events.births > 0 && avoiding_events.kill_pairs == avoiding.metrics().kills &&
          avoiding_events.kill_pairs > 0 && count(avoiding_events, petri::HuntDecision::kin_reluctance) > 0,
          "The kin treatment did not exercise inheritance, hunting and reluctance");
    check(!count(active_events, petri::HuntDecision::kin_reluctance), "The control avoided eligible prey as kin");

    config.kin_tuning = {0, 0, true};
    petri::World protected_prey(42, config);
    Events protected_events{protected_prey};
    observer.context = &protected_events;
    for (int tick = 0; tick < 1000; ++tick) {
        protected_prey.step(observer);
    }
    check(protected_events.kill_pairs == protected_prey.metrics().kills && protected_events.kill_pairs > 0 &&
          count(protected_events, petri::HuntDecision::kin_reluctance) > 0,
          "Strict protection must reject similar prey and allow hunts on dissimilar prey");

    config.kin_avoidance = false;
    config.cross_faction_hunts = true;
    petri::World faction_hunts(42, config);
    std::array<unsigned, petri::faction_count> founders{};
    for (int slot = 0; slot < petri::founder_count; ++slot) {
        const auto faction = faction_hunts.creatures()[slot].genes.faction;
        check(faction >= 1 && faction <= petri::faction_count, "Founder lacks a valid faction marker");
        ++founders[faction - 1];
    }
    for (const auto size : founders) {
        check(size == petri::founder_count / petri::faction_count, "Founder factions must start balanced");
    }
    Events faction_events{faction_hunts};
    observer.context = &faction_events;
    for (int tick = 0; tick < 3000; ++tick) {
        faction_hunts.step(observer);
        check(total_material(faction_hunts) == budget, "Faction protection changed the total material budget");
    }
    check(faction_events.inherited_mate_marker && faction_events.new_markers,
          "The faction test must exercise inheritance from the second parent and mutation");
    check(faction_events.kill_pairs > 0 && faction_events.kill_pairs == faction_hunts.metrics().kills &&
          count(faction_events, petri::HuntDecision::faction_protected) > 0,
          "Faction protection must reject some hunts and allow cross-faction kills");
}

void kin_relationships() {
    petri::Creature parent{};
    parent.id = 12;
    parent.founder = 2;
    petri::Creature first{};
    first.id = 13;
    first.parent = 3;
    first.other_parent = parent.id;
    petri::Creature half_sibling{};
    half_sibling.id = 14;
    half_sibling.parent = parent.id;
    petri::Creature distant{};
    distant.id = 15;
    distant.founder = parent.founder;
    check(parent.close_kin(first) && first.close_kin(parent) && first.close_kin(half_sibling) &&
          half_sibling.close_kin(first), "Close kin must include both parents and half siblings");
    check(!parent.close_kin(distant) && !parent.close_kin({}) && !parent.close_kin(parent),
          "A founder, empty slot or self was mistaken for a close relative");

    parent.genes = {6, 8};
    distant.genes = {10, 9};
    check(parent.genes.prey_resemblance(parent.genes) == petri::Genome::max_prey_resemblance &&
          parent.genes.prey_resemblance(distant.genes) == 3 && distant.genes.prey_resemblance(parent.genes) == 3,
          "Prey resemblance must weight digestion twice and be symmetric");
    distant.genes.evasion = 3;
    check(parent.genes.prey_resemblance(distant.genes) == 0, "Dissimilar prey received a resemblance weight");
}

void food_web_ecology() {
    const petri::Config control_config{80, true, 1, 0, false, true};
    const petri::Config predation_config{80, true, 1, 0, true, true};
    petri::World control(42, control_config);
    petri::World hunting(42, predation_config);
    const auto budget = total_material(hunting);
    check(total_material(control) == budget, "Food-web treatments began with different material");
    for (std::size_t slot = 0; slot < control.creatures().size(); ++slot) {
        const auto& left = control.creatures()[slot];
        const auto& right = hunting.creatures()[slot];
        check(left.cell == right.cell && left.genes.digestion == right.genes.digestion,
              "Matched food-web worlds had different founders");
        check(!right.alive() || (right.genes.digestion >= 5 && right.genes.digestion <= 11),
              "Food-web founder digestion was out of bounds");
    }
    bool carried_nutrients = false;
    for (int tick = 0; tick < 2000; ++tick) {
        control.step();
        hunting.step();
        check(total_material(control) == budget && total_material(hunting) == budget,
              "Feeding, transport, reproduction, or hunting lost food-web material");
        for (const auto& creature : hunting.creatures()) {
            check(creature.nutrients_carried <= petri::body_mass, "An organism carried too much material");
            carried_nutrients |= creature.nutrients_carried > 0;
        }
    }
    check(carried_nutrients && hunting.metrics().births > 0, "Food-web reproduction did not run");
    check(hunting.metrics().kills > 0 && control.metrics().attacks == 0,
          "Food-web hunting control failed");
}

void observe_events() {
    const petri::Config config{48, true, 1, 0, true};
    petri::World observed(42, config);
    petri::World unobserved(42, config);
    struct Events {
        std::array<std::uint64_t, petri::founder_count> births{};
        std::uint64_t hunts = 0;
        std::uint64_t plant_meals = 0, meat_meals = 0;
        std::uint64_t attacks = 0, landed = 0;
        std::array<std::uint64_t, 2> deaths{};
    } events;
    const petri::StepObserver observer{
        &events,
        [](void* context, const petri::Creature& parent, const petri::Creature& child) {
            auto& counts = static_cast<Events*>(context)->births;
            check(child.parent == parent.id && child.founder == parent.founder,
                  "Birth observer received the wrong parent or lineage");
            ++counts[parent.founder - 1];
        },
        [](void* context, std::uint16_t hunter, std::uint16_t prey) {
            const int distance = std::abs(int(hunter % petri::width) - int(prey % petri::width)) +
                std::abs(int(hunter / petri::width) - int(prey / petri::width));
            check(hunter < petri::width * petri::height && prey < petri::width * petri::height &&
                  distance == 1, "Hunt observer received invalid hunter or prey coordinates");
            ++static_cast<Events*>(context)->hunts;
        },
        [](void* context, const petri::Creature& creature, bool meat) {
            check(creature.alive(), "Meal observer received a dead creature");
            auto& events = *static_cast<Events*>(context);
            ++(meat ? events.meat_meals : events.plant_meals);
        }, nullptr, nullptr, nullptr,
        [](void* context, const petri::Creature& attacker, const petri::Creature& target, bool landed) {
            check(attacker.alive() && target.alive() && attacker.id != target.id,
                  "Attack observer lost the live participants");
            auto& events = *static_cast<Events*>(context);
            ++events.attacks;
            events.landed += landed;
        },
        [](void* context, const petri::Creature& creature, petri::DeathCause cause) {
            check(creature.alive(), "Death observer lost the creature before reporting its death");
            ++static_cast<Events*>(context)->deaths[static_cast<unsigned>(cause)];
        }
    };
    for (int tick = 0; tick < 2000; ++tick) {
        observed.step(observer);
        unobserved.step();
    }
    std::uint64_t reported = 0;
    for (const auto count : events.births) {
        reported += count;
    }
    check(reported > 0 && reported == observed.metrics().births, "Birth observer missed a birth");
    check(events.hunts > 0 && events.hunts == observed.metrics().kills,
          "Hunt observer missed a kill");
    check(events.attacks == observed.metrics().attacks && events.attacks > events.landed &&
          events.landed == events.hunts, "Attack observer must report failed and successful strikes");
    check(events.deaths[0] > 0 && events.deaths[1] == events.hunts &&
          events.deaths[0] + events.deaths[1] == observed.metrics().deaths,
          "Death observer must distinguish energy depletion from predation");
    check(events.plant_meals > 0 && events.plant_meals <= observed.metrics().plants_eaten &&
          events.meat_meals > 0 && events.meat_meals <= observed.metrics().animal_eaten,
          "Meal observer missed bites or reported empty meals");
    check(observed.fingerprint() == unobserved.fingerprint(), "Observation changed the simulation");
}

void predation() {
    petri::World control(42, {48, true, 1});
    petri::World hunting(42, {48, true, 1, 0, true});
    check(total_material(control) == total_material(hunting), "Predation changed the starting budget");
    for (std::size_t index = 0; index < control.cells().size(); ++index) {
        const auto& first = control.cells()[index];
        const auto& second = hunting.cells()[index];
        check(first.plants == second.plants && first.moisture == second.moisture &&
              first.nutrients == second.nutrients && first.animal == second.animal,
              "Predation changed the seeded environment");
    }
    for (std::size_t slot = 0; slot < control.creatures().size(); ++slot) {
        check(control.creatures()[slot].cell == hunting.creatures()[slot].cell,
              "Predation changed founder placement");
    }
    const auto budget = total_material(hunting);
    for (int tick = 0; tick < 2000; ++tick) {
        control.step();
        hunting.step();
        const auto& metrics = hunting.metrics();
        check(metrics.kills <= metrics.attacks && metrics.kills <= metrics.deaths,
              "Invalid attack accounting");
        check(total_material(hunting) == budget, "Predation destroyed or created biomass");

        std::array<bool, petri::width * petri::height> occupied{};
        unsigned counted = 0;
        for (const auto& creature : hunting.creatures()) {
            if (creature.alive()) {
                check(creature.cell < occupied.size() && !occupied[creature.cell],
                      "A hunt corrupted creature occupancy");
                occupied[creature.cell] = true;
                ++counted;
            }
        }
        check(counted == metrics.population &&
              metrics.population + metrics.deaths == petri::founder_count + metrics.births,
              "Predation corrupted lifecycle counts");
    }
    check(control.metrics().attacks == 0 && control.metrics().kills == 0,
          "The control attacked prey");
    check(hunting.metrics().kills > 0 && hunting.metrics().attacks > hunting.metrics().kills,
          "Expected both successful and failed hunts");
}

void animal_food() {
    petri::World world(42, {48, true, 1});
    petri::World seeded(42, {48, true, 1, 75});
    petri::World starving(42, {0, true, 1});
    bool observed_carcass = false;
    for (int tick = 0; tick < 1000; ++tick) {
        world.step();
        seeded.step();
        starving.step();
        for (const auto& cell : starving.cells()) {
            observed_carcass |= cell.animal > 0;
        }
        check(total_material(world) == 48 * petri::width * petri::height +
              petri::founder_count * petri::body_mass, "Death, feeding, or birth lost biomass");
        check(total_material(seeded) == total_material(world), "Seeded carrion changed the nutrient budget");
        check(total_material(starving) == petri::founder_count * petri::body_mass,
              "Starvation lost founder biomass");
    }
    check(observed_carcass, "Deaths did not leave carcasses");
    for (const auto& cell : starving.cells()) {
        check(cell.animal == 0, "Old carcass did not decay");
    }
    check(world.metrics().births > 0 && world.metrics().deaths > 0 &&
          world.metrics().animal_eaten > 0, "Natural carcasses were not scavenged");
    check(seeded.metrics().animal_eaten > world.metrics().animal_eaten,
          "Seeded carrion did not change feeding");

    for (const auto& creature : world.creatures()) {
        if (creature.alive()) {
            check(creature.genes.meat_digestion() >= petri::Genome::minimum &&
                  creature.genes.meat_digestion() <= petri::Genome::maximum &&
                  creature.genes.meat_digestion() + creature.genes.digestion ==
                      petri::Genome::minimum + petri::Genome::maximum,
                  "Digestion tradeoff failed");
        }
    }
}

void fruiting_plants() {
    petri::Config config{80, true, 1, 0, true, true, false, false, true, true, true, true};
    config.fruiting_plants = true;
    petri::World world(42, config);
    const auto budget = total_material(world);
    std::array<int, petri::width * petri::height> previous{};
    int fruiting_ticks = 0;
    bool packed_fruit = false;
    for (int tick = 0; tick < 1600; ++tick) {
        world.step();
        bool ripened = false;
        for (std::size_t index = 0; index < previous.size(); ++index) {
            const int fruit = world.fruit_at(index);
            check(fruit <= petri::max_fruit && (!fruit || world.fruiting_shrub(index)), "Invalid fruit stock");
            ripened |= fruit > previous[index];
            previous[index] = fruit;
        }
        fruiting_ticks += ripened;
        packed_fruit |= std::any_of(world.creatures().begin(), world.creatures().end(),
            [](const auto& creature) { return creature.packed_fruit != 0; });
        check(total_material(world) == budget, "Fruit production, eating, spoilage, or provisioning lost material");
    }
    check(world.staggered_growth() && fruiting_ticks > 2 && world.metrics().fruit_eaten > 0 && packed_fruit,
          "Fruiting, fruit meals, and care must use the new food stock");
}

void checkpoint_replay() {
    for (const auto config : {petri::Config{}, petri::Config{48, false},
                              petri::Config{0}, petri::Config{48, true, 1, 75},
                              petri::Config{48, true, 1, 0, true},
                              petri::Config{80, true, 1, 0, true, true},
                              petri::Config{48, true, 1, 0, true, true, false, false, true},
                              petri::Config{48, true, 1, 0, true, true, false, false, true, true, true, true},
                              petri::Config{80, true, 1, 0, true, true, false, false, true, true, true, true,
                                            true, false, {60, 1}},
                              petri::Config{80, true, 1, 0, true, true, false, false, true, true, true, true,
                                            true, false, {42, 1}, true, {2, 14, true}, false, true, false, true}}) {
        petri::World original(42, config);
        for (int tick = 0; tick < 901; ++tick) {
            original.step();
        }
        petri::Checkpoint bytes{};
        original.save_checkpoint(bytes);
        petri::World restored(1);
        check(restored.load_checkpoint(bytes), "Valid checkpoint was rejected");
        check(original.fingerprint() == restored.fingerprint(), "Restored state differs");
        petri::Checkpoint reencoded{};
        restored.save_checkpoint(reencoded);
        check(bytes == reencoded, "Checkpoint encoding changed after restoration");
        for (int tick = 0; tick < 500; ++tick) {
            original.step();
            restored.step();
            check(original.fingerprint() == restored.fingerprint(), "Resumed run diverged");
        }

        const auto before = restored.fingerprint();
        auto damaged = bytes;
        damaged[damaged.size() / 2] ^= 1;
        check(!restored.load_checkpoint(damaged), "Checksum error was accepted");
        damaged = bytes;
        ++damaged[5];
        check(!restored.load_checkpoint(damaged), "Unknown version was accepted");
        check(restored.fingerprint() == before, "Rejection changed the running world");
    }
}

void reseal(petri::Checkpoint& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t index = 0; index + 8 < bytes.size(); ++index) {
        hash = (hash ^ bytes[index]) * 1099511628211ull;
    }
    for (int byte = 0; byte < 8; ++byte) {
        bytes[bytes.size() - 8 + byte] = static_cast<std::uint8_t>(hash >> (8 * byte));
    }
}

void detritus_limit() {
    // Cells hold 16-bit detritus in memory while checkpoints keep 32-bit fields.
    // Gather material into one cell so the total stays valid and only the limit differs.
    petri::World original(42, petri::Config{48, true, 1, 0, true, true});
    for (int tick = 0; tick < 200; ++tick) {
        original.step();
    }
    petri::Checkpoint saved{};
    original.save_checkpoint(saved);
    constexpr std::size_t cells = 6 + 151 + petri::climate_nodes + 15 * 8;
    constexpr std::size_t stride = 9, nutrients = 1, detritus = 3;
    const auto get = [](const petri::Checkpoint& bytes, std::size_t at) {
        return bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | std::uint32_t(bytes[at + 3]) << 24;
    };
    const auto set = [](petri::Checkpoint& bytes, std::size_t at, std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            bytes[at + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
        }
    };
    const auto concentrate = [&](std::uint32_t target) {
        auto bytes = saved;
        auto needed = target - get(bytes, cells + detritus);
        for (std::size_t cell = 1; cell < petri::width * petri::height && needed; ++cell) {
            const auto at = cells + cell * stride;
            const auto from_detritus = std::min(needed, get(bytes, at + detritus));
            set(bytes, at + detritus, get(bytes, at + detritus) - from_detritus);
            needed -= from_detritus;
            const auto from_nutrients = std::min<std::uint32_t>(needed, bytes[at + nutrients]);
            bytes[at + nutrients] -= from_nutrients;
            needed -= from_nutrients;
        }
        check(needed == 0, "Test world lacks the material to fill one cell");
        set(bytes, cells + detritus, target);
        reseal(bytes);
        return bytes;
    };
    petri::World restored(1);
    check(restored.load_checkpoint(concentrate(petri::Cell::max_detritus)), "Full detritus cell was rejected");
    check(restored.cells()[0].detritus == petri::Cell::max_detritus, "Full detritus cell was not restored");
    check(!restored.load_checkpoint(concentrate(petri::Cell::max_detritus + 1u)), "Detritus overflow was accepted");
}

double mean_digestion(const petri::World& world) {
    unsigned total = 0;
    for (const auto& creature : world.creatures()) {
        if (creature.alive()) {
            total += creature.genes.digestion;
        }
    }
    check(world.metrics().population > 0, "Population extinct before trait measurement");
    return double(total) / world.metrics().population;
}

void selection_scenarios() {
    for (const auto seed : {1u, 2u, 42u}) {
        petri::World scarce(seed, {7});
        petri::World rich(seed, {48});
        petri::World control(seed, {7, false});
        const auto advance = [](petri::World& world, std::uint64_t births) {
            while (world.metrics().births < births && world.metrics().ticks < 4000) {
                world.step();
            }
            check(world.metrics().births >= births, "Selection scenario missed birth target");
            check(world.metrics().capacity_blocks == 0, "Pool interfered with selection scenario");
        };
        advance(scarce, 250);
        advance(rich, 250);
        check(rich.metrics().ticks < scarce.metrics().ticks, "Rich world did not reproduce faster");
        check(mean_digestion(scarce) > mean_digestion(rich),
              "Scarcity effect at matched births changed");
        while (control.metrics().ticks < scarce.metrics().ticks) {
            control.step();
        }
        check(mean_digestion(scarce) > mean_digestion(control) + 1,
              "Digestion response did not exceed the neutral control");
    }
}

void life_frame() {
    petri::World viewed(42);
    petri::World headless(42);
    petri::Frame frame{};
    for (int tick = 0; tick < 128; ++tick) {
        petri::render_life(viewed, frame);
        for (const auto& creature : viewed.creatures()) {
            if (creature.alive()) {
                check(frame[creature.cell] == 0xfe06, "Creature color or position is wrong");
            }
        }
        check(frame.front() != 0 && frame.front() != 0xfe06, "Background pixel was not drawn");
        petri::render_life(viewed, frame, false);
        for (const auto& creature : viewed.creatures()) {
            if (creature.alive()) {
                check(frame[creature.cell] != 0xfe06, "Habitat view drew a creature");
            }
        }
        for (int step = 0; step < 8; ++step) {
            viewed.step();
            headless.step();
        }
    }
    check(viewed.fingerprint() == headless.fingerprint(), "Rendering changed the replay");

    petri::World carrion(42, {48, true, 1, 75});
    petri::render_life(carrion, frame, false);
    unsigned carcasses = 0;
    for (std::size_t index = 0; index < frame.size(); ++index) {
        if (carrion.cells()[index].animal > 0) {
            check(frame[index] == 0xa013, "Carcass did not render magenta");
            ++carcasses;
        }
    }
    check(carcasses > 0, "No carcasses to verify");
}

} // namespace

int main() {
    lifecycle();
    replay();
    carrying_capacity();
    animal_food();
    predation();
    observe_events();
    food_web_ecology();
    carcasses_nourish_plants();
    sexual_reproduction();
    nest_care();
    adaptive_hunting();
    kin_relationships();
    fruiting_plants();
    checkpoint_replay();
    detritus_limit();
    selection_scenarios();
    life_frame();
}
