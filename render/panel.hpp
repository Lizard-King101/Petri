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

class Panel {
public:
    explicit Panel(const World& world) : world_(world) {}

    void cycle_view(std::uint32_t now_ms);
    void set_camera(Camera camera, std::uint32_t now_ms);
    StepObserver before_step(std::uint32_t now_ms);
    void after_step(std::uint32_t now_us, std::uint32_t interval);
    void draw(std::uint32_t now_ms, std::uint32_t now_us, unsigned speed_index);

    const Camera& camera() const { return camera_; }
    const Frame& frame() const { return frame_; }
    PanelPoint location(std::size_t slot) const { return locations_[slot]; }

private:
    enum class DietShape { meat, mixed, plant };

    struct Actor {
        std::uint64_t id = 0;
        std::uint16_t cell = 0;
        Genome genes{};
        bool face_right = true;
    };
    struct LifeEvent {
        Actor target, attacker;
        std::uint32_t started_at = 0;
        bool active = false, dead = false;
    };

    void mark_hunt(std::uint16_t prey);
    void mark_meal(const Creature& creature, bool meat);
    void mark_attack(const Creature& attacker, const Creature& target, bool landed);
    void mark_death(const Creature& creature, DeathCause cause);
    Actor capture_actor(const Creature& creature) const;
    LifeEvent* start_event(const Creature& target, const Creature* attacker);
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
    static DietShape diet_shape(const Genome& genes);
    void sprite(int x, int y, int scale, DietShape diet, bool face_right,
                bool stride, bool mouth_open, std::uint16_t body, std::uint8_t faction = 0, int height = 7);
    void detail_sprite(int x, int y, const Genome& genes, bool face_right,
                       bool stride, bool eating, std::uint16_t body, int height = 16);
    void fill(int x, int y, int w, int h, std::uint16_t color);
    void pixel(int x, int y, std::uint16_t color);

    const World& world_;
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

} // namespace petri
