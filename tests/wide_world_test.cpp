#include "checkpoint_compat.hpp"
#include "observation.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void check_wide_world() {
    static_assert(petri::width == 128 && petri::height == 64);
    static_assert(petri::panel_width == 64 && petri::panel_height == 32);
    static_assert(petri::Frame{}.size() == 2048);
    const petri::Config config{24, true, 1, 0, true, true};
    constexpr int amber = 0xbba4;
    for (const unsigned seed : {1u, 4u, 14u, 42u}) {
        petri::World world(seed, config);
        unsigned peak = 0;
        for (int tick = 1; tick <= 20000; ++tick) {
            world.step();
            peak = std::max(peak, world.metrics().population);
            if (tick != 256 && tick != 20000) {
                continue;
            }

            petri::Frame frame{};
            petri::render_life(world, frame);
            std::array<bool, petri::width * petri::height> occupied{};
            unsigned touching = 0;
            for (const auto& creature : world.creatures()) {
                if (creature.alive()) {
                    check(creature.cell < occupied.size(), "Wide world has an out-of-bounds creature");
                    check(!occupied[creature.cell], "Wide world put two creatures on the same cell");
                    occupied[creature.cell] = true;
                    const int x = creature.cell % petri::width / 2;
                    const int y = creature.cell / petri::width / 2;
                    check(frame[y * petri::panel_width + x] == amber, "Wide overview lost a creature");
                }
            }
            for (const auto& creature : world.creatures()) {
                if (creature.alive()) {
                    const int x = creature.cell % petri::width;
                    const int y = creature.cell / petri::width;
                    touching += (x > 0 && occupied[creature.cell - 1]) ||
                        (x + 1 < petri::width && occupied[creature.cell + 1]) ||
                        (y > 0 && occupied[creature.cell - petri::width]) ||
                        (y + 1 < petri::height && occupied[creature.cell + petri::width]);
                }
            }
            const auto population = world.metrics().population;
            check(population >= 20 && touching * 4 < population, "Wide demo became sparse or crowded");
        }
        check(peak < petri::capacity && world.metrics().capacity_blocks == 0,
              "The creature pool limited the wide demo");
        check(world.metrics().births > 0, "The wide demo did not reproduce");
    }
}

void check_connected_biomes() {
    const petri::Config config{24, true, 1, 0, true, true, true};
    petri::World world(14, config);
    constexpr int cells = petri::width * petri::height;
    std::array<bool, cells> reached{};
    std::array<int, cells> queue{};
    unsigned tail = 1;
    queue[0] = 10 * petri::width + 16;
    reached[queue[0]] = true;
    for (unsigned head = 0; head < tail; ++head) {
        const int index = queue[head];
        const int x = index % petri::width;
        const int y = index / petri::width;
        const int neighbors[] = {
            x > 0 ? index - 1 : -1,
            x + 1 < petri::width ? index + 1 : -1,
            y > 0 ? index - petri::width : -1,
            y + 1 < petri::height ? index + petri::width : -1
        };
        for (const auto next : neighbors) {
            if (next >= 0 && !reached[next] &&
                petri::biome_at(world.cells()[next].moisture) != petri::Biome::scrub) {
                reached[next] = true;
                queue[tail++] = next;
            }
        }
    }
    for (int y = 10; y < petri::height; y += 21) {
        for (int x = 16; x < petri::width; x += 32) {
            check(reached[y * petri::width + x], "A wetland center is cut off from the corridor network");
        }
    }

    constexpr auto budget = 24 * cells + petri::founder_count * petri::body_mass;
    struct PreviousWetland {
        std::uint64_t id = 0;
        int region = -1;
    };
    std::array<PreviousWetland, petri::capacity> previous{};
    unsigned wetland_transfers = 0;
    for (int tick = 0; tick < 2000; ++tick) {
        world.step();
        std::uint64_t material = world.metrics().population * petri::body_mass;
        for (const auto& cell : world.cells()) {
            material += cell.nutrients + cell.plants + cell.detritus + cell.animal;
        }
        for (std::size_t slot = 0; slot < previous.size(); ++slot) {
            const auto& creature = world.creatures()[slot];
            auto& last = previous[slot];
            if (last.id != creature.id) {
                last = {creature.id, -1};
            }
            if (creature.alive()) {
                if (petri::biome_at(world.cells()[creature.cell].moisture) == petri::Biome::wetland) {
                    const int region = (creature.cell / petri::width / 21) * 4 +
                        creature.cell % petri::width / 32;
                    wetland_transfers += last.region >= 0 && last.region != region;
                    last.region = region;
                }
                check(creature.genes.moisture_preference >= petri::Genome::minimum &&
                      creature.genes.moisture_preference <= petri::Genome::maximum,
                      "Invalid heritable moisture preference");
                material += creature.nutrients_carried;
            }
        }
        check(material == budget, "Biome dynamics changed the amount of material");
    }
    check(wetland_transfers > 0, "Creatures did not use the connected habitats");
    const auto& metrics = world.metrics();
    check(metrics.biome_shifts > 0 && metrics.biome_crossings >= metrics.cross_biome_meals &&
          metrics.cross_biome_meals > 0 && metrics.feeding_round_trips > 0 &&
          metrics.hunting_round_trips > 0 && metrics.hunting_round_trips <= metrics.kills &&
          metrics.capacity_blocks == 0, "Expected feeding visits, successful hunts, and shifting biomes");

    petri::Checkpoint snapshot{};
    world.save_checkpoint(snapshot);
    petri::World restored(0);
    check(restored.load_checkpoint(snapshot) && restored.connected_biomes(), "Biome checkpoint failed to load");
    for (int tick = 0; tick < 500; ++tick) {
        world.step();
        restored.step();
    }
    check(world.fingerprint() == restored.fingerprint(), "Connected-biome replay diverged");
}

