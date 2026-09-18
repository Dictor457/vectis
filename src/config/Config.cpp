#include "vectis/config/Config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace vectis::config {

namespace fs = std::filesystem;

static std::string trim(std::string_view str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(start, end - start + 1));
}

ColorRGB ColorRGB::from_hex(std::string_view hex) {
    if (hex.empty()) return ColorRGB{};
    if (hex[0] == '#') hex.remove_prefix(1);
    if (hex.length() < 6) return ColorRGB{};

    try {
        uint32_t val = static_cast<uint32_t>(std::stoul(std::string(hex.substr(0, 6)), nullptr, 16));
        float r = static_cast<float>((val >> 16) & 0xFF) / 255.0f;
        float g = static_cast<float>((val >> 8) & 0xFF) / 255.0f;
        float b = static_cast<float>(val & 0xFF) / 255.0f;
        return ColorRGB{r, g, b};
    } catch (...) {
        return ColorRGB{};
    }
}

static fs::path get_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return fs::path(xdg) / "vectis" / "vectis.conf";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return fs::path(home) / ".config" / "vectis" / "vectis.conf";
    }
    return "vectis.conf";
}

static void create_default_config(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out.is_open()) return;

    out << "# =========================================\n"
        << "# Vectis GPU Terminal Configuration\n"
        << "# =========================================\n\n"
        << "font_family = JetBrains Mono\n"
        << "font_size = 18\n\n"
        << "window_width = 1024\n"
        << "window_height = 720\n\n"
        << "opacity = 0.90\n\n"
        << "padding_x = 12\n"
        << "padding_y = 12\n\n"
        << "# Стартовая папка: inherit (где запущен) или home (всегда в ~)\n"
        << "working_directory = inherit\n\n"
        << "cursor_shape = beam\n"
        << "cursor_blink = true\n\n"
        << "background = #101216\n"
        << "foreground = #e0e2e8\n"
        << "cursor = #52adfa\n\n"
        << "# 16 Цветов палитры ANSI (Tokyo Night Theme)\n"
        << "color0 = #15161e\n"
        << "color1 = #f7768e\n"
        << "color2 = #9ece6a\n"
        << "color3 = #e0af68\n"
        << "color4 = #7aa2f7\n"
        << "color5 = #bb9af7\n"
        << "color6 = #7dcfff\n"
        << "color7 = #a9b1d6\n"
        << "color8 = #414868\n"
        << "color9 = #f7768e\n"
        << "color10 = #9ece6a\n"
        << "color11 = #e0af68\n"
        << "color12 = #7aa2f7\n"
        << "color13 = #bb9af7\n"
        << "color14 = #7dcfff\n"
        << "color15 = #c0caf5\n";
}

Config Config::load() {
    Config cfg;
    fs::path path = get_config_path();

    if (!fs::exists(path)) {
        create_default_config(path);
        return cfg;
    }

    std::ifstream file(path);
    if (!file.is_open()) return cfg;

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, eq_pos));
        std::string val = trim(trimmed.substr(eq_pos + 1));

        if (key == "font_family") cfg.font_family = val;
        else if (key == "font_size") cfg.font_size = std::stoi(val);
        else if (key == "window_width") cfg.window_width = std::stoi(val);
        else if (key == "window_height") cfg.window_height = std::stoi(val);
        else if (key == "opacity") cfg.opacity = std::stof(val);
        else if (key == "padding_x") cfg.padding_x = std::stoi(val);
        else if (key == "padding_y") cfg.padding_y = std::stoi(val);
        else if (key == "working_directory") cfg.working_directory = val;
        else if (key == "cursor_shape") cfg.cursor_shape = val;
        else if (key == "cursor_blink") cfg.cursor_blink = (val == "true" || val == "1");
        else if (key == "background") cfg.background = ColorRGB::from_hex(val);
        else if (key == "foreground") cfg.foreground = ColorRGB::from_hex(val);
        else if (key == "cursor") cfg.cursor = ColorRGB::from_hex(val);
        else if (key.starts_with("color")) {
            try {
                int idx = std::stoi(key.substr(5));
                if (idx >= 0 && idx < 16) {
                    cfg.colors[static_cast<size_t>(idx)] = ColorRGB::from_hex(val);
                }
            } catch (...) {}
        }
    }

    return cfg;
}

} // namespace vectis::config
