#include "world.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace {

template<class Integer>
bool parse(std::string_view arg, Integer& value) {
    const auto end = arg.data() + arg.size();
    const auto result = std::from_chars(arg.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool read_checkpoint(const char* path, petri::World& world) {
    std::ifstream file(path, std::ios::binary);
    std::error_code error;
    const auto length = std::filesystem::file_size(path, error);
    const auto read = [&](auto& data) {
        return file.read(reinterpret_cast<char*>(data.data()), data.size()) &&
               file.peek() == std::char_traits<char>::eof() && file.eof() && !file.bad() &&
               world.load_checkpoint(data);
    };
    if (error) {
        return false;
    }
    if (length == petri::LegacyCheckpoint{}.size()) {
        petri::LegacyCheckpoint data;
        return read(data);
    }
    if (length == petri::ScavengingCheckpoint{}.size()) {
        petri::ScavengingCheckpoint data;
        return read(data);
    }
    if (length == petri::PredationCheckpoint{}.size()) {
        petri::PredationCheckpoint data;
        return read(data);
    }
    if (length == petri::FoodWebCheckpoint{}.size()) {
        petri::FoodWebCheckpoint data;
        return read(data);
    }
    if (length == petri::ConnectedCheckpoint{}.size()) {
        petri::ConnectedCheckpoint data;
        return read(data);
    }
    if (length == petri::IrregularCheckpoint{}.size()) {
        petri::IrregularCheckpoint data;
        return read(data);
    }
    if (length == petri::SexualCheckpoint{}.size()) {
        petri::SexualCheckpoint data;
        return read(data);
    }
    if (length == petri::NestCheckpoint{}.size()) {
        petri::NestCheckpoint data;
        return read(data);
    }
    if (length == petri::AdaptiveCheckpoint{}.size()) {
        petri::AdaptiveCheckpoint data;
        return read(data);
    }
    if (length == petri::HuntTuningCheckpoint{}.size()) {
        petri::HuntTuningCheckpoint data;
        return read(data);
    }
    if (length == petri::KinAvoidanceCheckpoint{}.size()) {
        petri::KinAvoidanceCheckpoint data;
        return read(data);
    }
    if (length == petri::KinTuningCheckpoint{}.size()) {
        petri::KinTuningCheckpoint data;
        return read(data);
    }
    if (length == petri::FactionCheckpoint{}.size()) {
        petri::FactionCheckpoint data;
        return read(data);
    }
    if (length == petri::FloraCheckpoint{}.size()) {
        petri::FloraCheckpoint data;
        return read(data);
    }
    if (length == petri::WeatherCheckpoint{}.size()) {
        petri::WeatherCheckpoint data;
        return read(data);
    }
    petri::Checkpoint data;
    return length == data.size() && read(data);
}

bool write_checkpoint(const char* path, const petri::World& world) {
    petri::Checkpoint data;
    world.save_checkpoint(data);

    // Stage beside the destination so an interrupted write leaves the old file readable.
    auto temporary = std::string(path) + ".XXXXXX";
    const int descriptor = mkstemp(temporary.data());
    if (descriptor < 0) {
        return false;
    }
    auto* file = fdopen(descriptor, "wb");
    if (!file) {
        close(descriptor);
        std::remove(temporary.c_str());
        return false;
    }
    const bool complete = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    const bool closed = std::fclose(file) == 0;
    if (complete && closed && std::rename(temporary.c_str(), path) == 0) {
        return true;
    }
    std::remove(temporary.c_str());
    return false;
}

enum class Diet { unknown, plant, mixed, animal };

Diet diet(const petri::Creature& creature) {
    const auto consumed = std::uint64_t(creature.plants_eaten) + creature.animal_eaten;
    if (consumed < 32) {
        return Diet::unknown;
    }
    const double animal_share = double(creature.animal_eaten) / consumed;
    if (animal_share < 0.2) {
        return Diet::plant;
    }
    if (animal_share < 0.8) {
        return Diet::mixed;
    }
    return Diet::animal;
}

bool meat_specialist(const petri::Creature& creature) {
    return creature.genes.meat_digestion() >= 10 && diet(creature) == Diet::animal;
}

bool hunter(const petri::Creature& creature) {
    return creature.prey_killed && creature.animal_eaten > creature.plants_eaten &&
           std::uint64_t(creature.plants_eaten) + creature.animal_eaten >= 32;
}

struct RunReport {
    using Counts = std::array<std::uint64_t, petri::founder_count>;
    const petri::World& world;
    std::uint64_t previous_kills;
    Counts births{};
    std::uint64_t hunter_births = 0, grazer_births = 0;
    std::array<std::uint64_t, static_cast<unsigned>(petri::HuntDecision::count)> decisions{};
    petri::HuntEnergy hunt_energy;
    std::uint64_t same_founder_kills = 0, close_kin_kills = 0, parent_offspring_kills = 0, similar_prey_kills = 0;
    std::uint64_t same_faction_kills = 0;

    static void record_birth(void* context, const petri::Creature& parent, const petri::Creature& child) {
        auto& report = *static_cast<RunReport*>(context);
        if (meat_specialist(parent) && child.genes.meat_digestion() >= 10) {
            ++report.births[parent.founder - 1];
        }
        bool hunting_parent = hunter(parent);
        bool grazing_parent = diet(parent) == Diet::plant;
        if (child.other_parent) {
            const auto& creatures = report.world.creatures();
            const auto mate = std::find_if(creatures.begin(), creatures.end(), [&](const auto& candidate) {
                return candidate.id == child.other_parent;
            });
            if (mate != creatures.end()) {
                hunting_parent |= hunter(*mate);
                grazing_parent |= diet(*mate) == Diet::plant;
            }
        }
        report.hunter_births += hunting_parent;
        report.grazer_births += grazing_parent;
    }

    static void record_decision(void* context, petri::HuntDecision decision) {
        ++static_cast<RunReport*>(context)->decisions[static_cast<unsigned>(decision)];
    }

    static void record_energy(void* context, const petri::HuntEnergy& action) {
        auto& total = static_cast<RunReport*>(context)->hunt_energy;
        total.chase += action.chase;
        total.strike += action.strike;
        total.upkeep += action.upkeep;
        total.prey_reserve += action.prey_reserve;
        total.kill_meal += action.kill_meal;
        total.kill_cap_loss += action.kill_cap_loss;
        total.other_meal += action.other_meal;
    }

    static void record_kill(void* context, const petri::Creature& hunter, const petri::Creature& prey) {
        auto& report = *static_cast<RunReport*>(context);
        report.same_founder_kills += hunter.founder == prey.founder;
        report.close_kin_kills += hunter.close_kin(prey);
        report.parent_offspring_kills += hunter.has_parent(prey.id) || prey.has_parent(hunter.id);
        report.similar_prey_kills += hunter.genes.prey_resemblance(prey.genes) > 0;
        report.same_faction_kills += hunter.genes.faction && hunter.genes.faction == prey.genes.faction;
    }

    template<std::size_t size>
    static void print_counts(const char* label, const std::array<std::uint64_t, size>& counts) {
        std::cout << ' ' << label << '=';
        bool first = true;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] > 0) {
                if (!first) {
                    std::cout << ',';
                }
                std::cout << i + 1 << ':' << counts[i];
                first = false;
            }
        }
        if (first) {
            std::cout << '-';
        }
    }

    void print_lineages() {
        Counts live{};
        Counts highest_meat_digestion{};
        Counts oldest_generation{};
        std::array<std::uint64_t, petri::faction_count> faction_population{};
        unsigned plant_feeders = 0;
        unsigned live_hunters = 0;
        for (const auto& creature : world.creatures()) {
            if (creature.alive()) {
                if (meat_specialist(creature)) {
                    const auto lineage = creature.founder - 1;
                    ++live[lineage];
                    highest_meat_digestion[lineage] = std::max(
                        highest_meat_digestion[lineage], std::uint64_t(creature.genes.meat_digestion()));
                    oldest_generation[lineage] = std::max(
                        oldest_generation[lineage], std::uint64_t(creature.generation));
                }
                plant_feeders += diet(creature) == Diet::plant;
                live_hunters += hunter(creature);
                if (const auto faction = creature.genes.faction) {
                    ++faction_population[faction - 1];
                }
            }
        }
        const auto& metrics = world.metrics();
        std::cout << "sample_tick=" << metrics.ticks << " plant_feeders=" << plant_feeders
                  << " population=" << metrics.population << " kills=" << metrics.kills
                  << " capacity_blocks=" << metrics.capacity_blocks
                  << " live_hunters=" << live_hunters
                  << " hunter_births=" << hunter_births << " grazer_births=" << grazer_births;
        print_counts("live_specialists", live);
        print_counts("specialist_births", births);
        print_counts("specialist_meat_gene_max", highest_meat_digestion);
        print_counts("specialist_generation_max", oldest_generation);
        if (world.faction_markers()) {
            print_counts("faction_population", faction_population);
        }
        std::cout << '\n';
        births.fill(0);
        hunter_births = grazer_births = 0;
    }

    void print_hunts() {
        constexpr std::array labels{"sated", "low_energy", "no_prey", "blocked", "forage_or_rest",
                                    "strike", "pursue", "flee", "cover", "kin_reluctance", "faction_protected"};
        static_assert(labels.size() == std::tuple_size_v<decltype(decisions)>);
        const auto& metrics = world.metrics();
        const auto kills = metrics.kills - previous_kills;
        std::cout << "hunt_tick=" << metrics.ticks << " kills=" << kills
                  << " same_founder_kills=" << same_founder_kills << " close_kin_kills=" << close_kin_kills
                  << " parent_offspring_kills=" << parent_offspring_kills << " similar_prey_kills=" << similar_prey_kills;
        if (world.faction_markers()) {
            std::cout << " same_faction_kills=" << same_faction_kills
                      << " cross_faction_kills=" << kills - same_faction_kills;
        }
        for (unsigned i = 0; i < labels.size(); ++i) {
            std::cout << ' ' << labels[i] << '=' << decisions[i];
        }
        std::cout << " chase_energy=" << hunt_energy.chase << " strike_energy=" << hunt_energy.strike
                  << " hunt_upkeep=" << hunt_energy.upkeep << " prey_reserve=" << hunt_energy.prey_reserve
                  << " kill_meal_energy=" << hunt_energy.kill_meal << " kill_cap_loss=" << hunt_energy.kill_cap_loss
                  << " other_meal_energy=" << hunt_energy.other_meal
                  << " hunt_cost=" << hunt_energy.chase + hunt_energy.strike
                  << " hunt_return=" << hunt_energy.prey_reserve + hunt_energy.kill_meal - hunt_energy.kill_cap_loss
                  << '\n';
        decisions.fill(0);
        hunt_energy = {};
        same_founder_kills = close_kin_kills = parent_offspring_kills = similar_prey_kills = 0;
        same_faction_kills = 0;
        previous_kills = metrics.kills;
    }
};

