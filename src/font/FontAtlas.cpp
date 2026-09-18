#include "vectis/font/FontAtlas.hpp"

#include <fontconfig/fontconfig.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace vectis::font {

FontAtlas::FontAtlas(const std::string& font_name, int font_size) : font_size_(font_size) {
    if (FT_Init_FreeType(&ft_)) {
        throw std::runtime_error("[Vectis Font]: FreeType init failed");
    }

    std::string reg_path = find_font_path(font_name, "Regular");
    if (reg_path.empty()) reg_path = "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf";

    std::string bold_path = find_font_path(font_name, "Bold");
    if (bold_path.empty()) bold_path = "/usr/share/fonts/TTF/JetBrainsMono-Bold.ttf";

    std::string ital_path = find_font_path(font_name, "Italic");
    if (ital_path.empty()) ital_path = "/usr/share/fonts/TTF/JetBrainsMono-Italic.ttf";

    std::string bi_path = find_font_path(font_name, "Bold Italic");
    if (bi_path.empty()) bi_path = bold_path;

    faces_[REGULAR] = get_or_load_face(reg_path);
    try { faces_[BOLD] = get_or_load_face(bold_path); } catch (...) { faces_[BOLD] = faces_[REGULAR]; }
    try { faces_[ITALIC] = get_or_load_face(ital_path); } catch (...) { faces_[ITALIC] = faces_[REGULAR]; }
    try { faces_[BOLD_ITALIC] = get_or_load_face(bi_path); } catch (...) { faces_[BOLD_ITALIC] = faces_[BOLD]; }

    cell_width_ = (faces_[REGULAR]->size->metrics.max_advance >> 6);
    cell_height_ = ((faces_[REGULAR]->size->metrics.ascender - faces_[REGULAR]->size->metrics.descender) >> 6);
    if (cell_width_ <= 0) cell_width_ = static_cast<int>(font_size * 0.6f);
    if (cell_height_ <= 0) cell_height_ = static_cast<int>(font_size * 1.2f);

    glGenTextures(1, &texture_id_);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlas_w_, atlas_h_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    for (char32_t c = 32; c < 127; ++c) {
        load_glyph_to_atlas(c, REGULAR);
    }
}

FontAtlas::~FontAtlas() {
    for (auto& [path, face] : loaded_faces_) {
        FT_Done_Face(face);
    }
    if (ft_) {
        FT_Done_FreeType(ft_);
    }
    if (texture_id_ != 0) {
        glDeleteTextures(1, &texture_id_);
    }
}

FT_Face FontAtlas::get_or_load_face(const std::string& font_path) {
    auto it = loaded_faces_.find(font_path);
    if (it != loaded_faces_.end()) return it->second;

    FT_Face face;
    if (FT_New_Face(ft_, font_path.c_str(), 0, &face)) {
        throw std::runtime_error("[Vectis Font]: Failed to load: " + font_path);
    }

    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(font_size_));
    loaded_faces_[font_path] = face;
    return face;
}

std::string FontAtlas::find_font_path(const std::string& font_name, const std::string& style_str) {
    FcConfig* config = FcInitLoadConfigAndFonts();
    std::string query = font_name + ":style=" + style_str;
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(query.c_str()));
    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern* match = FcFontMatch(config, pat, &result);
    std::string path;

    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            path = reinterpret_cast<char*>(file);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    FcConfigDestroy(config);
    return path;
}

std::string FontAtlas::find_fallback_font(char32_t cp) {
    FcConfig* config = FcInitLoadConfigAndFonts();
    FcPattern* pat = FcPatternCreate();
    FcCharSet* cs = FcCharSetCreate();

    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>("monospace"));
    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern* match = FcFontMatch(config, pat, &result);
    std::string path;

    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            path = reinterpret_cast<char*>(file);
        }
        FcPatternDestroy(match);
    }

    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    FcConfigDestroy(config);
    return path;
}

bool FontAtlas::load_glyph_to_atlas(char32_t cp, FontStyle style) {
    uint64_t key = (static_cast<uint64_t>(cp) << 2) | static_cast<uint64_t>(style);
    if (glyphs_.find(key) != glyphs_.end()) return true;

    FT_Face face_to_use = faces_[style];
    if (FT_Get_Char_Index(face_to_use, cp) == 0) {
        std::string fallback_path = find_fallback_font(cp);
        if (!fallback_path.empty()) {
            try {
                face_to_use = get_or_load_face(fallback_path);
            } catch (...) {
                face_to_use = faces_[REGULAR];
            }
        }
    }

    if (FT_Load_Char(face_to_use, cp, FT_LOAD_RENDER)) {
        return false;
    }

    FT_GlyphSlot g = face_to_use->glyph;

    if (cur_x_ + g->bitmap.width + 1 >= static_cast<unsigned int>(atlas_w_)) {
        cur_x_ = 1;
        cur_y_ += max_row_h_ + 1;
        max_row_h_ = 0;
    }

    if (cur_y_ + g->bitmap.rows + 1 >= static_cast<unsigned int>(atlas_h_)) {
        return false;
    }

    if (g->bitmap.width > 0 && g->bitmap.rows > 0) {
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, cur_x_, cur_y_,
                        static_cast<GLsizei>(g->bitmap.width),
                        static_cast<GLsizei>(g->bitmap.rows),
                        GL_RED, GL_UNSIGNED_BYTE, g->bitmap.buffer);
    }

    GlyphInfo info{};
    info.uv_x0 = static_cast<float>(cur_x_) / atlas_w_;
    info.uv_y0 = static_cast<float>(cur_y_) / atlas_h_;
    info.uv_x1 = static_cast<float>(cur_x_ + g->bitmap.width) / atlas_w_;
    info.uv_y1 = static_cast<float>(cur_y_ + g->bitmap.rows) / atlas_h_;
    info.width = static_cast<int>(g->bitmap.width);
    info.height = static_cast<int>(g->bitmap.rows);
    info.bearing_x = g->bitmap_left;
    info.bearing_y = g->bitmap_top;
    info.advance = static_cast<int>(g->advance.x >> 6);

    glyphs_[key] = info;

    cur_x_ += g->bitmap.width + 1;
    max_row_h_ = std::max(max_row_h_, static_cast<int>(g->bitmap.rows));

    return true;
}

bool FontAtlas::has_glyph(char32_t cp, FontStyle style) {
    uint64_t key = (static_cast<uint64_t>(cp) << 2) | static_cast<uint64_t>(style);
    if (glyphs_.find(key) != glyphs_.end()) return true;
    return load_glyph_to_atlas(cp, style);
}

const GlyphInfo& FontAtlas::glyph(char32_t cp, FontStyle style) {
    uint64_t key = (static_cast<uint64_t>(cp) << 2) | static_cast<uint64_t>(style);
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return it->second;

    if (load_glyph_to_atlas(cp, style)) {
        return glyphs_[key];
    }
    return glyphs_[static_cast<uint64_t>(U' ') << 2];
}

} // namespace vectis::font
