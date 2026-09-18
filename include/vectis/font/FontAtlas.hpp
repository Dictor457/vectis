#pragma once

#include <epoxy/gl.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <string>
#include <unordered_map>
#include <array>
#include <cstdint>

namespace vectis::font {

enum FontStyle : uint8_t {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3
};

struct GlyphInfo {
    float uv_x0{0.0f}, uv_y0{0.0f};
    float uv_x1{0.0f}, uv_y1{0.0f};
    int width{0};
    int height{0};
    int bearing_x{0};
    int bearing_y{0};
    int advance{0};
};

class FontAtlas {
public:
    FontAtlas(const std::string& font_name, int font_size);
    ~FontAtlas();

    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    [[nodiscard]] GLuint texture_id() const noexcept { return texture_id_; }
    [[nodiscard]] const GlyphInfo& glyph(char32_t cp, FontStyle style = REGULAR);
    [[nodiscard]] bool has_glyph(char32_t cp, FontStyle style = REGULAR);

    [[nodiscard]] int cell_width() const noexcept { return cell_width_; }
    [[nodiscard]] int cell_height() const noexcept { return cell_height_; }

private:
    std::string find_font_path(const std::string& font_name, const std::string& style_str);
    std::string find_fallback_font(char32_t cp);

    bool load_glyph_to_atlas(char32_t cp, FontStyle style);
    FT_Face get_or_load_face(const std::string& font_path);

    FT_Library ft_{nullptr};
    std::array<FT_Face, 4> faces_{nullptr, nullptr, nullptr, nullptr};
    std::unordered_map<std::string, FT_Face> loaded_faces_;

    GLuint texture_id_{0};
    std::unordered_map<uint64_t, GlyphInfo> glyphs_;

    int font_size_{18};
    int cell_width_{0};
    int cell_height_{0};

    const int atlas_w_{2048};
    const int atlas_h_{2048};
    int cur_x_{1};
    int cur_y_{1};
    int max_row_h_{0};
};

} // namespace vectis::font