void usage(std::ostream& out) {
    out << "Usage: petri [--seed UINT32] [--nutrients 0-255] [--carcass-decay 0-255] [--initial-carrion 0-100] "
           "[--predation] [--food-web] [--connected-biomes] [--irregular-terrain] [--sexual] [--nests] [--guarding] "
           "[--provisioning] [--adaptive-hunts] [--passive-prey] [--kin-avoidance] [--kin-strength 0-20] "
           "[--resemblance-strength 0-20] [--protect-similar-prey] [--hunt-threshold 0-160] [--chase-cost 0-160] "
           "[--faction-markers] [--cross-faction-hunts] [--staggered-growth] [--fruiting-plants] "
           "[--dynamic-environment] [--neutral] | petri --load PATH\n"
        << "For either form: [--ticks UINT64] [--until-births UINT64] [--save PATH] [--lineage-every TICKS] [--hunt-every TICKS] [--benchmark]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t seed = 42;
    std::uint64_t ticks = 1000;
    unsigned nutrients = petri::Config{}.initial_nutrients;
    unsigned carcass_decay = 1;
    unsigned initial_carrion = 0;
    bool benchmark = false;
    bool custom_start = false;
    bool neutral = false;
    bool predation = false;
    bool food_web = false;
    bool connected_biomes = false;
    bool irregular_terrain = false;
    bool sexual_reproduction = false;
    bool nests = false, guarding = false, provisioning = false;
    bool adaptive_hunts = false, passive_prey = false, kin_avoidance = false;
    bool faction_markers = false, cross_faction_hunts = false;
    bool staggered_growth = false, fruiting_plants = false;
    bool dynamic_environment = false;
    bool hunt_tuning_supplied = false, kin_tuning_supplied = false;
    bool protect_similar_prey = false;
    unsigned hunt_threshold = petri::HuntTuning::default_threshold;
    unsigned chase_cost = petri::HuntTuning::default_chase_cost;
    unsigned kin_strength = petri::KinTuning::default_strength;
    unsigned resemblance_strength = petri::KinTuning::default_strength;
    std::uint64_t birth_target = 0;
    std::uint64_t lineage_every = 0;
    std::uint64_t hunt_every = 0;
    const char* load_path = nullptr;
    const char* save_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--help") {
            usage(std::cout);
            return 0;
        }
        if (option == "--benchmark") {
            benchmark = true;
            continue;
        }
        if (option == "--neutral") {
            neutral = true;
            custom_start = true;
            continue;
        }
        if (option == "--predation") {
            predation = true;
            custom_start = true;
            continue;
        }
        if (option == "--food-web") {
            food_web = true;
            custom_start = true;
            continue;
        }
        if (option == "--connected-biomes") {
            connected_biomes = true;
            custom_start = true;
            continue;
        }
        if (option == "--irregular-terrain") {
            irregular_terrain = true;
            custom_start = true;
            continue;
        }
        if (option == "--sexual") {
            sexual_reproduction = true;
            custom_start = true;
            continue;
        }
        if (option == "--nests" || option == "--guarding" || option == "--provisioning") {
            nests |= option == "--nests";
            guarding |= option == "--guarding";
            provisioning |= option == "--provisioning";
            custom_start = true;
            continue;
        }
        if (option == "--adaptive-hunts" || option == "--passive-prey" || option == "--kin-avoidance") {
            adaptive_hunts |= option == "--adaptive-hunts";
            passive_prey |= option == "--passive-prey";
            kin_avoidance |= option == "--kin-avoidance";
            custom_start = true;
            continue;
        }
        if (option == "--protect-similar-prey") {
            protect_similar_prey = kin_tuning_supplied = custom_start = true;
            continue;
        }
        if (option == "--faction-markers" || option == "--cross-faction-hunts") {
            faction_markers |= option == "--faction-markers";
            cross_faction_hunts |= option == "--cross-faction-hunts";
            custom_start = true;
            continue;
        }
        if (option == "--staggered-growth" || option == "--fruiting-plants") {
            staggered_growth |= option == "--staggered-growth";
            fruiting_plants |= option == "--fruiting-plants";
            custom_start = true;
            continue;
        }
        if (option == "--dynamic-environment") {
            dynamic_environment = custom_start = true;
            continue;
        }
        if (i + 1 < argc) {
            const std::string_view value = argv[++i];
            if ((option == "--seed" && parse(value, seed)) ||
                (option == "--nutrients" && parse(value, nutrients) && nutrients <= 255) ||
                (option == "--carcass-decay" && parse(value, carcass_decay) && carcass_decay <= 255) ||
                (option == "--initial-carrion" && parse(value, initial_carrion) && initial_carrion <= 100)) {
                custom_start = true;
                continue;
            }
            if ((option == "--hunt-threshold" && parse(value, hunt_threshold) &&
                 hunt_threshold <= petri::HuntTuning::max_threshold) ||
                (option == "--chase-cost" && parse(value, chase_cost) &&
                 chase_cost <= petri::HuntTuning::max_chase_cost)) {
                hunt_tuning_supplied = custom_start = true;
                continue;
            }
            if ((option == "--kin-strength" && parse(value, kin_strength) && kin_strength <= petri::KinTuning::max_strength) ||
                (option == "--resemblance-strength" && parse(value, resemblance_strength) &&
                 resemblance_strength <= petri::KinTuning::max_strength)) {
                kin_tuning_supplied = custom_start = true;
                continue;
            }
            if ((option == "--ticks" && parse(value, ticks)) ||
                (option == "--until-births" && parse(value, birth_target) && birth_target > 0) ||
                (option == "--lineage-every" && parse(value, lineage_every) && lineage_every > 0) ||
                (option == "--hunt-every" && parse(value, hunt_every) && hunt_every > 0)) {
                continue;
            }
            if (option == "--load" && !value.empty()) {
                load_path = argv[i];
                continue;
            }
            if (option == "--save" && !value.empty()) {
                save_path = argv[i];
                continue;
            }
        }
        std::cerr << "Invalid or incomplete option: " << option << '\n';
        usage(std::cerr);
        return 2;
    }

    if (load_path && custom_start) {
        std::cerr << "--load supplies the initial world. Use initialization options for a new run.\n";
        return 2;
    }

    if (!carcass_decay && initial_carrion) {
        std::cerr << "--initial-carrion requires a positive --carcass-decay.\n";
        return 2;
    }

    if (!carcass_decay && (predation || food_web)) {
        std::cerr << "--predation and --food-web require a positive --carcass-decay.\n";
        return 2;
    }

    if (connected_biomes && (!petri::wide_world || !food_web)) {
        std::cerr << "--connected-biomes requires the wide build and --food-web.\n";
        return 2;
    }

    if (irregular_terrain && !connected_biomes) {
        std::cerr << "--irregular-terrain requires --connected-biomes.\n";
        return 2;
    }

    if (sexual_reproduction && !food_web) {
        std::cerr << "--sexual requires --food-web so both parents can contribute body nutrients.\n";
        return 2;
    }
    if ((nests && !sexual_reproduction) || ((guarding || provisioning) && !nests)) {
        std::cerr << "--nests requires --sexual; --guarding and --provisioning require --nests.\n";
        return 2;
    }
    if ((adaptive_hunts && !(predation && food_web)) || (passive_prey && !adaptive_hunts)) {
        std::cerr << "Use --adaptive-hunts with --food-web and --predation. --passive-prey requires --adaptive-hunts.\n";
        return 2;
    }
    if ((hunt_tuning_supplied || kin_avoidance) && !adaptive_hunts) {
        std::cerr << "--hunt-threshold, --chase-cost and --kin-avoidance require --adaptive-hunts.\n";
        return 2;
    }
    if (kin_tuning_supplied && !kin_avoidance) {
        std::cerr << "--kin-strength, --resemblance-strength and --protect-similar-prey require --kin-avoidance.\n";
        return 2;
    }
    if (cross_faction_hunts && !adaptive_hunts) {
        std::cerr << "--cross-faction-hunts requires --adaptive-hunts.\n";
        return 2;
    }
    if (dynamic_environment && !connected_biomes) {
        std::cerr << "--dynamic-environment requires --connected-biomes.\n";
        return 2;
    }

    petri::World world(seed, {static_cast<std::uint8_t>(nutrients), !neutral,
                             static_cast<std::uint8_t>(carcass_decay), static_cast<std::uint8_t>(initial_carrion),
                             predation, food_web, connected_biomes, irregular_terrain, sexual_reproduction,
                             nests, guarding, provisioning, adaptive_hunts, passive_prey,
                             {static_cast<std::uint8_t>(hunt_threshold), static_cast<std::uint8_t>(chase_cost)}, kin_avoidance,
                             {static_cast<std::uint8_t>(kin_strength), static_cast<std::uint8_t>(resemblance_strength),
                              protect_similar_prey}, faction_markers, cross_faction_hunts, staggered_growth, fruiting_plants,
                              dynamic_environment});
    if (load_path && !read_checkpoint(load_path, world)) {
        std::cerr << "Could not read a valid checkpoint from " << load_path << '\n';
        return 1;
    }
    auto peak_population = world.metrics().population;
    RunReport report{world, world.metrics().kills};
    const petri::StepObserver observer{&report, lineage_every ? RunReport::record_birth : nullptr,
                                     nullptr, nullptr, hunt_every ? RunReport::record_decision : nullptr,
                                     hunt_every ? RunReport::record_energy : nullptr,
                                     hunt_every ? RunReport::record_kill : nullptr};
    const auto start = std::chrono::steady_clock::now();
    const auto initial_tick = world.metrics().ticks;
    for (std::uint64_t remaining = ticks;
         remaining > 0 && (birth_target == 0 || world.metrics().births < birth_target);
         --remaining) {
        world.step(observer);
        const auto elapsed_ticks = world.metrics().ticks - initial_tick;
        if (lineage_every && elapsed_ticks % lineage_every == 0) {
            report.print_lineages();
        }
        if (hunt_every && elapsed_ticks % hunt_every == 0) {
            report.print_hunts();
        }
        peak_population = std::max(peak_population, world.metrics().population);
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    if (hunt_every && (world.metrics().ticks - initial_tick) % hunt_every != 0) {
        report.print_hunts();
    }
    unsigned plant_biomass = 0;
    unsigned animal_biomass = 0;
    for (const auto& cell : world.cells()) {
        plant_biomass += cell.plants;
        animal_biomass += cell.animal;
    }
    const auto& metrics = world.metrics();
    if (load_path) {
        std::cout << "source=checkpoint";
    } else {
        std::cout << "seed=" << seed << " nutrients=" << nutrients
                  << " mode=" << (neutral ? "neutral" : "evolving") << " initial_carrion=" << initial_carrion;
    }
    std::cout << " carcass_decay=" << unsigned(world.carcass_decay()) << " tick=" << metrics.ticks
              << " predation=" << world.predation() << " food_web=" << world.food_web()
              << " population=" << metrics.population << " peak_population=" << peak_population
              << " births=" << metrics.births
              << " deaths=" << metrics.deaths << " capacity_blocks=" << metrics.capacity_blocks
              << " attacks=" << metrics.attacks << " kills=" << metrics.kills
              << " plant_biomass=" << plant_biomass << " animal_biomass=" << animal_biomass
              << " plants_eaten=" << metrics.plants_eaten << " animal_eaten=" << metrics.animal_eaten
              << " fingerprint=" << std::hex << std::setw(16) << std::setfill('0')
              << world.fingerprint() << std::dec;
    if (world.faction_markers()) {
        std::cout << " faction_markers=1 cross_faction_hunts=" << world.cross_faction_hunts();
    }
    if (world.staggered_growth()) {
        std::cout << " staggered_growth=1";
    }
    if (world.dynamic_environment()) {
        std::cout << " dynamic_environment=1";
    }
    if (world.fruiting_plants()) {
        unsigned shrubs = 0, fruit_biomass = 0;
        for (std::size_t index = 0; index < world.cells().size(); ++index) {
            shrubs += world.fruiting_shrub(index);
            fruit_biomass += world.fruit_at(index);
        }
        std::cout << " fruiting_shrubs=" << shrubs << " fruit_biomass=" << fruit_biomass
                  << " fruit_eaten=" << metrics.fruit_eaten;
    }
    if (world.adaptive_hunts()) {
        const auto tuning = world.hunt_tuning();
        std::cout << " adaptive_hunts=1 passive_prey=" << world.passive_prey()
                  << " hunt_threshold=" << unsigned(tuning.threshold) << " chase_cost=" << unsigned(tuning.chase_cost)
                  << " kin_avoidance=" << world.kin_avoidance();
        if (world.kin_avoidance()) {
            const auto kin = world.kin_tuning();
            std::cout << " kin_strength=" << unsigned(kin.kin_strength)
                      << " resemblance_strength=" << unsigned(kin.resemblance_strength)
                      << " protect_similar_prey=" << kin.protect_similar_prey;
        }
        if (metrics.population) {
            unsigned hunting = 0, evasion = 0, reluctance = 0;
            for (const auto& creature : world.creatures()) {
                if (creature.alive()) {
                    hunting += creature.genes.hunting;
                    evasion += creature.genes.evasion;
                    reluctance += creature.genes.kin_reluctance;
                }
            }
            std::cout << " mean_hunting=" << double(hunting) / metrics.population
                      << " mean_evasion=" << double(evasion) / metrics.population;
            if (world.kin_avoidance()) {
                std::cout << " mean_kin_reluctance=" << double(reluctance) / metrics.population;
            }
        }
    }
    if (metrics.population > 0) {
        unsigned digestion = 0;
        unsigned thrift = 0;
        unsigned plant_diet = 0, mixed_diet = 0, animal_diet = 0, meat_specialists = 0;
        std::array<bool, petri::founder_count> founder_alive{};
        for (const auto& creature : world.creatures()) {
            if (creature.alive()) {
                digestion += creature.genes.digestion;
                thrift += creature.genes.thrift;
                founder_alive[creature.founder - 1] = true;
                const auto observed_diet = diet(creature);
                plant_diet += observed_diet == Diet::plant;
                mixed_diet += observed_diet == Diet::mixed;
                animal_diet += observed_diet == Diet::animal;
                meat_specialists += meat_specialist(creature);
            }
        }
        std::cout << " lineages=" << std::count(founder_alive.begin(), founder_alive.end(), true)
                  << " plant_diet=" << plant_diet << " mixed_diet=" << mixed_diet
                  << " animal_diet=" << animal_diet << " meat_specialists=" << meat_specialists
                  << std::fixed << std::setprecision(3)
                  << " mean_digestion=" << double(digestion) / metrics.population
                  << " mean_meat_digestion=" << 16 - double(digestion) / metrics.population
                  << " mean_thrift=" << double(thrift) / metrics.population
                  << std::defaultfloat << std::setprecision(6);
    } else {
        std::cout << " lineages=0 plant_diet=0 mixed_diet=0 animal_diet=0 meat_specialists=0 mean_digestion=NA mean_meat_digestion=NA mean_thrift=NA";
    }
    if (world.sexual_reproduction()) {
        std::uint32_t generation = 0;
        unsigned drive = 0, receptivity = 0;
        for (const auto& creature : world.creatures()) {
            if (creature.alive()) {
                generation = std::max(generation, creature.generation);
                drive += creature.genes.mate_drive;
                receptivity += creature.genes.receptivity;
            }
        }
        std::cout << " sexual=1 max_living_generation=" << generation;
        if (metrics.population) {
            std::cout << " mean_mate_drive=" << double(drive) / metrics.population
                      << " mean_receptivity=" << double(receptivity) / metrics.population;
        } else {
            std::cout << " mean_mate_drive=NA mean_receptivity=NA";
        }
    }
    if (world.nests()) {
        const auto& care = metrics.nests;
        std::cout << " nests=1 guarding=" << world.guarding() << " provisioning=" << world.provisioning()
                  << " fledged=" << care.fledged << " nest_starved=" << care.starved << " nest_killed=" << care.killed
                  << " nest_attacks=" << care.attacks << " guarded_attacks=" << care.guarded_attacks
                  << " guard_ticks=" << care.guard_ticks << " deliveries=" << care.deliveries
                  << " food_delivered=" << care.food_delivered << " both_near=" << care.both_near
                  << " both_alive=" << care.both_alive << " repeated_mates=" << care.repeated_mates
                  << " after_nest_near=" << care.after_nest_near << " after_nest_alive=" << care.after_nest_alive
                  << " previous_mate_alive=" << care.previous_mate_alive
                  << " subsequent_matings=" << care.subsequent_matings
                  << " active_nests=" << (metrics.births - care.fledged - care.starved - care.killed);
        if (metrics.population) {
            unsigned guard = 0, provision = 0;
            for (const auto& creature : world.creatures()) {
                if (creature.alive()) {
                    guard += creature.genes.guard_drive;
                    provision += creature.genes.provision_drive;
                }
            }
            std::cout << " mean_guard_drive=" << double(guard) / metrics.population
                      << " mean_provision_drive=" << double(provision) / metrics.population;
        }
    }
    if (world.connected_biomes()) {
        std::array<unsigned, 3> residents{};
        std::array<unsigned, 3> cells{};
        std::array<unsigned, 3> preferences{};
        for (const auto& cell : world.cells()) {
            ++cells[static_cast<unsigned>(petri::biome_at(cell.moisture))];
        }
        for (const auto& creature : world.creatures()) {
            if (creature.alive()) {
                const auto biome = static_cast<unsigned>(petri::biome_at(world.cells()[creature.cell].moisture));
                ++residents[biome];
                preferences[biome] += creature.genes.preferred_moisture();
            }
        }
        constexpr const char* names[] = {"scrub", "grassland", "wetland"};
        std::cout << " crossings=" << metrics.biome_crossings
                  << " crossing_meals=" << metrics.cross_biome_meals
                  << " crossing_hunts=" << metrics.cross_biome_hunts
                  << " feeding_round_trips=" << metrics.feeding_round_trips
                  << " hunting_round_trips=" << metrics.hunting_round_trips
                  << " biome_shifts=" << metrics.biome_shifts;
        for (unsigned i = 0; i < residents.size(); ++i) {
            std::cout << ' ' << names[i] << "_cells=" << cells[i]
                      << ' ' << names[i] << "_creatures=" << residents[i]
                      << ' ' << names[i] << "_preference=";
            if (residents[i]) {
                std::cout << double(preferences[i]) / residents[i];
            } else {
                std::cout << "NA";
            }
        }
    }
    if (metrics.detritus_clipped) {
        std::cout << " detritus_clipped=" << metrics.detritus_clipped;
    }
    std::cout << '\n';
    if (benchmark) {
        if (elapsed.count() > 0) {
            std::cout << "ticks_per_second=" << (metrics.ticks - initial_tick) / elapsed.count() << ' ';
        }
        std::cout << "world_bytes=" << sizeof(world) << '\n';
    }
    if (save_path && !write_checkpoint(save_path, world)) {
        std::cerr << "Could not write checkpoint to " << save_path << '\n';
        return 1;
    }
    if (birth_target > metrics.births) {
        std::cerr << "Birth target " << birth_target << " was not reached within the tick limit\n";
        return 3;
    }
}
