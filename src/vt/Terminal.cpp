#include "vectis/vt/Terminal.hpp"
#include <stdexcept>
#include <algorithm>

namespace vectis::vt {

const VTermScreenCallbacks Terminal::s_callbacks = []() {
    VTermScreenCallbacks cb{};
    cb.damage = &Terminal::cb_damage;
    cb.movecursor = &Terminal::cb_movecursor;
    cb.sb_pushline = &Terminal::cb_sb_pushline;
    cb.settermprop = &Terminal::cb_settermprop;
    return cb;
}();

Terminal::Terminal(int rows, int cols) : rows_(rows), cols_(cols) {
    vt_ = vterm_new(rows_, cols_);
    if (!vt_) {
        throw std::runtime_error("Failed to allocate VTerm instance");
    }

    vterm_set_utf8(vt_, 1);
    vts_ = vterm_obtain_screen(vt_);
    vterm_screen_enable_altscreen(vts_, 1);

    vterm_screen_set_callbacks(vts_, &s_callbacks, this);
    vterm_screen_reset(vts_, 1);
}

Terminal::~Terminal() {
    if (vt_) {
        vterm_free(vt_);
    }
}

void Terminal::set_palette_color(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (!vt_ || index < 0 || index >= 16) return;
    VTermColor col{};
    col.type = VTERM_COLOR_RGB;
    col.rgb.red = r;
    col.rgb.green = g;
    col.rgb.blue = b;
    vterm_state_set_palette_color(vterm_obtain_state(vt_), index, &col);
}

void Terminal::feed(std::string_view bytes) {
    if (!vt_ || bytes.empty()) return;
    vterm_input_write(vt_, bytes.data(), bytes.size());
}

void Terminal::resize(int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    rows_ = rows;
    cols_ = cols;
    vterm_set_size(vt_, rows_, cols_);
    vterm_screen_flush_damage(vts_);
}

void Terminal::scroll_up(int lines) {
    scroll_offset_ = std::min(scroll_offset_ + lines, static_cast<int>(scrollback_.size()));
}

void Terminal::scroll_down(int lines) {
    scroll_offset_ = std::max(scroll_offset_ - lines, 0);
}

void Terminal::scroll_to_bottom() noexcept {
    scroll_offset_ = 0;
}

int Terminal::line_length(int row) const {
    if (row < 0 || row >= rows_) return 0;
    for (int c = cols_ - 1; c >= 0; --c) {
        auto cell = get_cell(row, c);
        if (cell.codepoint > 32 || cell.has_bg) {
            return c + 1;
        }
    }
    return 0;
}

Cell Terminal::get_cell(int row, int col) const {
    Cell result{};
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) {
        return result;
    }

    int total_history = static_cast<int>(scrollback_.size());
    int target_idx = total_history - scroll_offset_ + row;

    if (target_idx < total_history) {
        if (target_idx >= 0 && target_idx < total_history) {
            const auto& line = scrollback_[static_cast<size_t>(target_idx)];
            if (col < static_cast<int>(line.size())) {
                return line[static_cast<size_t>(col)];
            }
        }
        return result;
    }

    int screen_row = target_idx - total_history;
    if (!vts_ || screen_row < 0 || screen_row >= rows_) {
        return result;
    }

    VTermPos pos{screen_row, col};
    VTermScreenCell vcell{};
    if (vterm_screen_get_cell(vts_, pos, &vcell)) {
        result.codepoint = vcell.chars[0] != 0 ? vcell.chars[0] : ' ';
        result.bold = (vcell.attrs.bold != 0);
        result.italic = (vcell.attrs.italic != 0);
        result.underline = (vcell.attrs.underline != 0);
        result.has_bg = (vcell.bg.type == VTERM_COLOR_RGB || vcell.bg.type == VTERM_COLOR_INDEXED);

        vterm_screen_convert_color_to_rgb(vts_, &vcell.fg);
        result.fg = Color{vcell.fg.rgb.red, vcell.fg.rgb.green, vcell.fg.rgb.blue, 255};

        if (result.has_bg) {
            vterm_screen_convert_color_to_rgb(vts_, &vcell.bg);
            result.bg = Color{vcell.bg.rgb.red, vcell.bg.rgb.green, vcell.bg.rgb.blue, 255};
        }
    }
    return result;
}

Cursor Terminal::cursor() const noexcept {
    Cursor cur = cursor_;
    cur.shape = cursor_shape_;
    if (scroll_offset_ > 0) {
        cur.visible = false;
    }
    return cur;
}

int Terminal::cb_damage(VTermRect, void*) {
    return 1;
}

int Terminal::cb_movecursor(VTermPos pos, VTermPos, int visible, void* user) {
    auto* self = static_cast<Terminal*>(user);
    self->cursor_.row = pos.row;
    self->cursor_.col = pos.col;
    self->cursor_.visible = (visible != 0);
    return 1;
}

int Terminal::cb_sb_pushline(int cols, const VTermScreenCell* cells, void* user) {
    auto* self = static_cast<Terminal*>(user);
    std::vector<Cell> line(static_cast<size_t>(cols));

    for (int c = 0; c < cols; ++c) {
        line[c].codepoint = cells[c].chars[0] != 0 ? cells[c].chars[0] : ' ';
        line[c].bold = (cells[c].attrs.bold != 0);
        line[c].italic = (cells[c].attrs.italic != 0);
        line[c].underline = (cells[c].attrs.underline != 0);
        line[c].has_bg = (cells[c].bg.type == VTERM_COLOR_RGB || cells[c].bg.type == VTERM_COLOR_INDEXED);

        VTermColor fg = cells[c].fg;
        vterm_screen_convert_color_to_rgb(self->vts_, &fg);
        line[c].fg = Color{fg.rgb.red, fg.rgb.green, fg.rgb.blue, 255};

        if (line[c].has_bg) {
            VTermColor bg = cells[c].bg;
            vterm_screen_convert_color_to_rgb(self->vts_, &bg);
            line[c].bg = Color{bg.rgb.red, bg.rgb.green, bg.rgb.blue, 255};
        }
    }

    self->scrollback_.push_back(std::move(line));
    if (self->scrollback_.size() > self->max_scrollback_) {
        self->scrollback_.pop_front();
    }
    return 1;
}

int Terminal::cb_settermprop(VTermProp prop, VTermValue* val, void* user) {
    auto* self = static_cast<Terminal*>(user);
    if (prop == VTERM_PROP_TITLE) {
        if (val && val->string.str) {
            self->title_ = std::string(val->string.str);
            self->title_changed_ = true;
        }
    } else if (prop == VTERM_PROP_MOUSE) {
        self->mouse_mode_ = val->number;
    } else if (prop == VTERM_PROP_CURSORSHAPE) {
        self->cursor_shape_ = val->number;
    }
    return 1;
}

} // namespace vectis::vt
