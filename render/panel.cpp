#include "panel.hpp"
#include "terrain.hpp"

#include <algorithm>
#include <cstdlib>

namespace petri {
namespace {

constexpr std::uint32_t hunt_visible = 2350, hunt_quiet = 350;
constexpr std::uint32_t approach_time = 750, bite_time = 1150, meal_visible = 900;
constexpr std::uint32_t attack_impact = 280, attack_action = 800, attack_rejoin = 300;
constexpr std::uint32_t collapse_time = 700, death_visible = 1900;
constexpr auto followed_color = rgb565(64, 208, 240);
constexpr auto carrion_color = rgb565(160, 20, 120);
constexpr auto eye_color = rgb565(240, 244, 192);
constexpr auto fruit_color = rgb565(248, 116, 24);
constexpr std::array<std::uint16_t, faction_count + 1> coat_colors{
    0, rgb565(56, 224, 224), rgb565(200, 80, 232), rgb565(250, 232, 136), rgb565(88, 216, 128),
};

constexpr std::uint16_t dim(std::uint16_t color) {
    return (color & 0xf7de) >> 1;
}

constexpr std::uint32_t terrain_pattern(unsigned cell) {
    auto value = (cell + 0x9e3779b9u) * 0x85ebca6bu;
    value = (value ^ (value >> 13)) * 0xc2b2ae35u;
    return value ^ (value >> 16);
}

std::uint16_t body_color(bool connected_biomes, Look look) {
    constexpr std::array colors{rgb565(208, 96, 48), rgb565(208, 160, 48), rgb565(112, 192, 112)};
    return connected_biomes ? colors[static_cast<unsigned>(look.body())] : creature_tint(connected_biomes);
}

} // namespace

void Panel::set_camera(Camera camera, std::uint32_t now_ms) {
    if (camera.view == PanelView::hunt && camera_.view != PanelView::hunt) {
        hunt_.active = false;
        hunt_.quiet_since = now_ms;
    }
    center_ = camera.follow && (camera.follow != camera_.follow || camera.zoom != camera_.zoom);
    if (center_ && placed_ && camera.zoom == camera_.zoom) {
        camera.left = camera_.left;
        camera.top = camera_.top;
    }
    focus_.active = false;
    framing_ = false;
    if (camera.follow != camera_.follow) {
        subject_ = {};
        dying_ = {};
    }
    camera_ = camera;
    auto_follow_ = false;
}

void Panel::cycle_view(std::uint32_t now_ms) {
    if (camera_.view == PanelView::close && camera_.zoom == sprite_zoom) {
        auto camera = camera_;
        camera.zoom = detail_zoom;
        camera.left = camera_.left * 2 + panel_width / 2;
        camera.top = camera_.top * 2 + panel_height / 2;
        const bool automatic = auto_follow_;
        set_camera(camera, now_ms);
        auto_follow_ = automatic;
        return;
    }
    const auto next = static_cast<PanelView>((static_cast<unsigned>(camera_.view) + 1) % 4);
    const auto zoom = next == PanelView::close ? sprite_zoom : 0;
    set_camera({next, static_cast<std::uint8_t>(zoom)}, now_ms);
    auto_follow_ = next == PanelView::close;
}

void Panel::begin_update(std::uint32_t now_ms) {
    now_ms_ = now_ms;
    for (std::size_t slot = 0; slot < previous_.size(); ++slot) {
        const auto& creature = state_.creatures[slot];
        previous_[slot] = {creature.id, creature.cell};
    }
}

void Panel::apply(const RenderEvent& event) {
    switch (event.kind) {
    case RenderEvent::Kind::hunt:
        mark_hunt(event.flag);
        break;
    case RenderEvent::Kind::meal:
        mark_meal(event.first.id, event.flag);
        break;
    case RenderEvent::Kind::attack:
        mark_attack(event.first, event.second, event.flag);
        break;
    case RenderEvent::Kind::death:
        mark_death(event.first, event.flag);
        break;
    }
}

void Panel::end_update(std::uint32_t now_us, std::uint32_t interval) {
    motion_start_ = now_us;
    motion_duration_ = std::min<std::uint32_t>(550000, interval * 4 / 5);
}

void Panel::mark_hunt(bool wet) {
    if (camera_.view == PanelView::hunt && !hunt_.active && now_ms_ - hunt_.quiet_since >= hunt_quiet) {
        hunt_.wet = wet;
        hunt_.started_at = now_ms_;
        hunt_.active = true;
    }
}

void Panel::mark_meal(std::uint64_t id, bool meat) {
    if (id == camera_.follow) {
        subject_.ate_at = now_ms_;
        subject_.fed = true;
        subject_.meat = meat;
    }
}

Panel::Actor Panel::capture_actor(const RenderActor& creature) const {
    const auto previous = std::find_if(previous_.begin(), previous_.end(), [&](const Previous& actor) {
        return actor.id == creature.id;
    });
    const bool right = creature.id == camera_.follow ? subject_.face_right
        : previous == previous_.end() || creature.cell % width >= previous->cell % width;
    return {creature.id, creature.cell, creature.look, right};
}

Panel::LifeEvent* Panel::start_event(const RenderActor& target, const RenderActor* attacker) {
    const auto visible = [&](const RenderActor& creature) {
        const int tile = camera_.zoom ? 1 << (camera_.zoom - 1) : 1;
        const auto origin = cell_origin(creature.cell, tile);
        const int x = origin[0] - camera_.left;
        const int y = origin[1] - camera_.top;
        return creature.id == camera_.follow || (camera_.view == PanelView::close &&
            x > -tile && x < panel_width && y > -tile && y < panel_height);
    };
    if (!visible(target) && (!attacker || !visible(*attacker))) {
        return nullptr;
    }
    const auto priority = [&](const LifeEvent& event) {
        return !event.active || now_ms_ - event.started_at >= death_visible ? -1
            : event.target.id == camera_.follow ? 2 : event.attacker.id == camera_.follow ? 1 : 0;
    };
    auto slot = std::min_element(events_.begin(), events_.end(), [&](const LifeEvent& a, const LifeEvent& b) {
        const int left = priority(a), right = priority(b);
        return left == right ? now_ms_ - a.started_at > now_ms_ - b.started_at : left < right;
    });
    LifeEvent event{capture_actor(target), attacker ? capture_actor(*attacker) : Actor{}, now_ms_, true};
    if (priority(event) < priority(*slot)) {
        return nullptr;
    }
    *slot = event;
    return &*slot;
}

void Panel::mark_attack(const RenderActor& attacker, const RenderActor& target, bool landed) {
    auto same = std::find_if(events_.begin(), events_.end(), [&](const LifeEvent& event) {
        return event.active && !event.dead && event.attacker.id == attacker.id && event.target.id == target.id &&
            now_ms_ - event.started_at < attack_action;
    });
    auto* event = same == events_.end() ? start_event(target, &attacker) : &*same;
    if (event && landed) {
        event->dead = true;
        event->started_at = now_ms_;
    }
    if (landed && target.id == camera_.follow) {
        dying_ = {target.id, attacker.id, target.cell, now_ms_};
    }
}

void Panel::mark_death(const RenderActor& creature, bool predation) {
    if (predation) {
        return;
    }
    if (auto* event = start_event(creature, nullptr)) {
        event->dead = true;
    }
    if (creature.id == camera_.follow) {
        dying_ = {creature.id, 0, creature.cell, now_ms_};
    }
}

const Panel::LifeEvent* Panel::attack_for(std::uint64_t id) const {
    const LifeEvent* oldest = nullptr;
    for (const auto& event : events_) {
        const auto age = now_ms_ - event.started_at;
        if (event.active && event.attacker.id && age < attack_action + attack_rejoin &&
            (event.attacker.id == id || event.target.id == id) &&
            (!oldest || age > now_ms_ - oldest->started_at)) {
            oldest = &event;
        }
    }
    return oldest;
}

void Panel::pixel(int x, int y, std::uint16_t color) {
    if (0 <= x && x < panel_width && 0 <= y && y < panel_height) {
        frame_[y * panel_width + x] = color;
    }
}

void Panel::fill(int x, int y, int w, int h, std::uint16_t color) {
    const int left = std::max(0, x);
    const int right = std::min(panel_width, x + w);
    const int bottom = std::min(panel_height, y + h);
    if (left >= right) {
        return;
    }
    for (int row = std::max(0, y); row < bottom; ++row) {
        std::fill_n(frame_.begin() + row * panel_width + left, right - left, color);
    }
}

void Panel::sprite(int x, int y, int scale, DietShape diet, bool face_right,
                   bool stride, bool mouth_open, std::uint16_t body, std::uint8_t faction, int height) {
    static constexpr char shapes[][7][9] = {
        {"....#...", "....###.", "...##o##", "#######.", ".#####t.", "..#####.", "..l..l.."},
        {"........", "#...#.#.", "##..#o#.", "########", ".######.", "..####..", "..l..l.."},
        {".....#.#", ".....###", "....##o.", ".#######", "#######.", "..####..", "..l..l.."},
    };
    const auto& shape = shapes[static_cast<unsigned>(diet)];
    const auto part_at = [&](int col, int row, std::uint16_t color) {
        fill(x + (face_right ? col : 7 - col) * scale, y + (7 - height + row * height / 7) * scale,
             scale, scale, color);
    };
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 8; ++col) {
            const char part = shape[row][col];
            if (part == '.' || (mouth_open && diet == DietShape::meat && row == 3 && col >= 5)) {
                continue;
            }
            const auto color = part == 'o' ? (height == 7 ? eye_color : dim(body))
                : part == 't' ? rgb565(120, 180, 180) : body;
            const int foot = part == 'l' && stride ? (col == 2 ? -1 : 1) : 0;
            part_at(col + foot, row, color);
        }
    }
    const auto coat = coat_colors[faction];
    switch (faction) {
    case 1:
        part_at(2, 3, coat);
        part_at(2, 4, coat);
        break;
    case 2:
        part_at(3, 4, coat);
        break;
    case 3:
        part_at(2, 3, coat);
        part_at(3, 3, coat);
        break;
    case 4:
        part_at(2, 4, coat);
        part_at(4, 4, coat);
        break;
    }
}