void check_motion() {
    check(petri::interpolate_axis(16, 24, 0, 550000) == 16, "Motion missed the departure cell");
    check(petri::interpolate_axis(16, 24, 275000, 550000) == 20, "Motion skipped the midpoint");
    check(petri::interpolate_axis(24, 16, 275000, 550000) == 20, "Reverse motion skipped the midpoint");
    check(petri::interpolate_axis(16, 24, 900000, 550000) == 24, "Motion overshot the destination");
    check(petri::interpolate_axis(16, 24, 0, 0) == 24, "Instant placement stopped in transit");
}

void check_panel_camera() {
    petri::World world(14, petri::observatory_config());
    petri::LocalPanel panel(world);
    petri::PanelInput input;
    const auto send = [&](const char* bytes) {
        bool requested = false;
        while (*bytes) {
            requested |= input.feed(*bytes++, panel, 1000);
        }
        return requested;
    };
    panel.draw(0, 0, 0);
    const auto followed = panel.camera().follow;
    check(followed != 0 && panel.camera().view == petri::PanelView::close,
          "Standalone sprites should follow a living creature");
    check(send("!1 2 0 0 0 0\n?"), "A camera command lost the frame request");
    panel.draw(1000, 1000000, 0);
    petri::Frame overview;
    petri::render_life(world, overview);
    check(input.revision() == 1 && panel.frame() == overview && panel.camera().follow == 0,
          "Fit must restore the complete overview and clear follow");
    send("!2 0 4 65535 65535 0\n");
    panel.draw(1000, 1000000, 0);
    check(panel.camera().left == petri::scene_width(8) - petri::panel_width &&
          panel.camera().top == petri::height * 8 - petri::panel_height,
          "Sprite camera must stay inside the world");
    send("!3 0 5 65535 65535 0\n");
    panel.draw(1000, 1000000, 0);
    check(input.revision() == 3 && panel.camera().zoom == petri::detail_zoom &&
          panel.camera().left == petri::scene_width(16) - petri::panel_width &&
          panel.camera().top == petri::height * 16 - petri::panel_height,
          "Detail commands must reach the 16-pixel view and clamp the viewport");
    send("!4 0 6 0 0 0\n!4 2 5 0 0 0\n");
    check(input.revision() == 3, "Invalid sprite zoom must leave the camera unchanged");
    const auto follow_cmd = "!4 0 5 0 0 " + std::to_string(followed) + "\n";
    send(follow_cmd.c_str());
    panel.draw(1000, 1000000, 0);
    const auto pan_start = panel.camera();
    panel.draw(1500, 1500000, 0);
    check(panel.camera().left != pan_start.left || panel.camera().top != pan_start.top,
          "A distant follow target should start a smooth camera pan");
    panel.draw(3000, 3000000, 0);
    const auto target = std::find_if(world.creatures().begin(), world.creatures().end(),
        [&](const auto& creature) { return creature.id == followed; });
    const auto point = panel.location(target - world.creatures().begin());
    check(input.revision() == 4 && point.x >= 8 && point.x <= petri::panel_width - 8 &&
          point.y >= 8 && point.y <= petri::panel_height - 8, "The followed detail sprite must fit in the frame");
    panel.set_camera({petri::PanelView::close, petri::sprite_zoom, 0, 0, followed}, 3000);
    panel.cycle_view(3000);
    panel.draw(3000, 3000000, 0);
    check(panel.camera().zoom == petri::detail_zoom && panel.camera().follow == followed,
          "The physical view control should keep the subject when zooming in");
    panel.cycle_view(3000);
    panel.draw(3000, 3000000, 0);
    check(panel.camera().view == petri::PanelView::hunt && panel.camera().follow == 0,
          "The physical view control should take effect after remote follow");
}

