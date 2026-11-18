// Renders panel frames into a PNG contact sheet for reviewing graphics without the board.
//
//   petri_snapshot OUTPUT.png [--seed N] [--ticks N] [--frames N] [--interval-ms N] [--rain 0|1] [--ticks-per-frame N]
//                  [--biome 0|1|2] [--x CELL --y CELL] [--storm 1]
//
// By default the close views follow the first creature. --biome holds the camera
// over the most vegetated stretch of scrub (0), grassland (1), or wetland (2);
// --x and --y hold it over a chosen cell; --storm 1 over the strongest storm.
// Rows are views (close at zoom 4, detail at zoom 5, zoom 3, life, habitat);
// columns are consecutive frames. Each LED is drawn as a 6x6 block with a dark gap.

#include "observation.hpp"
#include "panel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace petri;

std::uint32_t crc_table[256];

void init_crc() {
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

std::uint32_t crc(const std::uint8_t* data, std::size_t size, std::uint32_t c = 0xffffffffu) {
    for (std::size_t i = 0; i < size; ++i) {
        c = crc_table[(c ^ data[i]) & 255] ^ (c >> 8);
    }
    return c;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void chunk(std::vector<std::uint8_t>& png, const char* type, const std::vector<std::uint8_t>& data) {
    put32(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    put32(png, crc(body.data(), body.size()) ^ 0xffffffffu);
}

// Writes an RGB image as a PNG with stored (uncompressed) deflate blocks.
bool write_png(const char* path, int w, int h, const std::vector<std::uint8_t>& rgb) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const auto n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(n & 255);
        z.push_back(n >> 8);
        z.push_back(~n & 255);
        z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    }
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* file = std::fopen(path, "wb");
    if (!file) {
        return false;
    }
    std::fwrite(png.data(), 1, png.size(), file);
    return std::fclose(file) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s OUTPUT.png [--seed N] [--ticks N] [--frames N] [--interval-ms N]\n", argv[0]);
        return 2;
    }
    std::uint32_t seed = 14;
    unsigned ticks = 3000, frames = 6, interval = 100;
    bool rain = true;
    unsigned ticks_per_frame = 0;
    int biome = -1, at_x = -1, at_y = -1;
    bool storm = false;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const auto value = static_cast<unsigned>(std::strtoul(argv[i + 1], nullptr, 10));
        if (option == "--seed") seed = value;
        else if (option == "--ticks") ticks = value;
        else if (option == "--frames") frames = value;
        else if (option == "--interval-ms") interval = value;
        else if (option == "--rain") rain = value != 0;
        else if (option == "--ticks-per-frame") ticks_per_frame = value;
        else if (option == "--biome") biome = int(value);
        else if (option == "--x") at_x = int(value);
        else if (option == "--y") at_y = int(value);
        else if (option == "--storm") storm = value != 0;
    }
    init_crc();
    auto world = std::make_unique<World>(seed, observatory_config());
    for (unsigned t = 0; t < ticks; ++t) {
        world->step();
    }
    if (biome >= 0) {
        // The 8x4-cell window (the zoom 4 view) with the most of that biome and its plants or litter.
        int best = -1;
        for (int y = 0; y + 4 <= height; ++y) {
            for (int x = 0; x + 8 <= width; ++x) {
                int score = 0;
                for (int dy = 0; dy < 4; ++dy) {
                    for (int dx = 0; dx < 8; ++dx) {
                        const auto& cell = world->cells()[(y + dy) * width + x + dx];
                        if (int(biome_at(cell.moisture)) == biome) {
                            score += 16 + std::min<int>(8, cell.plants) + std::min<int>(4, cell.detritus);
                        }
                    }
                }
                if (score > best) {
                    best = score;
                    at_x = x + 4;
                    at_y = y + 2;
                }
            }
        }
    }
    if (storm) {
        int strongest = 0;
        for (const auto& front : world->storms()) {
            if (front.active() && front.strength > strongest) {
                strongest = front.strength;
                at_x = std::clamp(int(front.x / 256), 0, width - 1);
                at_y = std::clamp(int(front.y / 256), 0, height - 1);
            }
        }
        if (!strongest) {
            std::fprintf(stderr, "no active storm at this tick\n");
        }
    }
    const bool pinned = at_x >= 0 && at_y >= 0;
    struct View {
        PanelView view;
        std::uint8_t zoom;
    };
    const View views[] = {{PanelView::close, 4}, {PanelView::close, 5}, {PanelView::close, 3},
                          {PanelView::life, 0}, {PanelView::habitat, 0}};
    constexpr int dot = 6, gap = 1, cell = dot + gap, margin = 8;
    const int sheet_w = margin + int(frames) * (panel_width * cell + margin);
    const int sheet_h = margin + int(std::size(views)) * (panel_height * cell + margin);
    std::vector<std::uint8_t> rgb(std::size_t(sheet_w) * sheet_h * 3, 12);
    int row = 0;
    // Each view row replays the same stretch of time from the same starting world.
    Checkpoint start{};
    world->save_checkpoint(start);
    for (const auto& v : views) {
        world->load_checkpoint(start);
        auto panel = std::make_unique<LocalPanel>(*world);
        panel->panel().set_rain(rain);
        const auto follow = world->creatures()[0].alive() ? world->creatures()[0].id : 0;
        std::uint32_t now = 1000;
        if (pinned && v.zoom) {
            const int tile = 1 << (v.zoom - 1);
            const auto origin = cell_origin(std::size_t(at_y * width + at_x), tile);
            const int left = std::clamp(origin[0] + tile / 2 - panel_width / 2, 0, scene_width(tile) - panel_width);
            const int top = std::clamp(origin[1] + tile / 2 - panel_height / 2, 0, height * tile - panel_height);
            panel->set_camera({v.view, v.zoom, std::uint16_t(left), std::uint16_t(top), 0}, now);
        } else {
            panel->set_camera({v.view, v.zoom, 0, 0, v.view == PanelView::close ? follow : 0}, now);
        }
        for (int warm = 0; warm < 40; ++warm) {
            now += 33;
            panel->draw(now, now * 1000, 0);
        }
        for (unsigned f = 0; f < frames; ++f) {
            now += interval;
            for (unsigned t = 0; t < ticks_per_frame; ++t) {
                world->step(panel->before_step(now));
                panel->after_step(now * 1000, interval * 1000);
            }
            panel->draw(now, now * 1000, 0);
            const int ox = margin + int(f) * (panel_width * cell + margin);
            const int oy = margin + row * (panel_height * cell + margin);
            for (int y = 0; y < panel_height; ++y) {
                for (int x = 0; x < panel_width; ++x) {
                    const auto c = panel->frame()[y * panel_width + x];
                    const std::uint8_t r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
                    for (int dy = 0; dy < dot; ++dy) {
                        for (int dx = 0; dx < dot; ++dx) {
                            auto* p = &rgb[(std::size_t(oy + y * cell + dy) * sheet_w + ox + x * cell + dx) * 3];
                            p[0] = r;
                            p[1] = g;
                            p[2] = b;
                        }
                    }
                }
            }
        }
        ++row;
    }
    if (!write_png(argv[1], sheet_w, sheet_h, rgb)) {
        std::fprintf(stderr, "could not write %s\n", argv[1]);
        return 1;
    }
    std::printf("wrote %s (%dx%d) tick=%llu weather_active=%d\n", argv[1], sheet_w, sheet_h,
                (unsigned long long)world->metrics().ticks, world->weather_active() ? 1 : 0);
}
