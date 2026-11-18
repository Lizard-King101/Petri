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
constexpr std::uint32_t grazing_visible = 450;
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

constexpr std::uint32_t mix(std::uint32_t a, std::uint32_t b) {
    std::uint32_t value = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
    value ^= value >> 15;
    value *= 0x2c1b3c6du;
    return value ^ (value >> 12);
}

// Blends `color` toward `toward` by alpha / 256, per RGB565 channel.
constexpr std::uint16_t blend(std::uint16_t color, std::uint16_t toward, unsigned alpha) {
    const unsigned r = color >> 11, g = (color >> 5) & 63, b = color & 31;
    const unsigned tr = toward >> 11, tg = (toward >> 5) & 63, tb = toward & 31;
    const auto lerp = [alpha](unsigned from, unsigned to) { return (from * (256 - alpha) + to * alpha) >> 8; };
    return static_cast<std::uint16_t>(lerp(r, tr) << 11 | lerp(g, tg) << 5 | lerp(b, tb));
}

constexpr auto rain_color = rgb565(220, 235, 255);
// Rain becomes visible above this bias. The world's rainfall nodes span +-80 and the season +-16.
constexpr int rain_threshold = 8;

constexpr std::uint32_t terrain_pattern(unsigned cell) {
    auto value = (cell + 0x9e3779b9u) * 0x85ebca6bu;
    value = (value ^ (value >> 13)) * 0xc2b2ae35u;
    return value ^ (value >> 16);
}

// Pigment on the ground-tone scale: 1 matches the driest scrub, 15 the deepest
// water. The colors follow the ground's hues but stay brighter, so a matched
// creature blends in without vanishing.
std::uint16_t pigment_color(int tone) {
    struct Key {
        int tone;
        unsigned r, g, b;
    };
    static constexpr std::array<Key, 7> keys{{{1, 214, 112, 58}, {3, 204, 152, 62}, {5, 156, 182, 66}, {7, 98, 186, 94},
                                              {10, 66, 174, 140}, {13, 60, 150, 190}, {15, 70, 128, 214}}};
    tone = std::clamp(tone, 1, 15);
    std::size_t i = 0;
    while (i + 2 < keys.size() && tone > keys[i + 1].tone) {
        ++i;
    }
    const auto& a = keys[i];
    const auto& b = keys[i + 1];
    const int span = b.tone - a.tone, at = std::clamp(tone - a.tone, 0, span);
    const auto lerp = [&](unsigned from, unsigned to) { return (from * unsigned(span - at) + to * unsigned(at)) / unsigned(span); };
    return rgb565(lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b));
}

std::uint16_t body_color(bool connected_biomes, Look look) {
    if (look.morphology()) {
        return pigment_color(look.pigment());
    }
    constexpr std::array colors{rgb565(208, 96, 48), rgb565(208, 160, 48), rgb565(112, 192, 112)};
    return connected_biomes ? colors[static_cast<unsigned>(look.body())] : creature_tint(connected_biomes);
}

// Darker and lighter coat shades for patterns.
std::uint16_t pattern_shade(std::uint16_t body) {
    return blend(body, 0, 110);
}

std::uint16_t pattern_light(std::uint16_t body) {
    return blend(body, rgb565(255, 250, 230), 90);
}

constexpr auto meat_color = rgb565(150, 34, 58), meat_dark = rgb565(96, 18, 40);
constexpr auto bone_color = rgb565(226, 214, 190), bone_dim = rgb565(150, 140, 124);

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
        if (!event.flag) {
            grazing_[grazing_next_] = {event.first.cell, now_ms_, true};
            grazing_next_ = static_cast<std::uint8_t>((grazing_next_ + 1) % grazing_.size());
        }
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

Build build_of(Look look) {
    if (!look.morphology()) {
        return {};
    }
    return {look.size_class(), look.pattern(), look.limbs()};
}