void Panel::detail_sprite(int x, int y, Look look, bool face_right,
                          bool stride, bool eating, std::uint16_t body, int height) {
    constexpr int tile = 16;
    const auto part = [&](int col, int row, int w, int h, std::uint16_t color) {
        fill(x + (face_right ? col : tile - col - w), y + tile - height + row * height / tile,
             w, std::max(1, h * height / tile), color);
    };
    const auto shade = dim(body);
    constexpr auto nose = rgb565(32, 24, 32);
    const auto eye = height == tile ? eye_color : dim(body);
    const int back = look.back();
    const auto diet = look.shape();

    // Draw the far legs behind the body and the near legs over the belly.
    part(5 + int(stride), 10, 2, 4, shade);
    part(9 - int(stride), 10, 2, 4, shade);
    part(3, back + 1, 9, 11 - back, body);
    part(4, back, 6, 1, body);
    part(4, 11, 7, 1, shade);
    part(4, 11, 2, 3, body);
    part(9, 11, 2, 3, body);
    part(4 - int(stride), 14, 3, 1, body);
    part(9 + int(stride), 14, 3, 1, body);

    // Coat markings share the inherited faction marker used by hunting.
    const auto coat = coat_colors[look.faction()];
    switch (look.faction()) {
    case 1:
        part(5, back + 1, 1, 3, coat);
        part(7, back + 1, 1, 3, coat);
        break;
    case 2:
        part(5, back + 2, 2, 2, coat);
        part(8, back + 1, 1, 1, coat);
        break;
    case 3:
        part(5, back + 1, 4, 1, coat);
        part(6, back + 2, 2, 1, coat);
        break;
    case 4:
        part(4, back + 2, 2, 1, coat);
        part(7, back + 3, 3, 1, coat);
        break;
    }

    if (diet == DietShape::meat) {
        part(0, 7 + int(stride), 2, 1, shade);
        part(1, 8, 3, 2, body);
        part(10, 3, 1, 3, body);
        part(11, 4, 2, 2, body);
        part(10, 5, 5, 3, body);
        part(11, 7, 5, 2, body);
        part(12, 9 + int(eating), 3, 1, body);
        part(13, 6, 1, 1, eye);
        part(15, 7, 1, 1, nose);
    } else if (diet == DietShape::plant) {
        part(1, 7, 2, 2, shade);
        part(2, 8, 2, 2, body);
        part(10, 2, 2, 5, body);
        part(13, 1, 1, 5, body);
        part(13, 2, 2, 3, body);
        part(10, 5, 5, 4, body);
        part(12, 8, 4, 2, body);
        part(13, 10 + int(eating), 2, 1, shade);
        part(13, 6, 1, 1, eye);
        part(15, 8, 1, 1, nose);
    } else {
        part(0, 5 + int(stride), 2, 3, body);
        part(1, 7, 3, 3, body);
        part(9, 4, 2, 3, body);
        part(12, 4, 2, 3, body);
        part(10, 5, 5, 5, body);
        part(12, 8, 4, 2, body);
        part(12, 10 + int(eating), 3, 1, shade);
        part(13, 6, 1, 1, eye);
        part(15, 8, 1, 1, nose);
    }
}