void check_zoom_layers() {
    auto config = petri::observatory_config();
    config.initial_carrion_percent = 40;
    const petri::World world(14, config);
    const auto target = std::find_if(world.creatures().begin(), world.creatures().end(), [&](const auto& creature) {
        const auto& cell = world.cells()[creature.cell];
        return creature.alive() && cell.plants && cell.animal;
    });
    check(target != world.creatures().end(), "Zoom fixture needs a live creature sharing plants and carrion");

    petri::LocalPanel panel(world);
    constexpr auto carrion = petri::rgb565(160, 20, 120);
    std::uint16_t body = 0, coat = 0;
    std::uint32_t now = 0;
    for (const auto zoom : {3, 2, 4, 5}) {
        now += 3000;
        const auto view = zoom >= 4 ? petri::PanelView::close : petri::PanelView::life;
        panel.set_camera({view, static_cast<std::uint8_t>(zoom), 0, 0, target->id}, now);
        panel.draw(now, now * 1000, 0);
        panel.draw(now + 2000, (now + 2000) * 1000, 0);
        const int tile = 1 << (zoom - 1);
        const auto origin = petri::cell_origin(target->cell, tile);
        const int px = origin[0] - panel.camera().left;
        const int py = origin[1] - panel.camera().top;
        const auto pixel = [&](int x, int y) { return panel.frame()[(py + y) * petri::panel_width + px + x]; };
        const auto count = [&](std::uint16_t color) {
            int matches = 0;
            for (int y = 0; y < tile; ++y) {
                for (int x = 0; x < tile; ++x) {
                    matches += pixel(x, y) == color;
                }
            }
            return matches;
        };
        if (tile == 4) {
            body = pixel(2, 2);
            coat = pixel(1, 1);
            check(body != coat && count(body) == 3 && count(coat) == 1, "4× live sprite must occupy 2×2 pixels");
            check(pixel(0, 3) == carrion && pixel(1, 3) == carrion && count(carrion) == 2,
                  "4× carrion must occupy two pixels on the bottom row");
            check(pixel(2, 3) != pixel(0, 1) && pixel(2, 3) != body && pixel(2, 3) != carrion,
                  "4× plants must remain visible below the creature and beside carrion");
        } else if (tile == 2) {
            check(pixel(0, 1) == carrion && count(carrion) == 1 && pixel(1, 1) == body && count(body) == 1,
                  "2× live sprites and carrion must occupy separate right and left pixels");
        } else {
            check(count(body) > 0 && count(coat) > 0, "Close sprites must share the 4× body and coat colors");
            check(tile == 8 ? count(coat) <= 2 : count(coat) > 2, "8× coat marks must use fewer pixels than 16×");
        }
    }
}