void Panel::sprite(int x, int y, int scale, DietShape diet, bool face_right,
                   bool stride, bool mouth_open, std::uint16_t body, std::uint8_t faction, int height, Build build) {
    // Rows 0-5 are the body and head, row 6 the legs.
    static constexpr char shapes[][7][9] = {
        {"....#...", "....###.", "...##o##", "#######.", ".#####t.", "..#####.", "..l..l.."},
        {"........", "#...#.#.", "##..#o#.", "########", ".######.", "..####..", "..l..l.."},
        {".....#.#", ".....###", "....##o.", ".#######", "#######.", "..####..", "..l..l.."},
    };
    // Body size sets the drawn width and body height; limbs set the leg length.
    static constexpr std::array<int, 8> widths{5, 6, 7, 8, 8, 9, 9, 10}, bodies{3, 4, 5, 6, 6, 7, 7, 8};
    static constexpr std::array<int, 4> legs{1, 1, 2, 3};
    const int size = std::clamp(build.size, 0, 7);
    const int w = widths[size], body_rows = bodies[size], leg_rows = legs[std::clamp(build.limbs, 0, 3)];
    const int total = body_rows + leg_rows;
    const int left = (8 - w) / 2 - (w > 8 ? 1 : 0);
    const auto& shape = shapes[static_cast<unsigned>(diet)];
    // Row r of `total` sits so the feet stay on row 6 of the cell; `height` squashes
    // the sprite toward the ground as it collapses.
    const auto put = [&](int col, int row, std::uint16_t color) {
        const int dx = left + col;
        const int dy = 6 - (total - 1 - row) * height / 7;
        fill(x + (face_right ? dx : 7 - dx) * scale, y + dy * scale, scale, scale, color);
    };
    const auto shade = pattern_shade(body);
    for (int row = 0; row < body_rows; ++row) {
        const int src_row = row * 6 / body_rows;
        for (int col = 0; col < w; ++col) {
            const int src_col = col * 8 / w;
            const char part = shape[src_row][src_col];
            if (part == '.' || (mouth_open && diet == DietShape::meat && src_row == 3 && src_col >= 5)) {
                continue;
            }
            auto color = part == 'o' ? (height == 7 ? eye_color : dim(body))
                : part == 't' ? rgb565(120, 180, 180) : body;
            // Patterns mark the torso, behind the head.
            if (part == '#' && src_col < 5 && row >= 1) {
                const bool marked = build.pattern == 1 ? (col + 2 * row) % 5 == 0
                    : build.pattern == 2 ? col % 2 == 1 && row * 3 >= body_rows
                    : build.pattern == 3 ? (col * 2 < w && row * 2 >= body_rows) || (col * 2 >= w && row == 1)
                    : false;
                if (marked) {
                    color = build.pattern == 3 && row == 1 ? pattern_light(body) : shade;
                }
            }
            put(col, row, color);
        }
    }
    for (const int src_col : {2, 5}) {
        const int col = src_col * w / 8 + (w > 8 && src_col == 5 ? 1 : 0);
        const int foot = stride ? (src_col == 2 ? -1 : 1) : 0;
        for (int k = 0; k < leg_rows; ++k) {
            put(col + (k == leg_rows - 1 ? foot : 0), body_rows + k, body);
        }
    }
    // Faction markers keep their places on the torso.
    const auto coat = coat_colors[faction];
    const auto mark = [&](int src_col, int src_row) { put(src_col * w / 8, src_row * body_rows / 6, coat); };
    switch (faction) {
    case 1:
        mark(2, 3);
        mark(2, 4);
        break;
    case 2:
        mark(3, 4);
        break;
    case 3:
        mark(2, 3);
        mark(3, 3);
        break;
    case 4:
        mark(2, 4);
        mark(4, 4);
        break;
    }
}