void Panel::draw_hunt() {
    const auto age = now_ms_ - hunt_.started_at;
    if (hunt_.active && age >= hunt_visible) {
        hunt_.active = false;
        hunt_.quiet_since = now_ms_;
    }
    if (!hunt_.active) {
        render_life(state_, frame_, false);
        for (auto& color : frame_) {
            color = (color & 0xe79c) >> 2;
        }
        return;
    }

    frame_.fill(0);
    constexpr auto cyan = rgb565(0, 190, 211);
    constexpr auto gold = rgb565(220, 126, 24);
    constexpr auto pink = rgb565(242, 47, 120);
    const bool approaching = age < approach_time;
    const bool biting = !approaching && age < bite_time;
    const bool stride = approaching && (age / 120) % 2;
    const bool mouth_open = biting || (!approaching && (age / 200) % 2);
    const int hunter_x = 1 + 17 * int(std::min(age, approach_time)) / int(approach_time);
    fill(0, 27, panel_width, 1, hunt_.wet ? rgb565(0, 36, 28) : rgb565(39, 16, 0));

    if (approaching || biting) {
        sprite(40, 6 + int(biting), 3, DietShape::plant, false, biting, false, gold);
    } else {
        fill(41, 19, 12, 6, pink);
        fill(44, 16, 6, 3, pink);
    }
    sprite(hunter_x, 6, 3, DietShape::meat, true, stride, mouth_open, cyan);
    if (biting) {
        pixel(40, 14, pink);
        pixel(42, 10, pink);
        pixel(44, 12, pink);
    }
}

