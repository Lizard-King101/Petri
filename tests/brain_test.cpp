#include "brain.hpp"
#include "observation.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

using namespace petri;
using namespace petri::brain;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::uint32_t random(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

void fill(std::int8_t* values, int count, std::uint32_t& state, int limit = 127) {
    for (int i = 0; i < count; ++i) {
        values[i] = static_cast<std::int8_t>(int(random(state) % (2 * limit + 1)) - limit);
    }
}

Senses random_senses(std::uint32_t& state) {
    Senses senses;
    fill(&senses.direction[0][0], directions * direction_inputs, state);
    fill(senses.self, self_inputs, state);
    return senses;
}

bool same(const Decision& a, const Decision& b) {
    return a.action == b.action && a.sprint == b.sprint && a.breed == b.breed && a.signal == b.signal &&
           a.memory == b.memory;
}

void check_quadrants() {
    int counts[directions] = {};
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if ((!dx && !dy) || std::abs(dx) + std::abs(dy) > radius) {
                continue;
            }
            const int q = quadrant(dx, dy);
            ++counts[q];
            // A quarter turn clockwise carries each quadrant onto the next.
            check(quadrant(-dy, dx) == (q + 1) % directions, "Quadrants are not symmetric under a quarter turn");
        }
    }
    for (const int count : counts) {
        check(count == radius * (radius + 1) / 2, "Quadrants hold different numbers of cells");
    }
    for (int d = 0; d < directions; ++d) {
        check(quadrant(step_x[d], step_y[d]) == d, "A step does not lie in its own quadrant");
    }
}

// The network written out plainly, without the shared self term.
Decision reference(const Senses& senses, const std::int8_t* weights, unsigned turn) {
    const auto clip = [](int value) { return std::clamp(value, -127, 127); };
    int best = 0, action = 0;
    for (int n = 0; n < directions; ++n) {
        const int d = (turn + n) % directions;
        const auto* w = weights;
        int hidden[scorer_hidden];
        for (int h = 0; h < scorer_hidden; ++h) {
            int sum = 0;
            for (int i = 0; i < direction_inputs; ++i) {
                sum += *w++ * senses.direction[d][i];
            }
            for (int i = 0; i < self_inputs; ++i) {
                sum += *w++ * senses.self[i];
            }
            hidden[h] = clip((sum + *w++ * 64) / 64);
        }
        int score = 0;
        for (int h = 0; h < scorer_hidden; ++h) {
            score += *w++ * hidden[h];
        }
        score += *w++ * 64;
        check(w == weights + scorer_bytes, "The scorer's size does not match its layout");
        if (n == 0 || score > best) {
            best = score;
            action = d + 1;
        }
    }
    const auto* w = weights + scorer_bytes;
    int hidden[head_hidden];
    for (int h = 0; h < head_hidden; ++h) {
        int sum = 0;
        for (int i = 0; i < self_inputs; ++i) {
            sum += *w++ * senses.self[i];
        }
        for (int i = 0; i < direction_inputs; ++i) {
            int total = 0;
            for (int d = 0; d < directions; ++d) {
                total += senses.direction[d][i];
            }
            sum += *w++ * (total / directions);
        }
        hidden[h] = clip((sum + *w++ * 64) / 64);
    }
    int out[head_outputs];
    for (int o = 0; o < head_outputs; ++o) {
        int sum = 0;
        for (int h = 0; h < head_hidden; ++h) {
            sum += *w++ * hidden[h];
        }
        out[o] = sum + *w++ * 64;
    }
    check(w == weights + brain_bytes, "The brain's size does not match its layout");
    Decision decision;
    decision.action = static_cast<std::uint8_t>(out[out_stay] >= best ? 0 : action);
    decision.sprint = out[out_sprint] > 0;
    decision.breed = out[out_breed] > 0;
    decision.signal = out[out_signal] > 0;
    decision.memory = {static_cast<std::int8_t>(clip(out[out_memory0] / 64)),
                       static_cast<std::int8_t>(clip(out[out_memory1] / 64))};
    return decision;
}

void check_network() {
    std::uint32_t state = 7;
    int actions[directions + 1] = {};
    for (int trial = 0; trial < 20000; ++trial) {
        std::int8_t weights[brain_bytes];
        // Small weights leave units unsaturated; large ones saturate them and force ties.
        fill(weights, brain_bytes, state, trial % 3 == 0 ? 127 : trial % 3 == 1 ? 24 : 4);
        const auto senses = random_senses(state);
        const unsigned turn = random(state) % directions;
        const auto decision = think(senses, weights, turn);
        check(same(decision, reference(senses, weights, turn)), "The network differs from its reference");
        check(decision.action <= directions, "The network chose an unknown action");
        ++actions[decision.action];

        // Turning the world a quarter turn turns the chosen move with it and changes nothing else.
        Senses turned = senses;
        for (int d = 0; d < directions; ++d) {
            std::copy_n(senses.direction[d], int(direction_inputs), turned.direction[(d + 1) % directions]);
        }
        auto expected = decision;
        if (expected.action) {
            expected.action = static_cast<std::uint8_t>(expected.action % directions + 1);
        }
        check(same(think(turned, weights, (turn + 1) % directions), expected),
              "The network favors a compass direction");
    }
    for (const int count : actions) {
        check(count > 500, "Random brains never chose one of the actions");
    }

    // With nothing to tell the directions apart, the first one examined is kept.
    std::int8_t weights[brain_bytes] = {};
    weights[scorer_bytes - 1] = 1; // The move score's bias: every move beats staying.
    Senses blank{};
    for (unsigned turn = 0; turn < directions; ++turn) {
        check(think(blank, weights, turn).action == turn + 1, "A tie did not keep the first direction examined");
    }
    weights[scorer_bytes - 1] = 0;
    check(think(blank, weights, 0).action == 0, "A silent brain should stay put");
}

