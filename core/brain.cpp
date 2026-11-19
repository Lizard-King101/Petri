#include "brain.hpp"

namespace petri::brain {
namespace {

constexpr int quadrant_cells = radius * (radius + 1) / 2;

// One quadrant's cells with their closeness. Grouping the scan by quadrant
// keeps its running sums in registers.
struct Offset {
    std::int8_t dx, dy;
    std::uint8_t close;
};

constexpr std::array<std::array<Offset, quadrant_cells>, directions> make_offsets() {
    std::array<std::array<Offset, quadrant_cells>, directions> table{};
    int count[directions] = {};
    for (int dy = -radius; dy <= radius; ++dy) {
        const int span = radius - (dy < 0 ? -dy : dy);
        for (int dx = -span; dx <= span; ++dx) {
            if (dx || dy) {
                const int distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                const int d = quadrant(dx, dy);
                table[d][count[d]++] = {static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dy),
                                        static_cast<std::uint8_t>(radius + 1 - distance)};
            }
        }
    }
    return table;
}

constexpr auto offsets = make_offsets();

constexpr int clip(int value) {
    return value < -127 ? -127 : value > 127 ? 127 : value;
}

constexpr std::int8_t input(int value) {
    return static_cast<std::int8_t>(clip(value));
}

// A unit's activation from its accumulator, where 64 is unity. The quotient
// truncates toward zero, which is the same on every compiler.
constexpr int activation(int sum) {
    return clip(sum / 64);
}

constexpr int magnitude(int value) {
    return value < 0 ? -value : value;
}

std::uint32_t next(std::uint32_t& state) {
    std::uint32_t value = (state += 0x9e3779b9u);
    value = (value ^ (value >> 16)) * 0x85ebca6bu;
    value = (value ^ (value >> 13)) * 0xc2b2ae35u;
    return value ^ (value >> 16);
}

} // namespace

int kinship(const Creature& me, const Creature& them) {
    if (me.has_parent(them.id) || them.has_parent(me.id)) {
        return kin_parent_or_child;
    }
    if (me.close_kin(them) || (me.last_mate && me.last_mate == them.id) || (them.last_mate && them.last_mate == me.id)) {
        return kin_sibling_or_mate;
    }
    return me.genes.faction && me.genes.faction == them.genes.faction ? kin_same_marker : 0;
}

Sight::Sight(const World& world) {
    for (const auto& creature : world.creatures()) {
        if (creature.alive()) {
            occupied[creature.cell >> 5] |= 1u << (creature.cell & 31);
        }
    }
}