std::array<int, 2> Panel::position(std::size_t slot, int tile, std::uint32_t elapsed) const {
    const auto& creature = state_.creatures[slot];
    const auto& previous = previous_[slot];
    const int from = previous.id == creature.id ? previous.cell : creature.cell;
    const auto old_origin = cell_origin(from, tile);
    const auto new_origin = cell_origin(creature.cell, tile);
    std::array<int, 2> normal{
        interpolate_axis(old_origin[0], new_origin[0], elapsed, motion_duration_),
        interpolate_axis(old_origin[1], new_origin[1], elapsed, motion_duration_),
    };
    const auto* event = camera_.view == PanelView::close ? attack_for(creature.id) : nullptr;
    if (!event) {
        return normal;
    }
    const auto age = now_ms_ - event->started_at;
    const bool attacker = creature.id == event->attacker.id;
    const auto cell = attacker ? event->attacker.cell : event->target.cell;
    auto staged = cell_origin(cell, tile);
    const auto target = cell_origin(event->target.cell, tile);
    const auto source = cell_origin(event->attacker.cell, tile);
    const int dx = target[0] - source[0];
    const int dy = target[1] - source[1];
    int distance = 0;
    if (attacker) {
        distance = age < attack_impact ? interpolate_axis(0, tile / 2, age, attack_impact)
            : interpolate_axis(tile / 2, event->dead ? tile : 0,
                               age - attack_impact, attack_action - attack_impact);
    } else if (age >= attack_impact && age < attack_action) {
        const auto recoil_age = age - attack_impact;
        constexpr auto recoil_half = (attack_action - attack_impact) / 2;
        distance = recoil_age < recoil_half ? interpolate_axis(0, tile / 4, recoil_age, recoil_half)
            : interpolate_axis(tile / 4, 0, recoil_age - recoil_half, recoil_half);
    }
    staged[0] += dx * distance / tile;
    staged[1] += dy * distance / tile;
    if (age >= attack_action) {
        staged[0] = interpolate_axis(staged[0], normal[0], age - attack_action, attack_rejoin);
        staged[1] = interpolate_axis(staged[1], normal[1], age - attack_action, attack_rejoin);
    }
    return staged;
}

