#pragma once

#include "life.hpp"

namespace petri {

enum class PanelView : std::uint8_t { close, hunt, life, habitat };
inline constexpr unsigned sprite_zoom = 4;
inline constexpr unsigned detail_zoom = 5;

struct Camera {
    PanelView view = PanelView::life;
    std::uint8_t zoom = 0;
    std::uint16_t left = 0, top = 0;
    std::uint64_t follow = 0;
};

struct PanelPoint {
    static constexpr std::int8_t hidden = -128;
    std::int8_t x = hidden, y = hidden;
};

// Draws a RenderState. When a new state arrives, call begin_update before the
// state changes, apply the events reported with it, then call end_update.
class Panel {
public:
    explicit Panel(const RenderState& state) : state_(state) {}

    void cycle_view(std::uint32_t now_ms);
    void set_camera(Camera camera, std::uint32_t now_ms);
    void begin_update(std::uint32_t now_ms);
    void apply(const RenderEvent& event);
    void apply(const EventLog& events) {
        for (const auto& event : events) {
            apply(event);
        }
    }
    void end_update(std::uint32_t now_us, std::uint32_t interval);
    void draw(std::uint32_t now_ms, std::uint32_t now_us, unsigned speed_index);

    const Camera& camera() const { return camera_; }
    const Frame& frame() const { return frame_; }
    PanelPoint location(std::size_t slot) const { return locations_[slot]; }

private:
    struct Actor {
        std::uint64_t id = 0;
        std::uint16_t cell = 0;
        Look look{};
        bool face_right = true;
    };
    struct LifeEvent {
        Actor target, attacker;
        std::uint32_t started_at = 0;
        bool active = false, dead = false;
    };

    void mark_hunt(bool wet);
    void mark_meal(std::uint64_t id, bool meat);
    void mark_attack(const RenderActor& attacker, const RenderActor& target, bool landed);
    void mark_death(const RenderActor& creature, bool predation);
    Actor capture_actor(const RenderActor& creature) const;
    LifeEvent* start_event(const RenderActor& target, const RenderActor* attacker);
    const LifeEvent* attack_for(std::uint64_t id) const;
    void update_camera(int tile, std::uint32_t elapsed, unsigned speed_index);
    std::array<int, 2> position(std::size_t slot, int tile, std::uint32_t elapsed) const;
    void draw_grid(bool show_creatures, int tile, std::uint32_t elapsed);
    void draw_close(int tile, std::uint32_t elapsed);
    void draw_environment(int tile);
    std::uint16_t ground_pixel(int x, int y, int tile) const;
    void draw_deaths(int tile);
    void draw_contact(int tile);
    void draw_hunt();
    void plant_sprite(int x, int y, int tile, int index);
    void sprite(int x, int y, int scale, DietShape diet, bool face_right,
                bool stride, bool mouth_open, std::uint16_t body, std::uint8_t faction = 0, int height = 7);
    void detail_sprite(int x, int y, Look look, bool face_right,
                       bool stride, bool eating, std::uint16_t body, int height = 16);
    void fill(int x, int y, int w, int h, std::uint16_t color);
    void pixel(int x, int y, std::uint16_t color);

    const RenderState& state_;
    Frame frame_{};
    Camera camera_{PanelView::close, sprite_zoom};
    struct Previous {
        std::uint64_t id = 0;
        std::uint16_t cell = 0;
    };
    std::array<Previous, capacity> previous_{};
    std::array<PanelPoint, capacity> locations_{};
    std::array<LifeEvent, 12> events_{};
    std::uint32_t now_ms_ = 0, motion_start_ = 0, motion_duration_ = 0;
    bool auto_follow_ = true, center_ = true;
    bool placed_ = false, framing_ = false;
    struct {
        std::uint16_t left = 0, top = 0;
        std::uint32_t started_at = 0, duration = 0;
        bool active = false;
    } focus_;
    struct {
        std::uint64_t id = 0, attacker = 0;
        std::uint16_t cell = 0;
        std::uint32_t started_at = 0;
    } dying_;

    struct {
        std::uint32_t started_at = 0, quiet_since = 0;
        bool wet = false, active = false;
    } hunt_;
    struct {
        std::uint32_t ate_at = 0, moved_at = 0;
        std::uint16_t cell = 0;
        bool fed = false, meat = false, face_right = true;
    } subject_;
};

// A Panel for a World on the same chip. It keeps its own RenderState in step
// with the world and offers the step hooks the firmware and tools use.
class LocalPanel {
public:
    explicit LocalPanel(const World& world) : world_(world), panel_(state_) { capture(world_, state_); }

    StepObserver before_step(std::uint32_t now_ms) {
        sync();
        panel_.begin_update(now_ms);
        events_.clear();
        return events_.observer(world_);
    }
    void after_step(std::uint32_t now_us, std::uint32_t interval) {
        capture(world_, state_);
        panel_.apply(events_);
        panel_.end_update(now_us, interval);
    }
    void draw(std::uint32_t now_ms, std::uint32_t now_us, unsigned speed_index) {
        sync();
        panel_.draw(now_ms, now_us, speed_index);
    }
    // Recaptures the world when it has moved on without the step hooks.
    void sync() {
        if (state_.tick != world_.metrics().ticks) {
            capture(world_, state_);
        }
    }

    void cycle_view(std::uint32_t now_ms) { panel_.cycle_view(now_ms); }
    void set_camera(Camera camera, std::uint32_t now_ms) { panel_.set_camera(camera, now_ms); }
    const Camera& camera() const { return panel_.camera(); }
    const Frame& frame() const { return panel_.frame(); }
    PanelPoint location(std::size_t slot) const { return panel_.location(slot); }
    Panel& panel() { return panel_; }
    const Panel& panel() const { return panel_; }
    const RenderState& state() const { return state_; }

private:
    const World& world_;
    RenderState state_{};
    EventLog events_{};
    Panel panel_;
};

} // namespace petri