void Panel::detail_sprite(int x, int y, Look look, bool face_right,
                          bool stride, bool eating, std::uint16_t body, int height) {
    constexpr int tile = 16;
    const auto build = build_of(look);
    // Body size scales the sprite about its feet; limbs stretch or shorten the
    // legs (rows 10-15) and lift or lower everything above them.
    static constexpr std::array<int, 8> scales{11, 12, 14, 16, 17, 18, 19, 20};
    static constexpr std::array<int, 4> stretch{-2, 0, 2, 3};
    const int scale = scales[std::clamp(build.size, 0, 7)];
    const int lift = stretch[std::clamp(build.limbs, 0, 3)];
    const auto row_at = [&](int row) {
        const int raised = row <= 10 ? (row - lift) * 5 : (10 - lift) * 5 + (row - 10) * (5 + lift);
        // In fifths of a pixel, then scaled about the ground line and squashed by `height`.
        const int from_ground = tile * 5 - raised;
        return y + tile - (from_ground * scale * height + 5 * 16 * 16 / 2) / (5 * 16 * 16);
    };
    const auto col_at = [&](int col) { return x + 8 + ((col - 8) * scale + (col >= 8 ? 8 : -8)) / 16; };
    const auto part = [&](int col, int row, int w, int h, std::uint16_t color) {
        const int c0 = face_right ? col : tile - col - w;
        const int x0 = col_at(c0), x1 = std::max(x0 + 1, col_at(c0 + w));
        const int y0 = row_at(row), y1 = std::max(y0 + 1, row_at(row + h));
        fill(x0, y0, x1 - x0, y1 - y0, color);
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

    // Inherited coat patterns, from a few spots to bold disruptive blotches.
    const auto dark = pattern_shade(body);
    switch (build.pattern) {
    case 1:
        part(5, back + 2, 1, 1, dark);
        part(8, back + 1, 1, 1, dark);
        part(7, back + 4, 1, 1, dark);
        part(4, back + 5, 1, 1, dark);
        break;
    case 2:
        for (int col = 4; col <= 10; col += 2) {
            part(col, back + 1, 1, std::min(4, 10 - back), dark);
        }
        break;
    case 3:
        part(3, back + 1, 4, 3, dark);
        part(8, back + 3, 3, 3, dark);
        part(5, 10, 4, 1, pattern_light(body));
        break;
    default:
        break;
    }

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

namespace {

// Plants read brighter than the ground beneath them so they hold up on the LEDs.
struct Greens {
    std::uint16_t base, mid, tip;
};
constexpr Greens grass_greens{rgb565(48, 104, 36), rgb565(76, 148, 50), rgb565(132, 192, 72)};
constexpr Greens reed_greens{rgb565(34, 92, 58), rgb565(54, 136, 82), rgb565(124, 176, 92)};
constexpr Greens scrub_greens{rgb565(92, 104, 46), rgb565(120, 136, 60), rgb565(176, 168, 92)};
constexpr Greens shrub_greens{rgb565(96, 56, 24), rgb565(64, 144, 56), rgb565(112, 176, 72)};
constexpr auto straw_color = rgb565(180, 150, 70);
constexpr auto seed_color = rgb565(220, 200, 128);
constexpr auto cattail_color = rgb565(128, 74, 36);
constexpr auto pad_color = rgb565(44, 132, 68), pad_light = rgb565(100, 184, 100);
constexpr auto bloom_color = rgb565(244, 200, 224);
constexpr auto twig_color = rgb565(104, 70, 38);
constexpr auto pebble_color = rgb565(88, 82, 76), pebble_light = rgb565(140, 132, 120);

Greens dried(Greens greens, unsigned alpha) {
    if (!alpha) {
        return greens;
    }
    return {blend(greens.base, straw_color, alpha * 3 / 4), blend(greens.mid, straw_color, alpha),
            blend(greens.tip, straw_color, alpha)};
}

// Horizontal offset at `k` pixels above the base of a stalk `height` tall whose
// tip leans `lean` pixels. The bend grows with the square of the height, so the
// base holds still and the tip moves most.
int bend(int lean, int k, int height) {
    if (!lean || height <= 1) {
        return 0;
    }
    const int span = (height - 1) * (height - 1);
    const int amount = lean * k * k;
    return (amount + (amount > 0 ? span / 2 : -span / 2)) / span;
}

} // namespace

int Panel::plant_lean(int index, std::uint32_t salt) const {
    // Wind blows the way the storms travel, since every storm rides the epoch's
    // prevailing wind. Under a storm the plants lean hard and shake; elsewhere a
    // light breeze sends a gust across the field every few seconds.
    const int cx = index % width * 16 + 8, cy = index / width * 16 + 8;
    int dir = 1, cover = 0, strongest = 0;
    bool any = false;
    for (const auto& storm : state_.climate.storms) {
        if (!storm.active()) {
            continue;
        }
        if (!any) {
            dir = storm.vx < 0 ? -1 : 1;
            any = true;
        }
        const std::int32_t dx = cx - storm.x / 16, dy = cy - storm.y / 16;
        const std::int32_t reach = storm.radius * 16;
        const std::int32_t distance = dx * dx + dy * dy, limit = reach * reach;
        if (distance < limit) {
            const int here = static_cast<int>((limit - distance) * 256 / limit) * storm.strength;
            cover += here;
            if (here > strongest) {
                strongest = here;
                dir = storm.vx < 0 ? -1 : 1;
            }
        }
    }
    // Cells further downwind meet each gust later, so it rolls across the field.
    const int column = index % width;
    const std::uint32_t downwind = static_cast<std::uint32_t>(dir > 0 ? width - column : column);
    const std::uint32_t wave = (now_ms_ + (salt & 127) + downwind * 120) % 3400;
    if (!cover) {
        return wave < 700 ? dir : 0;
    }
    int lean = cover >= 1536 ? 3 : cover >= 640 ? 2 : 1;
    if ((now_ms_ + salt % 97 * 7) / 110 % 3 == 0) {
        lean -= 1;
    }
    if (wave < 1100) {
        lean += 1;
    }
    return dir * std::min(3, lean);
}

void Panel::plant_blade(int x, int y, int height, int slant, int lean, std::uint16_t base, std::uint16_t mid,
                        std::uint16_t tip) {
    for (int k = 0; k < height; ++k) {
        // Blades rise from a narrow base and arch outward toward the tip.
        const auto color = k == height - 1 ? tip : k * 2 >= height ? mid : base;
        pixel(x + bend(slant, k, height) + bend(lean, k, height), y - k, color);
    }
}

int Panel::shown_plants(int index) {
    // Plants step toward their real amount one unit every 70 ms, so a grazed
    // clump shrinks over a few frames and growth fills in rather than popping.
    auto& entry = plants_shown_[(index / width % plant_rows) * plant_columns + index % width % plant_columns];
    const int actual = state_.cells[index].plants;
    if (entry.index != index) {
        entry = {static_cast<std::uint16_t>(index), static_cast<std::uint8_t>(actual), now_ms_};
    } else if (entry.plants == actual) {
        entry.stepped_at = now_ms_;
    } else if (now_ms_ - entry.stepped_at >= 70) {
        entry.plants = static_cast<std::uint8_t>(entry.plants + (entry.plants < actual ? 1 : -1));
        entry.stepped_at = now_ms_;
    }
    return entry.plants;
}

int Panel::grazing_age(int index) const {
    int age = -1;
    for (const auto& graze : grazing_) {
        if (graze.active && graze.cell == index && now_ms_ - graze.at < grazing_visible) {
            const int since = static_cast<int>(now_ms_ - graze.at);
            age = age < 0 ? since : std::min(age, since);
        }
    }
    return age;
}

void Panel::draw_grazing(int tile) {
    // Bits of leaf fly from a creature's mouth as it grazes.
    for (const auto& graze : grazing_) {
        const auto age = now_ms_ - graze.at;
        if (!graze.active || age >= 420) {
            continue;
        }
        const auto origin = cell_origin(graze.cell, tile);
        const int px = origin[0] - camera_.left + tile / 2, py = origin[1] - camera_.top + tile * 5 / 8;
        const int spread = 1 + static_cast<int>(age) / (tile == 16 ? 70 : 110);
        const int rise = static_cast<int>(age) / (tile == 16 ? 60 : 100);
        const auto color = age < 210 ? grass_greens.tip : dim(grass_greens.mid);
        const auto seed = mix(graze.cell, graze.at);
        const int bits = tile == 16 ? 3 : 2;
        for (int i = 0; i < bits; ++i) {
            const int side = i % 2 ? 1 : -1;
            const int wobble = static_cast<int>((seed >> (4 * i)) % 3) - 1;
            pixel(px + side * (spread + i / 2) + wobble, py - rise - i / 2 + static_cast<int>(age * age / 60000), color);
        }
    }
}

void Panel::plant_sprite(int x, int y, int tile, int index) {
    const auto& cell = state_.cells[index];
    const auto hash = mix(static_cast<std::uint32_t>(index), 0x2545f491u);
    const auto biome = cell.biome();
    const int plants = shown_plants(index);
    // A grazed plant jerks back and forth as it is bitten.
    const int grazed = grazing_age(index);
    const int flick = grazed < 0 ? 0 : ((grazed / 80) % 2 ? 2 : -2) * (int(grazing_visible) - grazed) / int(grazing_visible);
    const int detritus = static_cast<int>(cell.detritus());
    const bool detail = tile == 16;
    // Grass dries to straw toward the scrub edge at moisture 56; above 96 it stays green.
    const unsigned dryness = biome == Biome::grassland
        ? static_cast<unsigned>(std::clamp(96 - int(cell.moisture()), 0, 40)) * 4 : 0;
    const auto greens = dried(biome == Biome::wetland ? reed_greens : biome == Biome::scrub ? scrub_greens : grass_greens,
                              dryness);

    if (cell.shrub()) {
        const auto fruit = cell.fruit();
        const auto [stem, leaf, tip] = shrub_greens;
        const int shift = static_cast<int>(hash % 3) - 1;
        if (tile == 4) {
            pixel(x + 2, y + 3, fruit ? fruit_color : dim(leaf));
            pixel(x + 3, y + 2, dim(tip));
            return;
        }
        const int sway = std::clamp((plant_lean(index, hash) + flick) / 2, -1, 1);
        if (tile == 8) {
            const int sx = x + std::clamp(shift, 0, 1);
            fill(sx + 2, y + 4, 1, 3, stem);
            fill(sx + 1, y + 2, 4, 3, leaf);
            fill(sx + 2 + sway, y + 1, 2, 1, tip);
            if (fruit) {
                pixel(sx + 1, y + 2, fruit_color);
            }
            if (fruit >= 3) {
                pixel(sx + 3 + sway, y + 1, fruit_color);
            }
            return;
        }
        const int growth = std::min(4, std::max(0, (plants - 1) / 2));
        const int sx = x + 2 * shift;
        const int top = y + 9 - growth;
        fill(sx + 4, top + 2, 2, y + 13 - top, stem);
        fill(sx + 2, top + 1, 8, 3, leaf);
        fill(sx + 3 + sway, top, 6, 2, tip);
        fill(sx + 1, top + 3, 7, 2, leaf);
        constexpr std::array<std::array<int, 2>, max_fruit> berries{{{2, 2}, {5, 0}, {8, 2}, {4, 3}}};
        for (int i = 0; i < fruit; ++i) {
            pixel(sx + berries[i][0] + (berries[i][1] < 2 ? sway : 0), top + berries[i][1], fruit_color);
        }
        return;
    }

    if (tile == 4) {
        // Plants keep to the corner the creature and carrion leave free: the bottom
        // row right of the carrion and the right-hand column.
        if (biome == Biome::scrub && detritus >= 6 && (hash & 3) == 0) {
            pixel(x + 3, y + 1 + static_cast<int>((hash >> 2) % 3), dim(twig_color));
        }
        if (!plants) {
            return;
        }
        // The free ring, clockwise from below the creature: (2,3) (3,3) (3,2) (3,1) (3,0) (2,0) (1,0) (0,0).
        static constexpr std::array<std::array<int, 2>, 8> ring{{{2, 3}, {3, 3}, {3, 2}, {3, 1}, {3, 0}, {2, 0}, {1, 0}, {0, 0}}};
        if (biome == Biome::wetland) {
            // A reed stands in the right-hand column, or the left one above the carrion.
            const int column = (hash >> 1) % 3 ? 3 : 0;
            const int stalk = std::min(3, 1 + plants / 3);
            const int foot = column == 3 ? 3 : 2;
            for (int k = 0; k < stalk; ++k) {
                pixel(x + column, y + foot - k, k == stalk - 1 ? greens.tip : greens.mid);
            }
            return;
        }
        const int start = static_cast<int>((hash >> 1) % 4);
        pixel(x + ring[start][0], y + ring[start][1], plants >= 4 ? greens.mid : dim(greens.mid));
        if (plants >= 3) {
            const auto& next = ring[start + 1];
            pixel(x + next[0], y + next[1], biome == Biome::grassland && plants >= 7 ? seed_color : greens.tip);
        }
        if (plants >= 7) {
            const auto& far = ring[start + 3];
            pixel(x + far[0], y + far[1], greens.mid);
        }
        return;
    }

    // Scrub keeps the litter of what grew and died there: twigs and pebbles.
    if (biome == Biome::scrub) {
        const int pieces = detail ? std::min(3, (detritus + 4) / 6) : std::min(2, (detritus + 5) / 8);
        for (int i = 0; i < pieces; ++i) {
            const auto bit = mix(hash, 100u + unsigned(i));
            const int lx = x + 1 + static_cast<int>(bit % unsigned(tile - 3));
            const int ly = y + 2 + static_cast<int>((bit >> 5) % unsigned(tile - 3));
            const bool twig = (bit >> 10) % 3 != 0;
            if (twig) {
                const int dir = (bit >> 12) & 1 ? 1 : -1;
                pixel(lx, ly, twig_color);
                pixel(lx + 1, ly - (dir > 0 ? 1 : 0), twig_color);
                if (detail) {
                    pixel(lx + 2, ly - (dir > 0 ? 1 : 0), dim(twig_color));
                    pixel(lx + 1, ly + (dir > 0 ? 0 : -1), dim(twig_color));
                }
            } else {
                pixel(lx, ly, pebble_light);
                if (detail) {
                    pixel(lx + 1, ly, pebble_color);
                }
            }
        }
    }
    if (!plants) {
        return;
    }

    const int growth = std::min(4, (plants - 1) / 2);
    const int slots = detail ? 3 : 2;
    const int tufts = detail ? 1 + (plants >= 3) + (plants >= 6) : 1 + (plants >= 4);
    const int rotate = static_cast<int>(hash % unsigned(slots));
    for (int t = 0; t < tufts; ++t) {
        const auto bits = mix(hash, unsigned(t));
        const int slot = (rotate + t) % slots;
        // Bases spread over the lower part of the cell so tufts never form rows.
        const int bx = x + (detail ? 2 + slot * 4 + static_cast<int>(bits % 4) : 1 + slot * 3 + static_cast<int>(bits % 3));
        const int by = y + (detail ? 7 + static_cast<int>((bits >> 3) % 9) : 3 + static_cast<int>((bits >> 3) % 5));
        const int size = std::max(0, growth - t);
        const int lean = std::clamp(plant_lean(index, bits) + flick, detail ? -3 : -2, detail ? 3 : 2);
        switch (biome) {
        case Biome::grassland: {
            // A tussock of blades fanning from one base, with seed heads when lush.
            static constexpr std::array<std::array<int, 5>, 3> fans{{{-2, 0, 2, 0, 0}, {-3, -1, 1, 2, 0}, {-3, -1, 0, 1, 3}}};
            // At zoom 4 a tuft is one upright blade, with a shorter one arching out to each side once it fills in.
            static constexpr std::array<std::array<int, 3>, 2> small_fans{{{0, 0, 0}, {0, -1, 1}}};
            const int blades = detail ? 3 + (size >= 2) + (size >= 4) : plants >= 2 ? 3 : 1;
            const int tall = detail ? 4 + size + static_cast<int>((bits >> 14) & 1) : 3 + size / 2 + static_cast<int>((bits >> 14) & 1);
            int tallest = 0, tallest_slant = 0;
            for (int i = 0; i < blades; ++i) {
                const int slant = detail ? fans[blades - 3][i] : small_fans[blades == 3][i];
                const int height = detail ? std::max(2, tall - (std::abs(slant) >= 3 ? 1 : 0) - static_cast<int>((bits >> (16 + i)) & 1))
                                          : slant ? 2 : tall;
                plant_blade(bx, by, height, slant, lean, greens.base, greens.mid, greens.tip);
                if (height > tallest) {
                    tallest = height;
                    tallest_slant = slant;
                }
            }
            if (plants >= 6 && ((bits >> 9) & 1)) {
                const int k = tallest - 1;
                const int sx = bx + bend(tallest_slant, k, tallest) + bend(lean, k, tallest);
                pixel(sx, by - k, seed_color);
                if (detail) {
                    pixel(sx, by - k - 1, seed_color);
                    pixel(sx + (lean < 0 ? -1 : 1), by - k, dim(seed_color));
                }
            }
            break;
        }
        case Biome::wetland: {
            if (cell.moisture() >= 184 && (bits >> 11) % 3 == 0) {
                // Lily pads float flat on open water, with a bloom when lush.
                if (detail) {
                    fill(bx - 1, by, 5, 1, pad_color);
                    fill(bx, by - 1, 2, 1, pad_light);
                    pixel(bx + 2, by - 1, pad_color);
                    if (size >= 2) {
                        fill(bx + 3, by - 3, 3, 1, pad_color);
                        pixel(bx + 4, by - 4, pad_light);
                    }
                    if (plants >= 5 && ((bits >> 13) & 1)) {
                        pixel(bx + 1, by - 2, bloom_color);
                        pixel(bx + 1, by - 1, rgb565(250, 236, 150));
                    }
                } else {
                    fill(bx, by, 3, 1, pad_color);
                    pixel(bx + 1, by, pad_light);
                    if (plants >= 5 && ((bits >> 13) & 1)) {
                        pixel(bx + 1, by - 1, bloom_color);
                    }
                }
                break;
            }
            // Reeds stand tall and thin; the lushest carry a cattail.
            const int stalks = detail ? 1 + (size >= 1) + (size >= 3) : 1 + (size >= 2);
            for (int i = 0; i < stalks; ++i) {
                const int height = detail ? std::min(12, 7 + size + static_cast<int>((bits >> (14 + 2 * i)) % 3) - i)
                                          : 3 + size / 2 + static_cast<int>((bits >> (14 + i)) & 1) - (i > 0);
                const int sx = bx + i * 2 - (stalks > 1 ? 1 : 0);
                const int slant = static_cast<int>((bits >> (20 + i)) % 3) - 1;
                plant_blade(sx, by, height, detail ? slant : 0, lean, greens.base, greens.mid, greens.tip);
                if (i == 0 && plants >= 4 && ((bits >> 12) & 1)) {
                    const int k = height - (detail ? 3 : 2);
                    const int hx = sx + bend(detail ? slant : 0, k, height) + bend(lean, k, height);
                    if (detail) {
                        fill(hx, by - k - 1, 2, 3, cattail_color);
                    } else {
                        fill(hx, by - k, 1, 2, cattail_color);
                    }
                }
            }
            if (detail) {
                // Sedge at the foot of the clump.
                pixel(bx - 2, by, greens.base);
                pixel(bx - 2 + (lean > 0), by - 1, greens.mid);
                pixel(bx + stalks * 2, by, greens.base);
            }
            break;
        }
        case Biome::scrub: {
            // A stiff, spiky desert plant: it barely moves in the wind.
            const int reach = detail ? 2 + size / 2 : 2;
            plant_blade(bx, by, reach + 1, 0, lean / 2, greens.base, greens.mid, greens.tip);
            for (int k = 1; k <= reach; ++k) {
                const auto color = k == reach ? greens.tip : greens.mid;
                pixel(bx - k, by - k + (detail ? 1 : 0), color);
                pixel(bx + k, by - k + (detail ? 1 : 0), color);
            }
            if (detail) {
                pixel(bx - reach, by, greens.base);
                pixel(bx + reach, by, greens.base);
            } else {
                pixel(bx, by, greens.base);
            }
            break;
        }
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
    // Scrub ground is strewn with stones: a rock in some cells, a pebble in others.
    if (tile >= 8 && cell.biome() == Biome::scrub && (pattern >> 24) % 3 != 2) {
        const bool rock = tile == 16 && (pattern >> 24) % 3 == 0;
        const int rx = static_cast<int>((pattern >> 4) % unsigned(tile - 4)) + 1;
        const int ry = static_cast<int>((pattern >> 12) % unsigned(tile - 4)) + 3;
        if (rock && ((v == ry && u >= rx && u < rx + 3) || (v == ry - 1 && u > rx && u < rx + 3))) {
            return v == ry - 1 || u == rx ? rgb565(120, 108, 100) : rgb565(84, 74, 70);
        }
        if (!rock && v == ry && (u == rx || (tile == 16 && u == rx + 1))) {
            return u == rx ? rgb565(104, 94, 86) : rgb565(76, 66, 62);
        }
        // The driest ground cracks: a zigzag of darker pixels across the cell.
        if (cell.moisture() < 32) {
            const int length = tile == 16 ? 9 : 4;
            const int cx0 = static_cast<int>((pattern >> 18) % unsigned(tile - length)) + 1;
            const int cy0 = static_cast<int>((pattern >> 26) % unsigned(tile - 4)) + 2;
            const int along = u - cx0;
            if (along >= 0 && along < length) {
                const int step = static_cast<int>((pattern >> (along % 8 + 2)) & 1);
                const int wobble = (along / 2 + step) % 3 - 1;
                if (v == cy0 + wobble) {
                    return blend(color, 0, 110);
                }
            }
        }
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

void Panel::draw_terrain_rect(int x0, int y0, int x1, int y1, int tile) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(panel_width, x1);
    y1 = std::min(panel_height, y1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            terrain_.pixels[y * panel_width + x] = ground_pixel(x + camera_.left, y + camera_.top, tile);
        }
    }
    if (x1 > x0 && y1 > y0) {
        terrain_pixels_drawn_ += static_cast<std::uint32_t>((x1 - x0) * (y1 - y0));
    }
}

void Panel::update_terrain(int tile) {
    auto& cache = terrain_;
    const int dx = int(camera_.left) - cache.left, dy = int(camera_.top) - cache.top;
    const bool full = !terrain_cache_enabled_ || !cache.valid || cache.tile != tile ||
                      cache.connected != state_.connected_biomes || std::abs(dx) >= panel_width ||
                      std::abs(dy) >= panel_height;
    if (full) {
        draw_terrain_rect(0, 0, panel_width, panel_height, tile);
        cache.seen.fill({});
    } else if (dx || dy) {
        // Shift what is still on screen, then draw the strips the pan exposed.
        Frame shifted{};
        for (int y = 0; y < panel_height; ++y) {
            for (int x = 0; x < panel_width; ++x) {
                const int sx = x + dx, sy = y + dy;
                if (sx >= 0 && sx < panel_width && sy >= 0 && sy < panel_height) {
                    shifted[y * panel_width + x] = cache.pixels[sy * panel_width + sx];
                }
            }
        }
        cache.pixels = shifted;
        if (dx > 0) {
            draw_terrain_rect(panel_width - dx, 0, panel_width, panel_height, tile);
        } else if (dx < 0) {
            draw_terrain_rect(0, 0, -dx, panel_height, tile);
        }
        if (dy > 0) {
            draw_terrain_rect(0, panel_height - dy, panel_width, panel_height, tile);
        } else if (dy < 0) {
            draw_terrain_rect(0, 0, panel_width, -dy, tile);
        }
    }
    cache.left = camera_.left;
    cache.top = camera_.top;
    cache.tile = tile;
    cache.connected = state_.connected_biomes;
    cache.valid = true;

    // A pixel reads the cells at its dual-grid corners, so a cell affects
    // pixels from half a tile before its origin to half a tile past its end.
    const int first_x = std::max(0, int(camera_.left) / tile - 1);
    const int last_x = std::min(width - 1, (int(camera_.left) + panel_width - 1) / tile + 1);
    const int first_y = std::max(0, int(camera_.top) / tile - 1);
    const int last_y = std::min(height - 1, (int(camera_.top) + panel_height - 1) / tile + 1);
    for (int cy = first_y; cy <= last_y; ++cy) {
        for (int cx = first_x; cx <= last_x; ++cx) {
            const int index = cy * width + cx;
            auto& entry = cache.seen[(cy % TerrainCache::rows) * TerrainCache::columns + cx % TerrainCache::columns];
            const auto& cell = state_.cells[index];
            if (entry.index == index && entry.cell == cell) {
                continue;
            }
            // A cell first seen after a full draw is already correct; only changes redraw.
            if (entry.index == index || !full) {
                const int px = cx * tile - camera_.left, py = cy * tile - camera_.top;
                draw_terrain_rect(px - tile / 2, py - tile / 2, px + tile + tile / 2, py + tile + tile / 2, tile);
            }
            entry.index = static_cast<std::uint16_t>(index);
            entry.cell = cell;
        }
    }
}

int Panel::pixel_cell(int x, int y, int tile) const {
    if (camera_.zoom == 0) {
        constexpr int scale = width / panel_width;
        return (y * scale) * width + x * scale;
    }
    const int wx = std::clamp(x + camera_.left, 0, width * tile - 1);
    const int wy = std::clamp(y + camera_.top, 0, height * tile - 1);
    return (wy / tile) * width + wx / tile;
}

void Panel::grade_and_shimmer(int tile) {
    // The season tints the ground: warmer in the dry half, cooler in the wet half.
    const int season = state_.climate.active ? state_.climate.seasonal : 0;
    const auto toward = season < 0 ? rgb565(255, 180, 60) : rgb565(60, 150, 255);
    const unsigned grade = static_cast<unsigned>(std::abs(season)) * 3;
    if (grade) {
        for (auto& color : frame_) {
            color = blend(color, toward, grade);
        }
    }
    // Open water carries reflections. Each 9x6 patch of world pixels holds one
    // short horizontal glint that fades in, sways one pixel either way, and fades
    // out on its own cycle. Wetland is a real biome, so only it reflects.
    constexpr int patch_w = 9, patch_h = 6;
    constexpr auto glint_color = rgb565(190, 228, 250);
    for (int y = 0; y < panel_height; ++y) {
        const int wy = y + camera_.top;
        for (int x = 0; x < panel_width; ++x) {
            if (state_.cells[pixel_cell(x, y, tile)].biome() != Biome::wetland) {
                continue;
            }
            const int wx = x + camera_.left;
            const auto seed = mix(static_cast<std::uint32_t>(wx / patch_w), static_cast<std::uint32_t>(wy / patch_h));
            const int row = static_cast<int>((seed >> 4) % patch_h);
            if (wy % patch_h != row) {
                continue;
            }
            const std::uint32_t cycle = 1400 + (seed >> 20) % 900;
            const std::uint32_t age = (now_ms_ + seed) % cycle;
            // Visible for the first 30% of the cycle, brightest in the middle.
            const std::uint32_t shown = cycle * 3 / 10;
            if (age >= shown) {
                continue;
            }
            static constexpr int sway[] = {0, 1, 1, 0, -1, -1};
            const int start = static_cast<int>((seed >> 8) % (patch_w - 2)) +
                              sway[(age / 110 + (seed >> 28)) % 6];
            const int length = 2 + static_cast<int>((seed >> 12) & 1);
            const int column = wx % patch_w - start;
            if (column < 0 || column >= length) {
                continue;
            }
            const std::uint32_t rise = age < shown / 2 ? age : shown - age;
            const unsigned alpha = 30 + static_cast<unsigned>(rise * 110 / (shown / 2)) - (column == length - 1 ? 25 : 0);
            auto& color = frame_[y * panel_width + x];
            color = blend(color, glint_color, std::min(140u, alpha));
        }
    }
}

void Panel::draw_weather(int tile) {
    if (!state_.climate.active) {
        return;
    }
    // Drops fall along diagonal lanes in world pixels, one pixel right for every
    // two down, so a streak's three pixels share a lane. Each lane has its own
    // phase, so drops are staggered rather than falling in rows. Lanes are tied to
    // the world, so rain stays put as the camera pans.
    const bool overview = camera_.zoom == 0;
    const int period = overview ? 24 : 14;
    const int fall = static_cast<int>(now_ms_ / (overview ? 40 : 18));
    for (int y = 0; y < panel_height; ++y) {
        const int wy = y + (overview ? 0 : camera_.top);
        for (int x = 0; x < panel_width; ++x) {
            const int rain = rain_at(state_.climate, pixel_cell(x, y, tile)) - rain_threshold;
            if (rain <= 0) {
                continue;
            }
            const int wx = x + (overview ? 0 : camera_.left);
            const int lane = wx - wy / 2;
            const auto lane_seed = mix(static_cast<std::uint32_t>(lane), 0x51ed27u);
            const int along = wy - fall + static_cast<int>(lane_seed % static_cast<unsigned>(period));
            const int offset = ((along % period) + period) % period;
            if (offset > 2) {
                continue;
            }
            const int drop = (along - offset) / period;
            const auto chance = mix(lane_seed, static_cast<std::uint32_t>(drop)) & 255;
            if (chance < static_cast<unsigned>(std::min(120, rain))) {
                auto& color = frame_[y * panel_width + x];
                color = blend(color, rain_color, offset == 2 ? 230 : offset == 1 ? 130 : 60);
            }
        }
    }
}

void Panel::draw_environment(int tile) {
    if (tile >= 4) {
        update_terrain(tile);
        frame_ = terrain_.pixels;
        if (weather_enabled_) {
            grade_and_shimmer(tile);
        }
    }
    const int first_y = camera_.top / tile, last_y = (camera_.top + panel_height - 1) / tile;
    const int first_x = camera_.left / tile, last_x = (camera_.left + panel_width - 1) / tile;
    const bool connected = state_.connected_biomes;
    // Plants reach past their own cell, sideways and upward, so the cells beside
    // and below the view draw their plants too.
    const int reach = tile >= 8 ? 1 : 0;
    for (int y = first_y; y <= std::min(height - 1, last_y + reach); ++y) {
        for (int x = std::max(0, first_x - reach); x <= std::min(width - 1, last_x + reach); ++x) {
            const int index = y * width + x;
            const auto& cell = state_.cells[index];
            const int px = x * tile - camera_.left;
            const int py = y * tile - camera_.top;
            if (y > last_y || x < first_x || x > last_x) {
                if (cell.plants || cell.shrub() || (cell.biome() == Biome::scrub && cell.detritus())) {
                    plant_sprite(px, py, tile, index);
                }
                continue;
            }
            if (tile < 4) {
                const auto terrain = connected ? biome_tint(cell.biome(), cell.plants)
                    : rgb565(8 + cell.detritus() * 3, 8 + std::min(184, cell.plants * 8), 8 + cell.moisture() / 8);
                fill(px, py, tile, tile, terrain);
            }
            if (tile >= 4 && (cell.plants || cell.shrub() || (cell.biome() == Biome::scrub && cell.detritus()))) {
                plant_sprite(px, py, tile, index);
            }
            const bool collapsing = cell.carcass() && std::any_of(events_.begin(), events_.end(), [&](const LifeEvent& event) {
                return event.active && event.dead && event.target.cell == index &&
                    now_ms_ - event.started_at < collapse_time + (event.attacker.id ? attack_impact : 0);
            });
            if (cell.carcass() && !collapsing) {
                if (tile >= 8) {
                    draw_remains(px, py, tile, index);
                } else if (tile == 4) {
                    fill(px, py + 3, 2, 1, carrion_color);
                } else {
                    pixel(px, py + tile - 1, carrion_color);
                }
            }
        }
    }
}

void Panel::draw_remains(int x, int y, int tile, int index, bool bare) {
    // A fresh carcass is a body-sized lump with ribs showing; once scavengers and
    // decay take it below five units, only bones remain.
    const auto& cell = state_.cells[index];
    const bool fresh = !bare && cell.fresh_carcass();
    const bool flip = mix(static_cast<std::uint32_t>(index), 0x6d2b79f5u) & 1;
    const auto put = [&](int col, int row, int w, std::uint16_t color) {
        fill(x + (flip ? tile - col - w : col), y + row, w, 1, color);
    };
    const auto bone = bare ? bone_dim : bone_color;
    if (tile == 8) {
        if (fresh) {
            put(2, 5, 4, meat_color);
            put(1, 6, 6, meat_dark);
            put(3, 5, 1, bone_color);
            put(5, 5, 1, bone_dim);
            put(6, 5, 1, meat_color);
        } else {
            put(1, 6, 1, bone);
            put(2, 6, 4, bone_dim);
            put(3, 5, 1, bone);
            put(5, 5, 1, bone);
            put(6, 6, 1, bone);
        }
        return;
    }
    if (fresh) {
        put(5, 10, 5, meat_color);
        put(3, 11, 9, meat_color);
        put(2, 12, 11, meat_color);
        put(3, 13, 9, meat_dark);
        for (const int col : {5, 7, 9}) {
            put(col, 11, 1, bone_color);
            put(col, 12, 1, bone_dim);
        }
        put(12, 11, 2, bone_color);
        put(13, 10, 1, bone_dim);
    } else {
        put(3, 12, 9, bone_dim);
        for (const int col : {4, 6, 8, 10}) {
            put(col, 12, 1, bone);
        }
        for (const int col : {5, 7, 9}) {
            put(col, 11, 1, bone);
            put(col, 13, 1, bone_dim);
        }
        put(12, 11, 2, bone);
        put(12, 12, 2, bone);
        put(13, 11, 1, rgb565(40, 32, 30));
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
                       fading, actor.look.faction(), height, build_of(actor.look));
            }
        } else if (state_.cells[actor.cell].carcass()) {
            draw_remains(px, py, tile, actor.cell);
        } else if (age < (death_visible + collapse_time + delay) / 2) {
            // Eaten already: a moment of bare bones before they fade.
            draw_remains(px, py, tile, actor.cell, true);
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
        // The habitat view shows where it is raining.
        if (weather_enabled_ && rain_enabled_) {
            draw_weather(tile);
        }
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
            sprite(px, py, 1, creature.look.shape(), face_right, walking, bite, color, creature.look.faction(), 7,
                   build_of(creature.look));
        }
        if (bite) {
            pixel(px + (face_right ? tile - 1 : 0), py + (detail ? 11 : 4),
                  attacking || subject_.meat ? rgb565(230, 42, 110) : rgb565(69, 180, 62));
        }
    }
    draw_contact(tile);
    draw_grazing(tile);
    if (weather_enabled_ && rain_enabled_) {
        draw_weather(tile);
    }
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