void Panel::update_camera(int tile, std::uint32_t elapsed, unsigned speed_index) {
    const auto& creatures = state_.creatures;
    const auto find = [&](std::uint64_t id) {
        return id ? std::find_if(creatures.begin(), creatures.end(),
            [id](const RenderCreature& creature) { return creature.id == id; }) : creatures.end();
    };
    auto subject = find(camera_.follow);
    const auto old_follow = camera_.follow;
    const bool holding = subject == creatures.end() && dying_.id == camera_.follow && dying_.id &&
        now_ms_ - dying_.started_at < death_visible;
    const int previous_cell = dying_.id ? dying_.cell : subject_.cell;
    if (subject == creatures.end() && !holding) {
        if (dying_.id == camera_.follow) {
            subject = find(dying_.attacker);
            dying_ = {};
        }
        if (subject == creatures.end() && auto_follow_) {
            int closest = width + height;
            for (auto candidate = creatures.begin(); candidate != creatures.end(); ++candidate) {
                const int distance = std::abs(int(candidate->cell % width) - previous_cell % width) +
                    std::abs(int(candidate->cell / width) - previous_cell / width);
                if (candidate->alive() && (!placed_ || distance < closest)) {
                    subject = candidate;
                    closest = distance;
                    if (!placed_) {
                        break;
                    }
                }
            }
        }
    }
    camera_.follow = holding ? old_follow : subject == creatures.end() ? 0 : subject->id;
    if (subject != creatures.end() && subject->id != old_follow) {
        subject_ = {};
        subject_.cell = subject->cell;
        subject_.moved_at = now_ms_ - 200;
        center_ = true;
    }
    const bool anchored = holding || subject != creatures.end();
    auto anchor = holding ? cell_origin(dying_.cell, tile)
        : subject == creatures.end() ? std::array<int, 2>{} : position(subject - creatures.begin(), tile, elapsed);
    const LifeEvent* scene = camera_.view == PanelView::close ? attack_for(camera_.follow) : nullptr;
    if (holding && !scene && camera_.view == PanelView::close) {
        const auto held = std::find_if(events_.begin(), events_.end(), [&](const LifeEvent& event) {
            return event.active && event.dead && event.attacker.id && event.target.id == camera_.follow &&
                now_ms_ - event.started_at < death_visible;
        });
        if (held != events_.end()) {
            scene = &*held;
        }
    }
    if (scene) {
        const auto source = cell_origin(scene->attacker.cell, tile);
        const auto target = cell_origin(scene->target.cell, tile);
        anchor = {(source[0] + target[0]) / 2, (source[1] + target[1]) / 2};
    } else if (framing_ && anchored) {
        center_ = true;
    }
    framing_ = scene != nullptr;
    if (camera_.zoom == 0) {
        camera_.left = camera_.top = 0;
        focus_.active = center_ = false;
    } else {
        const int target_left = std::clamp(anchor[0] - (panel_width - tile) / 2, 0, scene_width(tile) - panel_width);
        const int target_top = std::clamp(anchor[1] - (panel_height - tile) / 2, 0, height * tile - panel_height);
        if (center_ && anchored) {
            if (!placed_) {
                camera_.left = target_left;
                camera_.top = target_top;
            } else {
                const int distance = std::max(std::abs(target_left - camera_.left), std::abs(target_top - camera_.top));
                focus_ = {camera_.left, camera_.top, now_ms_, static_cast<std::uint32_t>(std::min(1800, 400 + distance * 2)), true};
            }
            center_ = false;
        }
        if (focus_.active && anchored) {
            const auto age = now_ms_ - focus_.started_at;
            const unsigned progress = std::min(age, focus_.duration) * 256 / focus_.duration;
            const unsigned eased = progress * progress * (768 - 2 * progress) / 65536;
            camera_.left = interpolate_axis(focus_.left, target_left, eased, 256);
            camera_.top = interpolate_axis(focus_.top, target_top, eased, 256);
            focus_.active = age < focus_.duration;
        } else {
            const auto pan = [&](int origin, int point, int screen, int limit) {
                if (anchored) {
                    const int margin = std::min(tile, (screen - tile) / 2);
                    const int relative = point - origin;
                    const int correction = scene ? point - (screen - tile) / 2 - origin
                        : relative - std::clamp(relative, margin, screen - tile - margin);
                    const int step = speed_index == 3 ? 4 : 2;
                    origin += std::clamp(correction, -step, step);
                    if (!scene) {
                        origin = std::clamp(origin, point - (screen - tile), point);
                    }
                }
                return std::clamp(origin, 0, limit - screen);
            };
            camera_.left = pan(camera_.left, anchor[0], panel_width, scene_width(tile));
            camera_.top = pan(camera_.top, anchor[1], panel_height, height * tile);
        }
    }

    if (subject != creatures.end() && subject->cell != subject_.cell) {
        const int x = subject->cell % width;
        const int previous_x = subject_.cell % width;
        if (x != previous_x && !center_) {
            subject_.face_right = x > previous_x;
        }
        subject_.cell = subject->cell;
        subject_.moved_at = now_ms_;
    }
    placed_ = true;
}

