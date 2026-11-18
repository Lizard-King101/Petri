#include "render_state.hpp"

#include <algorithm>

namespace petri {
namespace {

DietShape diet_shape(const Genome& genes) {
    if (genes.meat_digestion() >= 10) {
        return DietShape::meat;
    }
    return genes.digestion >= 10 ? DietShape::plant : DietShape::mixed;
}

RenderActor actor_of(const Creature& creature) {
    return {creature.id, creature.cell, look_of(creature.genes)};
}

} // namespace

Look look_of(const Genome& genes) {
    const int back = 7 - (Genome::maximum - genes.thrift) / 5;
    const auto body = static_cast<unsigned>(biome_at(genes.preferred_moisture()));
    return {static_cast<std::uint32_t>(static_cast<unsigned>(diet_shape(genes)) | body << 2 |
                                       unsigned(std::clamp(back, 4, 7) - 4) << 4 | (genes.faction & 7u) << 6)};
}

RenderCell render_cell(const World& world, std::size_t index) {
    const auto& cell = world.cells()[index];
    const unsigned stock = world.fruiting_shrub(index) ? 1u + world.fruit_at(index) : 0u;
    const unsigned terrain = static_cast<unsigned>(biome_at(cell.moisture)) | (cell.moisture >> 4) << 2 |
                             (cell.animal ? 1u : 0u) << 6;
    return {static_cast<std::uint8_t>(terrain), cell.plants,
            static_cast<std::uint8_t>(std::min<unsigned>(16, cell.detritus) | stock << 5)};
}

RenderCreature render_creature(const Creature& creature) {
    if (!creature.alive()) {
        return {};
    }
    return {creature.id, creature.cell, look_of(creature.genes)};
}

void capture(const World& world, RenderState& state) {
    state.tick = world.metrics().ticks;
    state.connected_biomes = world.connected_biomes();
    for (std::size_t index = 0; index < state.cells.size(); ++index) {
        state.cells[index] = render_cell(world, index);
    }
    for (std::size_t slot = 0; slot < state.creatures.size(); ++slot) {
        state.creatures[slot] = render_creature(world.creatures()[slot]);
    }
}

void EventLog::add(const RenderEvent& event) {
    if (size_ < events_.size()) {
        events_[size_++] = event;
    } else {
        ++dropped_;
    }
}

StepObserver EventLog::observer(const World& world) {
    world_ = &world;
    StepObserver observer;
    observer.context = this;
    observer.record_hunt = [](void* context, std::uint16_t, std::uint16_t prey) {
        auto& log = *static_cast<EventLog*>(context);
        RenderEvent event;
        event.kind = RenderEvent::Kind::hunt;
        event.flag = log.world_->cells()[prey].moisture >= 64;
        event.first.cell = prey;
        log.add(event);
    };
    observer.record_meal = [](void* context, const Creature& creature, bool meat) {
        RenderEvent event;
        event.kind = RenderEvent::Kind::meal;
        event.flag = meat;
        event.first = actor_of(creature);
        static_cast<EventLog*>(context)->add(event);
    };
    observer.record_attack = [](void* context, const Creature& attacker, const Creature& target, bool landed) {
        RenderEvent event;
        event.kind = RenderEvent::Kind::attack;
        event.flag = landed;
        event.first = actor_of(attacker);
        event.second = actor_of(target);
        static_cast<EventLog*>(context)->add(event);
    };
    observer.record_death = [](void* context, const Creature& creature, DeathCause cause) {
        RenderEvent event;
        event.kind = RenderEvent::Kind::death;
        event.flag = cause == DeathCause::predation;
        event.first = actor_of(creature);
        static_cast<EventLog*>(context)->add(event);
    };
    return observer;
}

} // namespace petri