void check_senses() {
    World world(14, observatory_config());
    for (int tick = 0; tick < 3000; ++tick) {
        world.step();
    }
    const Sight sight(world);
    int seen = 0, walls = 0;
    for (int slot = 0; slot < capacity; ++slot) {
        const auto& me = world.creatures()[slot];
        if (!me.alive()) {
            continue;
        }
        check(Sight::test(sight.occupied, me.cell), "Sight missed a creature");
        const auto senses = sense(world, sight, slot);
        const int x = me.cell % width, y = me.cell / width;
        int crowd[directions] = {}, meat[directions] = {}, nearest[directions] = {};
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int distance = std::abs(dx) + std::abs(dy), xx = x + dx, yy = y + dy;
                if (!distance || distance > radius || xx < 0 || xx >= width || yy < 0 || yy >= height) {
                    continue;
                }
                const int cell = yy * width + xx, d = quadrant(dx, dy), close = radius + 1 - distance;
                meat[d] += world.cells()[cell].animal * close;
                if (world.occupant(cell) >= 0) {
                    crowd[d] += close;
                    nearest[d] = std::max(nearest[d], close);
                }
            }
        }
        const int to_edge[directions] = {y, width - 1 - x, height - 1 - y, x};
        for (int d = 0; d < directions; ++d) {
            const auto& in = senses.direction[d];
            check(in[in_crowd] == std::min(127, crowd[d] * 4), "The crowd sense is wrong");
            check(in[in_meat] == std::min(127, meat[d]), "The meat sense is wrong");
            check(in[in_near] == nearest[d] * 18, "The nearest-creature sense is wrong");
            check((in[in_wall] != 0) == (to_edge[d] < radius), "The wall sense is wrong");
            int step_food = 0;
            if (to_edge[d] > 0) {
                const int next = (y + step_y[d]) * width + x + step_x[d];
                step_food = std::min(127, (world.cells()[next].plants * me.genes.digestion +
                                           world.fruit_at(next) * (me.genes.digestion + 8)) >> 1);
            }
            check(in[in_step] == step_food, "The next-cell food sense is wrong");
            check(in[in_near] || (!in[in_size] && !in[in_resemblance]), "An absent creature has traits");
            check(in[in_signal] == 0, "A signal was sensed though none was flashed");
            seen += in[in_near] != 0;
            walls += in[in_wall] != 0;
        }
        check(senses.self[self_energy] == std::min(127, me.energy * 3 / 4), "The energy sense is wrong");
    }
    check(seen > 0, "No creature saw another");
    (void)walls;

    // A flash is sensed by neighbors and only in its own direction.
    for (int slot = 0; slot < capacity; ++slot) {
        const auto& me = world.creatures()[slot];
        if (!me.alive()) {
            continue;
        }
        const auto before = sense(world, sight, slot);
        for (int d = 0; d < directions; ++d) {
            if (!before.direction[d][in_near]) {
                continue;
            }
            Sight lit = sight;
            for (int other = 0; other < capacity; ++other) {
                const auto& them = world.creatures()[other];
                if (them.alive() && other != slot) {
                    const int dx = them.cell % width - me.cell % width, dy = them.cell / width - me.cell / width;
                    if (std::abs(dx) + std::abs(dy) <= radius && quadrant(dx, dy) == d) {
                        lit.flash(them.cell);
                    }
                }
            }
            const auto after = sense(world, lit, slot);
            for (int e = 0; e < directions; ++e) {
                check(after.direction[e][in_signal] == (e == d ? before.direction[d][in_crowd] : 0),
                      "A flash was sensed in the wrong direction");
            }
            return;
        }
    }
}

void check_inheritance() {
    std::uint32_t state = 99, rng = 5;
    std::int8_t parent[brain_bytes], child[brain_bytes];
    fill(parent, brain_bytes, state);
    long changes = 0;
    constexpr int births = 4000;
    for (int birth = 0; birth < births; ++birth) {
        const int reported = inherit(child, parent, rng);
        int differing = 0;
        for (int i = 0; i < brain_bytes; ++i) {
            const int step = std::abs(child[i] - parent[i]);
            check(step <= 4 && child[i] >= -127, "A weight mutated out of bounds");
            differing += step != 0;
        }
        check(differing == reported, "The mutation count is wrong");
        changes += differing;
    }
    // One weight in 128: about 1.85 changes per brain of this size.
    const long expected = long(births) * brain_bytes / 128;
    check(changes > expected * 8 / 10 && changes < expected * 12 / 10, "The mutation rate is off");

    unsigned turns[directions] = {};
    for (CreatureId id = 1; id <= 4000; ++id) {
        ++turns[turn_for(id, 1234)];
        check(turn_for(id, 77) == turn_for(id, 77), "The first direction is not repeatable");
    }
    for (const auto count : turns) {
        check(count > 850 && count < 1150, "The first direction is not evenly spread");
    }
}

} // namespace

int main() {
    static_assert(brain_bytes == 247 && brain_words == 62);
    check_quadrants();
    check_network();
    check_senses();
    check_inheritance();
}