void Panel::plant_sprite(int x, int y, int tile, int index) {
    const auto& cell = state_.cells[index];
    const auto fruit = cell.fruit();
    const bool shrub = cell.shrub();
    struct Colors { std::uint16_t stem, leaf, tip; };
    constexpr std::array colors{
        Colors{rgb565(104, 72, 28), rgb565(96, 128, 48), rgb565(152, 160, 72)},
        Colors{rgb565(40, 88, 40), rgb565(64, 152, 56), rgb565(120, 192, 72)},
        Colors{rgb565(40, 88, 64), rgb565(56, 144, 104), rgb565(156, 160, 80)},
    };
    constexpr Colors shrub_colors{rgb565(96, 56, 24), rgb565(64, 144, 56), rgb565(112, 176, 72)};
    const auto biome = cell.biome();
    const auto [stem, leaf, tip] = shrub ? shrub_colors : colors[static_cast<unsigned>(biome)];
    if (tile == 4) {
        // The plant sits below the creature and to the right of any carrion.
        pixel(x + 2, y + 3, fruit ? fruit_color : dim(leaf));
        if (cell.plants >= 2) {
            pixel(x + 3, y + 2, dim(tip));
        }
        if (cell.plants >= 7) {
            pixel(x + 3, y + 3, dim(leaf));
        }
        return;
    }
    if (tile == 8) {
        if (shrub) {
            fill(x + 2, y + 4, 1, 3, stem);
            fill(x + 1, y + 2, 4, 3, leaf);
            fill(x + 2, y + 1, 2, 1, tip);
            if (fruit) {
                pixel(x + 1, y + 2, fruit_color);
            }
            if (fruit >= 3) {
                pixel(x + 3, y + 1, fruit_color);
            }
            return;
        }
        const int top = biome == Biome::wetland ? 3 : 4;
        fill(x + 2, y + top, 1, 7 - top, leaf);
        pixel(x + 2, y + top, tip);
        pixel(x + 1, y + 5, leaf);
        if (cell.plants >= 5) {
            pixel(x + 3, y + 4, leaf);
        }
        return;
    }

    const int growth = std::min(4, (cell.plants - 1) / 2);
    if (shrub) {
        const int top = y + 9 - growth;
        fill(x + 4, top + 2, 2, y + 13 - top, stem);
        fill(x + 2, top + 1, 8, 3, leaf);
        fill(x + 3, top, 6, 2, tip);
        fill(x + 1, top + 3, 7, 2, leaf);
        constexpr std::array<std::array<int, 2>, max_fruit> berries{{{2, 2}, {5, 0}, {8, 2}, {4, 3}}};
        for (int i = 0; i < fruit; ++i) {
            pixel(x + berries[i][0], top + berries[i][1], fruit_color);
        }
        return;
    }
    switch (biome) {
    case Biome::scrub: {
        const int top = y + 11 - growth;
        fill(x + 5, top + 1, 2, y + 14 - top, stem);
        fill(x + 3, top + 1, 6, 2, leaf);
        fill(x + 4, top, 4, 1, tip);
        if (growth >= 2) {
            fill(x + 1, top + 3, 6, 2, leaf);
            fill(x + 2, top + 2, 3, 1, tip);
        }
        break;
    }
    case Biome::grassland: {
        const int length = 4 + growth;
        fill(x + 4, y + 14, 6, 1, stem);
        for (int i = 0; i < length; ++i) {
            const auto blade = i == length - 1 ? tip : leaf;
            pixel(x + 6 - i / 2, y + 14 - i, blade);
            pixel(x + 7 + i / 3, y + 14 - i, blade);
            if (i < length - 2) {
                pixel(x + 4 - i / 3, y + 14 - i, leaf);
            }
        }
        break;
    }
    case Biome::wetland: {
        const int top = 9 - growth;
        fill(x + 4, y + top + 2, 1, 13 - top, stem);
        fill(x + 7, y + top, 1, 15 - top, leaf);
        fill(x + 7, y + top - 1, 1, 2, tip);
        pixel(x + 4, y + top + 1, tip);
        if (growth >= 2) {
            fill(x + 9, y + top + 3, 1, 12 - top, stem);
            pixel(x + 9, y + top + 2, tip);
        }
        fill(x + 3, y + 13, 5, 1, leaf);
        pixel(x + 6, y + 12, leaf);
        break;
    }
    }
}

std::uint16_t Panel::ground_pixel(int x, int y, int tile) const {
    int owner = y / tile * width + x / tile;
    if (state_.connected_biomes) {
        const int col = (x + tile / 2) / tile - 1;
        const int row = (y + tile / 2) / tile - 1;
        const int u = (x + tile / 2) % tile, v = (y + tile / 2) % tile;
        const auto index = [&](int dx, int dy) {
            return std::clamp(row + dy, 0, height - 1) * width + std::clamp(col + dx, 0, width - 1);
        };
        const std::array<int, 4> cells{index(0, 0), index(1, 0), index(0, 1), index(1, 1)};
        const std::array<Biome, 4> corners{
            state_.cells[cells[0]].biome(), state_.cells[cells[1]].biome(),
            state_.cells[cells[2]].biome(), state_.cells[cells[3]].biome(),
        };
        const auto biome = dual_terrain(corners, u, v, tile, col, row);
        int best = 8 * tile * tile;
        for (unsigned i = 0; i < corners.size(); ++i) {
            const int dx = 2 * u + 1 - int(i % 2) * 2 * tile;
            const int dy = 2 * v + 1 - int(i / 2) * 2 * tile;
            const int distance = dx * dx + dy * dy;
            if (corners[i] == biome && distance < best) {
                best = distance;
                owner = cells[i];
            }
        }
    }
    const auto& cell = state_.cells[owner];
    const auto origin = cell_origin(owner, tile);
    const auto pattern = terrain_pattern(owner);
    const int u = x - origin[0], v = y - origin[1];
    const int feature_x = (pattern >> 8) % tile;
    const int feature_y = (pattern >> 16) % tile;
    const int cover = cell.plants / 3;
    auto color = state_.connected_biomes ? biome_tint(cell.biome(), cover)
        : rgb565(8 + cell.detritus() * 3, 8 + std::min(184, cover * 8), 8 + cell.moisture() / 8);
    int shade = 0;
    if (tile >= 8 && (pattern & 1) && std::abs(u - feature_x) + 2 * std::abs(v - feature_y) <= tile / 4) {
        shade = -1;
    }
    if ((pattern & 3) == 0 && v == feature_y && u >= feature_x && u <= feature_x + tile / 8) {
        shade = 1;
    }
    if (shade) {
        const auto biome = cell.biome();
        const int red = std::clamp(int(color >> 11) + 2 * shade, biome == Biome::scrub ? 4 : 0, 31);
        const int green = std::clamp(int((color >> 5) & 63) + (biome == Biome::scrub && shade < 0 ? 0 : 4 * shade),
                                   biome == Biome::scrub ? 4 : 0, 63);
        const int blue = std::clamp(int(color & 31) + (biome == Biome::wetland ? 2 * shade : 0), 0, 31);
        color = (red << 11) | (green << 5) | blue;
    }
    return color;
}