Senses sense(const World& world, const Sight& sight, int slot, const Inner& inner) {
    const auto& creatures = world.creatures();
    const auto& cells = world.cells();
    const auto& me = creatures[slot];
    const int x = me.cell % width, y = me.cell / width;
    const int digestion = me.genes.digestion, fruit_value = digestion + 8;
    const int preferred = me.genes.preferred_moisture();
    const auto& here = cells[me.cell];
    const int mismatch = magnitude(preferred - here.moisture);

    Senses senses{};
    const bool interior = x >= radius && x < width - radius && y >= radius && y < height - radius;
    const int to_edge[directions] = {y, width - 1 - x, height - 1 - y, x};
    for (int d = 0; d < directions; ++d) {
        // Sums over the quadrant, weighted by closeness. The creature's own values
        // (digestion, its present mismatch) are applied once afterwards.
        int foliage = 0, fruit = 0, meat = 0, crowd = 0, signal = 0, mismatches = 0, weight = 0;
        int nearest = 0, nearest_slot = -1;
        for (const auto& at : offsets[d]) {
            const int xx = x + at.dx, yy = y + at.dy;
            if (!interior && (xx < 0 || xx >= width || yy < 0 || yy >= height)) {
                continue;
            }
            const int close = at.close;
            const int index = yy * width + xx;
            const auto& cell = cells[index];
            const int stock = world.shrub_stock(index);
            foliage += cell.plants * close;
            fruit += (stock - (stock != 0)) * close;
            meat += cell.animal * close;
            mismatches += magnitude(preferred - cell.moisture) * close;
            weight += close;
            if (Sight::test(sight.occupied, index)) {
                crowd += close;
                signal += Sight::test(sight.flashing, index) ? close : 0;
                const int other = world.occupant(index);
                // The closest creature; at equal distance, the lower slot.
                if (close > nearest || (close == nearest && other < nearest_slot)) {
                    nearest = close;
                    nearest_slot = other;
                }
            }
        }
        auto& out = senses.direction[d];
        // Digestion's standard is 8, so plants / 2 at that standard.
        out[in_plants] = input((foliage * digestion + fruit * fruit_value) >> 4);
        out[in_meat] = input(meat);
        out[in_near] = input(nearest * 18);
        if (nearest_slot >= 0) {
            const auto& them = creatures[nearest_slot];
            out[in_size] = input((int(them.genes.body_size) - int(me.genes.body_size)) * 8);
            out[in_kin] = input(kinship(me, them));
        }
        out[in_crowd] = input(crowd * 4);
        out[in_signal] = input(signal * 4);
        out[in_habitat] = input((mismatch * weight - mismatches) / 64);
        out[in_wall] = input(to_edge[d] < radius ? (radius - to_edge[d]) * 21 : 0);
        if (to_edge[d] > 0) {
            // The quadrant sums blur the one cell a step would reach, so it is also sensed alone.
            const int next = (y + step_y[d]) * width + x + step_x[d];
            out[in_step] = input((cells[next].plants * digestion + world.fruit_at(next) * fruit_value) >> 1);
        }
    }

    int neighbors = 0;
    for (int d = 0; d < directions; ++d) {
        const int xx = x + step_x[d], yy = y + step_y[d];
        neighbors += xx >= 0 && xx < width && yy >= 0 && yy < height && Sight::test(sight.occupied, yy * width + xx);
    }
    auto& self = senses.self;
    self[self_energy] = input(me.energy * 3 / 4);
    self[self_nutrients] = input(me.nutrients_carried * 15);
    self[self_youth] = inner.youth;
    self[self_food] = input((here.plants * digestion + world.fruit_at(me.cell) * fruit_value) >> 1);
    self[self_carcass] = input(here.animal * 8);
    self[self_mismatch] = input(mismatch >> 1);
    self[self_neighbors] = input(neighbors * 31);
    self[self_damage] = inner.damage;
    self[self_memory0] = inner.memory[0];
    self[self_memory1] = inner.memory[1];
    return senses;
}