void check_dual_grid() {
    for (const int tile : {4, 8, 16}) {
        for (unsigned mask = 0; mask < 16; ++mask) {
            check(petri::dual_coverage(mask, 0, 0, tile) == bool(mask & 1) &&
                  petri::dual_coverage(mask, tile - 1, 0, tile) == bool(mask & 2) &&
                  petri::dual_coverage(mask, 0, tile - 1, tile) == bool(mask & 4) &&
                  petri::dual_coverage(mask, tile - 1, tile - 1, tile) == bool(mask & 8),
                  "A dual-grid tile must match the terrain at all four cell centers");
            for (unsigned neighbor = 0; neighbor < 16; ++neighbor) {
                for (int i = 0; i < tile; ++i) {
                    if ((mask & 10) >> 1 == (neighbor & 5)) {
                        check(petri::dual_coverage(mask, tile - 1, i, tile) == petri::dual_coverage(neighbor, 0, i, tile),
                              "Dual-grid tiles must join without vertical seams");
                    }
                    if ((mask & 12) >> 2 == (neighbor & 3)) {
                        check(petri::dual_coverage(mask, i, tile - 1, tile) == petri::dual_coverage(neighbor, i, 0, tile),
                              "Dual-grid tiles must join without horizontal seams");
                    }
                }
                if ((mask & neighbor) == mask) {
                    for (int y = 0; y < tile; ++y) {
                        for (int x = 0; x < tile; ++x) {
                            check(!petri::dual_coverage(mask, x, y, tile) || petri::dual_coverage(neighbor, x, y, tile),
                                  "Adding terrain corners must preserve existing coverage between biome layers");
                        }
                    }
                }
            }
        }
        check(!petri::dual_coverage(1, tile / 2 - 1, tile / 2 - 1, tile),
              "Isolated terrain must have rounded corners");
        check(!petri::dual_coverage(6, tile / 2, tile / 2, tile) &&
              !petri::dual_coverage(9, tile / 2, tile / 2, tile),
              "Diagonal terrain corners must keep their gap");
    }
}

void check_noisy_edges() {
    for (const int tile : {4, 8, 16}) {
        bool perturbed = false;
        for (const int row : {-1, 0, 7, 22, 63}) {
            for (const int col : {-1, 0, 8, 37, 127}) {
                const auto covered = [&](unsigned mask, int x, int y, int cx, int cy) {
                    const auto sample = petri::terrain_sample(cx, cy, x, y, tile);
                    return petri::dual_coverage_at(mask, sample[0], sample[1], 16);
                };
                for (unsigned mask = 0; mask < 16; ++mask) {
                    check(covered(mask, 0, 0, col, row) == bool(mask & 1) &&
                          covered(mask, tile - 1, 0, col, row) == bool(mask & 2) &&
                          covered(mask, 0, tile - 1, col, row) == bool(mask & 4) &&
                          covered(mask, tile - 1, tile - 1, col, row) == bool(mask & 8),
                          "Noisy terrain must preserve cell centers");
                    for (unsigned neighbor = 0; neighbor < 16; ++neighbor) {
                        for (int i = 0; i < tile; ++i) {
                            if ((mask & 10) >> 1 == (neighbor & 5)) {
                                check(covered(mask, tile - 1, i, col, row) == covered(neighbor, 0, i, col + 1, row),
                                      "Noisy boundaries must meet across vertical tile edges");
                            }
                            if ((mask & 12) >> 2 == (neighbor & 3)) {
                                check(covered(mask, i, tile - 1, col, row) == covered(neighbor, i, 0, col, row + 1),
                                      "Noisy boundaries must meet across horizontal tile edges");
                            }
                        }
                    }
                    std::array<bool, 256> visited{};
                    std::array<int, 256> queue{};
                    unsigned components = 0;
                    for (int y = 0; y < tile; ++y) {
                        for (int x = 0; x < tile; ++x) {
                            const bool land = covered(mask, x, y, col, row);
                            if (mask == 3 || mask == 5) {
                                perturbed |= land != petri::dual_coverage(mask, x, y, tile);
                            }
                            if (!land || visited[y * tile + x]) {
                                continue;
                            }
                            ++components;
                            queue[0] = y * tile + x;
                            visited[queue[0]] = true;
                            unsigned tail = 1;
                            for (unsigned head = 0; head < tail; ++head) {
                                const int px = queue[head] % tile, py = queue[head] / tile;
                                const std::array<std::array<int, 2>, 4> adjacent{{
                                    {px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1},
                                }};
                                for (const auto& next : adjacent) {
                                    const int nx = next[0], ny = next[1];
                                    if (nx >= 0 && nx < tile && ny >= 0 && ny < tile &&
                                            !visited[ny * tile + nx] && covered(mask, nx, ny, col, row)) {
                                        visited[ny * tile + nx] = true;
                                        queue[tail++] = ny * tile + nx;
                                    }
                                }
                            }
                        }
                    }
                    check(components == (mask == 0 ? 0u : mask == 6 || mask == 9 ? 2u : 1u),
                          "Edge noise must not add islands or join diagonal corners");
                }
            }
        }
        check(perturbed, "Edge noise must visibly break straight boundaries at each textured zoom");
    }
}