void Panel::draw_environment(int tile) {
    if (tile >= 4) {
        for (int y = 0; y < panel_height; ++y) {
            for (int x = 0; x < panel_width; ++x) {
                frame_[y * panel_width + x] = ground_pixel(x + camera_.left, y + camera_.top, tile);
            }
        }
    }
    const int last_y = (camera_.top + panel_height - 1) / tile;
    const bool connected = state_.connected_biomes;
    for (int y = camera_.top / tile; y <= last_y; ++y) {
        const int first_x = camera_.left / tile;
        const int last_x = (camera_.left + panel_width - 1) / tile;
        for (int x = first_x; x <= last_x; ++x) {
            const int index = y * width + x;
            const auto& cell = state_.cells[index];
            const int px = x * tile - camera_.left;
            const int py = y * tile - camera_.top;
            if (tile < 4) {
                const auto terrain = connected ? biome_tint(cell.biome(), cell.plants)
                    : rgb565(8 + cell.detritus() * 3, 8 + std::min(184, cell.plants * 8), 8 + cell.moisture() / 8);
                fill(px, py, tile, tile, terrain);
            }
            if (tile >= 4 && (cell.plants || cell.fruit())) {
                plant_sprite(px, py, tile, index);
            }
            const bool collapsing = cell.carcass() && std::any_of(events_.begin(), events_.end(), [&](const LifeEvent& event) {
                return event.active && event.dead && event.target.cell == index &&
                    now_ms_ - event.started_at < collapse_time + (event.attacker.id ? attack_impact : 0);
            });
            if (cell.carcass() && !collapsing) {
                if (tile >= 8) {
                    const int scale = tile / 8;
                    fill(px + 2 * scale, py + 5 * scale, 4 * scale, 2 * scale, carrion_color);
                } else if (tile == 4) {
                    fill(px, py + 3, 2, 1, carrion_color);
                } else {
                    pixel(px, py + tile - 1, carrion_color);
                }
            }
        }
    }
}

void Panel::draw_deaths(int tile) {
    for (const auto& event : events_) {
        const auto age = now_ms_ - event.started_at;
        if (!event.active || !event.dead || age >= death_visible) {
            continue;
        }
        const auto& actor = event.target;
        const auto origin = cell_origin(actor.cell, tile);
        const int px = origin[0] - camera_.left;
        const int py = origin[1] - camera_.top;
        const auto delay = event.attacker.id ? attack_impact : 0;
        const auto fallen_for = age > delay ? age - delay : 0;
        if (fallen_for < collapse_time) {
            const int full_height = tile == 8 ? 7 : 16;
            const int height = interpolate_axis(full_height, tile / 3, fallen_for, collapse_time);
            const auto color = body_color(state_.connected_biomes, actor.look);
            const auto fading = fallen_for < collapse_time / 2 ? color : dim(color);
            if (tile == 16) {
                detail_sprite(px, py, actor.look, actor.face_right, false, false, fading, height);
            } else {
                sprite(px, py, 1, actor.look.shape(), actor.face_right, false, false,
                       fading, actor.look.faction(), height);
            }
        } else {
            const bool remains = state_.cells[actor.cell].carcass();
            const int scale = tile / 8;
            const auto left = death_visible - age;
            const int w = remains ? 4 * scale : std::max(1, int(left * 4 * scale / (death_visible - collapse_time - delay)));
            fill(px + 2 * scale, py + 5 * scale, w, remains ? 2 * scale : scale,
                 remains ? carrion_color : dim(carrion_color));
        }
    }
}

void Panel::draw_contact(int tile) {
    for (const auto& event : events_) {
        const auto age = now_ms_ - event.started_at;
        if (!event.active || !event.attacker.id || age < attack_impact || age >= attack_impact + 240) {
            continue;
        }
        const auto target = cell_origin(event.target.cell, tile);
        const auto source = cell_origin(event.attacker.cell, tile);
        const int dx = target[0] - source[0], dy = target[1] - source[1];
        const int px = target[0] - camera_.left + (dx > 0 ? 0 : dx < 0 ? tile - 1 : tile / 2);
        const int py = target[1] - camera_.top + (dy > 0 ? 0 : dy < 0 ? tile - 1 : tile / 2);
        const int scale = tile / 8;
        const auto color = age < attack_impact + 80 ? eye_color : event.dead ? carrion_color : rgb565(240, 184, 80);
        const int spread = 1 + int((age - attack_impact) / 90);
        fill(px, py, scale, scale, color);
        pixel(px - spread, py - spread, color);
        pixel(px + spread, py + spread, color);
    }
}