Decision think(const Senses& senses, const std::int8_t* weights, unsigned turn) {
    // The self inputs' share of each scorer unit is the same for all four
    // directions, so it is summed once.
    int shared[scorer_hidden];
    for (int h = 0; h < scorer_hidden; ++h) {
        const auto* w = weights + h * scorer_unit_bytes + direction_inputs;
        int sum = w[self_inputs] * 64;
        for (int i = 0; i < self_inputs; ++i) {
            sum += w[i] * senses.self[i];
        }
        shared[h] = sum;
    }
    const auto* score_weights = weights + scorer_hidden * scorer_unit_bytes;
    int best = 0, action = 0;
    for (int n = 0; n < directions; ++n) {
        const int d = (turn + n) & 3;
        const auto& in = senses.direction[d];
        int score = score_weights[scorer_hidden] * 64;
        for (int h = 0; h < scorer_hidden; ++h) {
            const auto* w = weights + h * scorer_unit_bytes;
            int sum = shared[h];
            for (int i = 0; i < direction_inputs; ++i) {
                sum += w[i] * in[i];
            }
            score += score_weights[h] * activation(sum);
        }
        if (!action || score > best) {
            best = score;
            action = d + 1;
        }
    }

    const auto* head = weights + scorer_bytes;
    int hidden[head_hidden];
    for (int h = 0; h < head_hidden; ++h) {
        const auto* w = head + h * head_unit_bytes;
        int sum = w[self_inputs + direction_inputs] * 64;
        for (int i = 0; i < self_inputs; ++i) {
            sum += w[i] * senses.self[i];
        }
        for (int i = 0; i < direction_inputs; ++i) {
            const int total = senses.direction[0][i] + senses.direction[1][i] + senses.direction[2][i] +
                              senses.direction[3][i];
            sum += w[self_inputs + i] * (total / directions);
        }
        hidden[h] = activation(sum);
    }
    int out[head_outputs];
    const auto* w = head + head_hidden * head_unit_bytes;
    for (int o = 0; o < head_outputs; ++o, w += head_hidden + 1) {
        int sum = w[head_hidden] * 64;
        for (int h = 0; h < head_hidden; ++h) {
            sum += w[h] * hidden[h];
        }
        out[o] = sum;
    }

    Decision decision;
    // Staying wins a tie with the best move: moving must be worth its cost.
    decision.action = static_cast<std::uint8_t>(out[out_stay] >= best ? 0 : action);
    decision.sprint = out[out_sprint] > 0;
    decision.breed = out[out_breed] > 0;
    decision.signal = out[out_signal] > 0;
    decision.memory = {static_cast<std::int8_t>(activation(out[out_memory0])),
                       static_cast<std::int8_t>(activation(out[out_memory1]))};
    return decision;
}

void combine(std::int8_t* child, const std::int8_t* first, const std::int8_t* second, std::uint32_t& state) {
    const auto pick = [&]() { return next(state) & 1u ? second : first; };
    const std::int8_t* source = first;
    // The scorer: each hidden unit's row, then its weight into the move score.
    const int score_weights = scorer_hidden * scorer_unit_bytes;
    for (int h = 0; h < scorer_hidden; ++h) {
        source = pick();
        for (int i = 0; i < scorer_unit_bytes; ++i) {
            child[h * scorer_unit_bytes + i] = source[h * scorer_unit_bytes + i];
        }
        child[score_weights + h] = source[score_weights + h];
    }
    child[score_weights + scorer_hidden] = pick()[score_weights + scorer_hidden];
    // The head: each hidden unit's row, then its weight into every output.
    const int head = scorer_bytes, outputs = head + head_hidden * head_unit_bytes;
    for (int h = 0; h < head_hidden; ++h) {
        source = pick();
        for (int i = 0; i < head_unit_bytes; ++i) {
            child[head + h * head_unit_bytes + i] = source[head + h * head_unit_bytes + i];
        }
        for (int o = 0; o < head_outputs; ++o) {
            child[outputs + o * (head_hidden + 1) + h] = source[outputs + o * (head_hidden + 1) + h];
        }
    }
    for (int o = 0; o < head_outputs; ++o) {
        const int bias = outputs + o * (head_hidden + 1) + head_hidden;
        child[bias] = pick()[bias];
    }
}

unsigned turn_for(CreatureId id, std::uint64_t tick) {
    auto mixed = id * 0x9e3779b9u + static_cast<std::uint32_t>(tick) * 0x85ebca6bu + 0x7f4a7c15u;
    mixed = (mixed ^ (mixed >> 13)) * 0xc2b2ae35u;
    return (mixed ^ (mixed >> 16)) & 3u;
}

int inherit(std::int8_t* child, const std::int8_t* parent, std::uint32_t& state) {
    int changes = 0;
    for (int i = 0; i < brain_bytes; ++i) {
        int weight = parent[i];
        const auto roll = next(state);
        if ((roll & 127u) == 0) {
            const int step = 1 + static_cast<int>((roll >> 7) & 3u);
            weight = clip(weight + ((roll >> 9) & 1u ? step : -step));
            changes += weight != parent[i];
        }
        child[i] = static_cast<std::int8_t>(weight);
    }
    return changes;
}

} // namespace petri::brain