void check_terrain_projection() {
    petri::World world(14, petri::observatory_config());
    for (int tick = 0; tick < 256; ++tick) {
        world.step();
    }
    const auto subject = std::find_if(world.creatures().begin(), world.creatures().end(), [](const auto& creature) {
        const int x = creature.cell % petri::width, y = creature.cell / petri::width;
        return creature.alive() && y % 2 && x > 8 && x < petri::width - 9 && y > 4 && y < petri::height - 5;
    });
    check(subject != world.creatures().end(), "Terrain fixture needs an interior creature on an odd row");
    const auto fingerprint = world.fingerprint();
    for (const int zoom : {3, 4, 5}) {
        const int tile = 1 << (zoom - 1);
        const int left = subject->cell % petri::width * tile - petri::panel_width / 2;
        const int top = subject->cell / petri::width * tile - petri::panel_height / 2;
        petri::LocalPanel panel(world);
        const auto place = [&](int x, int y) {
            panel.set_camera({zoom == 3 ? petri::PanelView::life : petri::PanelView::close,
                static_cast<std::uint8_t>(zoom), static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)}, 0);
            panel.draw(0, 0, 0);
        };
        place(left, top);
        const auto point = panel.location(subject - world.creatures().begin());
        check(point.x == petri::panel_width / 2 + tile / 2 && point.y == petri::panel_height / 2 + tile / 2,
              "Picking coordinates must stay on the regular world grid on odd rows");
        const auto before = panel.frame();
        place(left + 1, top + 1);
        for (int y = 0; y < petri::panel_height - 1; ++y) {
            for (int x = 0; x < petri::panel_width - 1; ++x) {
                check(panel.frame()[y * petri::panel_width + x] == before[(y + 1) * petri::panel_width + x + 1],
                      "Ground texture and sprites must stay fixed to the world while panning");
            }
        }
    }
    check(world.fingerprint() == fingerprint, "Terrain rendering changed the simulation state");
}

