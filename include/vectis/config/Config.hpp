#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <array>
#include <algorithm>

namespace vectis::config {

struct ColorRGB {
    float r{0.88f};
    float g{0.90f};
    float b{0.94f};

    static ColorRGB from_hex(std::string_view hex);

    [[nodiscard]] uint8_t r_byte() const noexcept { return static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f)); }
    [[nodiscard]] uint8_t g_byte() const noexcept { return static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f)); }
    [[nodiscard]] uint8_t b_byte() const noexcept { return static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f)); }
};

struct Config {
    std::string font_family{"JetBrains Mono"};
    int font_size{18};
    int window_width{1024};
    int window_height{720};
    float opacity{0.90f};
    int padding_x{12};
    int padding_y{12};

    // Стартовая директория: inherit (где вызван) или home (домашняя папка ~)
    std::string working_directory{"inherit"};

    std::string cursor_shape{"beam"};
    bool cursor_blink{true};

    ColorRGB background{0.09f, 0.10f, 0.13f};
    ColorRGB foreground{0.88f, 0.90f, 0.94f};
    ColorRGB cursor{0.35f, 0.65f, 0.98f};

    std::array<ColorRGB, 16> colors{
        ColorRGB::from_hex("#15161e"),
        ColorRGB::from_hex("#f7768e"),
        ColorRGB::from_hex("#9ece6a"),
        ColorRGB::from_hex("#e0af68"),
        ColorRGB::from_hex("#7aa2f7"),
        ColorRGB::from_hex("#bb9af7"),
        ColorRGB::from_hex("#7dcfff"),
        ColorRGB::from_hex("#a9b1d6"),

        ColorRGB::from_hex("#414868"),
        ColorRGB::from_hex("#f7768e"),
        ColorRGB::from_hex("#9ece6a"),
        ColorRGB::from_hex("#e0af68"),
        ColorRGB::from_hex("#7aa2f7"),
        ColorRGB::from_hex("#bb9af7"),
        ColorRGB::from_hex("#7dcfff"),
        ColorRGB::from_hex("#c0caf5")
    };

    static Config load();
};

} // namespace vectis::config