void Panel::draw_grid(bool show_creatures, int tile, std::uint32_t elapsed) {
    if (camera_.zoom == 0) {
        render_life(state_, frame_, show_creatures);
    } else {
        draw_environment(tile);
    }
    if (!show_creatures) {
        return;
    }
    const auto& creatures = state_.creatures;
    const int offset = tile == 4 ? 1 : 0;
    PanelPoint followed;
    for (std::size_t slot = 0; slot < creatures.size(); ++slot) {
        const auto& creature = creatures[slot];
        if (!creature.alive()) {
            continue;
        }
        const auto pos = position(slot, tile, elapsed);
        const int x = camera_.zoom ? pos[0] - camera_.left + tile / 2
                                   : (creature.cell % width) / (width / panel_width);
        const int y = camera_.zoom ? pos[1] - camera_.top + tile / 2
                                   : (creature.cell / width) / (height / panel_height);
        if (x < 0 || y < 0 || x - offset >= panel_width || y - offset >= panel_height) {
            continue;
        }
        locations_[slot] = {static_cast<std::int8_t>(std::min(x, panel_width - 1)),
                            static_cast<std::int8_t>(std::min(y, panel_height - 1))};
        if (camera_.zoom) {
            const auto color = tile == 1 ? creature_tint(state_.connected_biomes)
                                         : body_color(state_.connected_biomes, creature.look);
            fill(x - offset, y - offset, offset + 1, offset + 1, color);
            if (tile == 4 && creature.look.faction()) {
                pixel(x - offset, y - offset, coat_colors[creature.look.faction()]);
            }
        }
        if (tile == 1 && creature.id == camera_.follow) {
            followed = locations_[slot];
        }
    }
    if (followed.x != PanelPoint::hidden) {
        pixel(followed.x, followed.y, followed_color);
    }
}

void Panel::draw_close(int tile, std::uint32_t elapsed) {
    const bool detail = camera_.zoom == detail_zoom;
    draw_environment(tile);
    draw_deaths(tile);

    const bool eating = subject_.fed && now_ms_ - subject_.ate_at < meal_visible;
    const bool stride = now_ms_ - subject_.moved_at < 200 && (now_ms_ / 85) % 2;
    const auto& creatures = state_.creatures;
    for (std::size_t slot = 0; slot < creatures.size(); ++slot) {
        const auto& creature = creatures[slot];
        if (!creature.alive()) {
            continue;
        }
        const auto pos = position(slot, tile, elapsed);
        const int px = pos[0] - camera_.left;
        const int py = pos[1] - camera_.top;
        if (px <= -tile || px >= panel_width || py <= -tile || py >= panel_height) {
            continue;
        }
        locations_[slot] = {static_cast<std::int8_t>(std::clamp(px + tile / 2, 0, panel_width - 1)),
                            static_cast<std::int8_t>(std::clamp(py + tile / 2, 0, panel_height - 1))};
        const bool tracked = creature.id == camera_.follow;
        const auto color = body_color(state_.connected_biomes, creature.look);
        const auto& previous = previous_[slot];
        const bool moved = previous.id == creature.id && previous.cell != creature.cell;
        bool face_right = tracked ? subject_.face_right
            : !moved || previous.cell % width <= creature.cell % width;
        const auto* action = attack_for(creature.id);
        const bool attacking = action && action->attacker.id == creature.id && now_ms_ - action->started_at < attack_action;
        if (action) {
            const auto source = cell_origin(action->attacker.cell, tile);
            const auto target = cell_origin(action->target.cell, tile);
            const bool forward = source[0] < target[0];
            face_right = action->attacker.id == creature.id ? forward : !forward;
        }
        const bool bite = (tracked && eating) || attacking;
        const bool walking = attacking || (tracked ? stride : moved && motion_duration_ && (now_ms_ / 85) % 2);
        if (detail) {
            detail_sprite(px, py, creature.look, face_right, walking, bite, color);
        } else {
            sprite(px, py, 1, creature.look.shape(), face_right, walking, bite, color, creature.look.faction());
        }
        if (bite) {
            pixel(px + (face_right ? tile - 1 : 0), py + (detail ? 11 : 4),
                  attacking || subject_.meat ? rgb565(230, 42, 110) : rgb565(69, 180, 62));
        }
    }
    draw_contact(tile);
}

void Panel::draw(std::uint32_t now_ms, std::uint32_t now_us, unsigned speed_index) {
    now_ms_ = now_ms;
    const auto elapsed = now_us - motion_start_;
    if (elapsed >= motion_duration_) {
        motion_duration_ = 0;
    }
    locations_.fill({});
    if (camera_.view == PanelView::hunt) {
        draw_hunt();
        return;
    }
    const int tile = camera_.zoom ? 1 << (camera_.zoom - 1) : 1;
    update_camera(tile, elapsed, speed_index);
    if (camera_.view == PanelView::close) {
        draw_close(tile, elapsed);
    } else {
        draw_grid(camera_.view == PanelView::life, tile, elapsed);
    }
}

} // namespace petri