void check_fruit_detail() {
    petri::World world(14, petri::observatory_config());
    int subject = -1;
    for (int tick = 0; tick < 128 && subject < 0; ++tick) {
        world.step();
        for (int index = 0; index < petri::width * petri::height; ++index) {
            if (world.fruit_at(index) && std::none_of(world.creatures().begin(), world.creatures().end(),
                    [&](const auto& creature) { return creature.alive() && creature.cell == index; })) {
                subject = index;
                break;
            }
        }
    }
    check(subject >= 0, "No ripe fruit to verify on the panel");
    constexpr auto fruit_color = petri::rgb565(248, 116, 24);
    petri::LocalPanel panel(world);
    const auto before = world.fingerprint();
    for (const int zoom : {3, 4, 5}) {
        const int tile = 1 << (zoom - 1);
        const auto origin = petri::cell_origin(subject, tile);
        const int left = std::clamp(origin[0] - petri::panel_width / 2,
                                    0, petri::scene_width(tile) - petri::panel_width);
        const int top = std::clamp(origin[1] - petri::panel_height / 2,
                                   0, petri::height * tile - petri::panel_height);
        panel.set_camera({zoom == 3 ? petri::PanelView::habitat : petri::PanelView::close,
            static_cast<std::uint8_t>(zoom), static_cast<std::uint16_t>(left), static_cast<std::uint16_t>(top)}, 0);
        panel.draw(0, 0, 0);
        const int px = origin[0] - left;
        const int py = origin[1] - top;
        bool visible = false;
        for (int y = 0; y < tile; ++y) {
            for (int x = 0; x < tile; ++x) {
                visible |= panel.frame()[(py + y) * petri::panel_width + px + x] == fruit_color;
            }
        }
        check(visible, "Ripe fruit must be visible in the zoomed-in plant view");
    }
    check(world.fingerprint() == before, "Rendering ripe fruit changed the biology");
}

void check_dynamic_environment() {
    auto config = petri::observatory_config();
    petri::World world(14, config);
    config.dynamic_environment = false;
    const petri::World initial(14, config);
    for (std::size_t i = 0; i < world.cells().size(); ++i) {
        const auto& a = world.cells()[i];
        const auto& b = initial.cells()[i];
        check(a.moisture == b.moisture && a.plants == b.plants && a.nutrients == b.nutrients &&
              world.fruiting_shrub(i) == initial.fruiting_shrub(i), "Weather changed the matched starting ecology");
    }
    unsigned established = 0;
    for (int tick = 0; tick < 5114; ++tick) {
        world.step();
        std::uint64_t material = world.metrics().population * petri::body_mass;
        for (std::size_t i = 0; i < world.cells().size(); ++i) {
            const auto& cell = world.cells()[i];
            material += cell.nutrients + cell.plants + cell.detritus + cell.animal + world.fruit_at(i);
            established += !initial.fruiting_shrub(i) && world.fruiting_shrub(i);
        }
        for (const auto& creature : world.creatures()) {
            material += creature.nutrients_carried + creature.provisions();
        }
        check(material == 24 * world.cells().size() + petri::founder_count * petri::body_mass,
              "Dieback or seed dispersal changed the material budget");
    }
    check(world.dynamic_environment() && world.staggered_growth() && established,
          "The environment must enable staggered updates and let shrubs establish new sites");

    petri::Checkpoint bytes{};
    world.save_checkpoint(bytes);
    petri::World restored(42);
    check(restored.load_checkpoint(bytes) && restored.dynamic_environment(), "Weather checkpoint was rejected");
    for (int tick = 0; tick < 256; ++tick) {
        world.step();
        restored.step();
        check(world.fingerprint() == restored.fingerprint(), "Weather replay diverged across a new weather target");
    }
}

void check_death_presentation() {
    for (const auto zoom : {petri::sprite_zoom, petri::detail_zoom}) {
        petri::World world(42, {0, true, 1});
        petri::LocalPanel panel(world);
        panel.set_camera({petri::PanelView::close, static_cast<std::uint8_t>(zoom), 0, 0, 1}, 0);
        panel.draw(0, 0, 3);
        std::uint32_t now = 0;
        while (now < 20000 && world.creatures()[0].id == 1) {
            now += 125;
            world.step(panel.before_step(now));
            panel.after_step(now * 1000, 125000);
            panel.draw(now, now * 1000, 3);
        }
        check(panel.camera().follow == 1, "Follow must hold on a creature when it dies");
        const auto before = world.fingerprint();
        const auto standing = panel.frame();
        panel.draw(now + 500, (now + 500) * 1000, 3);
        check(panel.frame() != standing && panel.camera().follow == 1, "A death must animate before follow releases");
        panel.draw(now + 2000, (now + 2000) * 1000, 3);
        check(panel.camera().follow == 0, "A manually followed energy death must release after its visual hold");
        check(world.fingerprint() == before, "Rendering a death changed simulation state");
    }
}

void check_attack_presentation() {
    struct Pair {
        std::uint64_t attacker = 0, target = 0;
        bool landed = false;
    };
    for (const bool landed : {false, true}) {
        petri::World world(42, {80, true, 1, 0, true, true});
        auto before = world;
        Pair pair;
        petri::StepObserver observer;
        observer.context = &pair;
        observer.record_attack = [](void* context, const petri::Creature& attacker,
                                    const petri::Creature& target, bool hit) {
            auto& pair = *static_cast<Pair*>(context);
            if (!pair.attacker) {
                pair = {attacker.id, target.id, hit};
            }
        };
        const auto alive = [&](std::uint64_t id) {
            return std::any_of(world.creatures().begin(), world.creatures().end(),
                [id](const petri::Creature& creature) { return creature.id == id; });
        };
        bool found = false;
        for (int tick = 0; tick < 3000 && !found; ++tick) {
            before = world;
            pair = {};
            world.step(observer);
            found = pair.attacker && pair.landed == landed && alive(pair.attacker) && alive(pair.target) != landed;
        }
        check(found, "No matching attack fixture found");
        world = before;
        petri::LocalPanel panel(world);
        panel.set_camera({petri::PanelView::close, petri::detail_zoom, 0, 0, pair.target}, 0);
        panel.draw(0, 0, 3);
        world.step(panel.before_step(1000));
        panel.after_step(1000000, 125000);
        panel.draw(1000, 1000000, 3);
        const auto windup = panel.frame();
        panel.draw(1220, 1220000, 3);
        check(panel.frame() != windup && panel.camera().follow == pair.target,
              "A strike must show a visible interaction while retaining the target");
        panel.draw(2800, 2800000, 3);
        check(panel.camera().follow == pair.target, "The death scene ended before its hold completed");
        const auto held = panel.camera();
        panel.draw(3000, 3000000, 3);
        check(panel.camera().follow == (landed ? pair.attacker : pair.target),
              "Only a lethal strike should hand follow to the attacker");
        if (landed) {
            check(panel.camera().left == held.left && panel.camera().top == held.top,
                  "Handing follow to an attacker must begin at the held camera position");
            panel.draw(5000, 5000000, 3);
            const auto attacker = std::find_if(world.creatures().begin(), world.creatures().end(),
                [&](const auto& creature) { return creature.id == pair.attacker; });
            const auto point = panel.location(attacker - world.creatures().begin());
            check(point.x >= 0 && point.y >= 0, "The attacker must come into frame after the pan");
        }
    }
}

} // namespace

void check_older_checkpoint() {
    // The observatory world saved as version 15 (160 slots, 64-bit fields) resumes exactly.
    auto original = std::make_unique<petri::World>(14, petri::observatory_config());
    for (int tick = 0; tick < 1500; ++tick) {
        original->step();
    }
    auto current = std::make_unique<petri::Checkpoint>();
    original->save_checkpoint(*current);
    const auto version16 = petri::compat::to_version16(*current);
    check(version16 != nullptr, "The observatory test world used morphology");
    const auto old = petri::compat::to_version15(*version16);
    check(old != nullptr, "The observatory test world used a slot past 160");
    auto restored = std::make_unique<petri::World>(1);
    check(restored->load_checkpoint(*old), "A version 15 observatory checkpoint was rejected");
    auto again = std::make_unique<petri::Checkpoint>();
    restored->save_checkpoint(*again);
    check(*again == *current, "A version 15 observatory checkpoint did not re-save identically");
    for (int tick = 0; tick < 500; ++tick) {
        original->step();
        restored->step();
    }
    check(restored->fingerprint() == original->fingerprint(), "An observatory run resumed from version 15 diverged");
}

int main() {
    check_wide_world();
    check_older_checkpoint();
    check_connected_biomes();
    check_motion();
    check_panel_camera();
    check_zoom_layers();
    check_dual_grid();
    check_noisy_edges();
    check_terrain_projection();
    check_fruit_detail();
    check_dynamic_environment();
    check_death_presentation();
    check_attack_presentation();
}
