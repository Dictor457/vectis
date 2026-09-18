#pragma once

#include <vterm.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <deque>

namespace vectis::vt {

struct Color {
    uint8_t r{255};
    uint8_t g{255};
    uint8_t b{255};
    uint8_t a{255};

    bool operator==(const Color&) const = default;
};

struct Cell {
    uint32_t codepoint{' '};
    Color fg{220, 225, 235, 255};
    Color bg{20, 22, 28, 255};
    bool has_bg{false};
    bool bold{false};
    bool italic{false};
    bool underline{false};
};

struct Cursor {
    int row{0};
    int col{0};
    int shape{1};
    bool visible{true};
};

class Terminal {
public:
    Terminal(int rows, int cols);
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void feed(std::string_view bytes);
    void resize(int rows, int cols);

    [[nodiscard]] Cell get_cell(int row, int col) const;
    [[nodiscard]] Cursor cursor() const noexcept;
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int line_length(int row) const;

    // Инъекция 16-цветовой палитры в ядро libvterm
    void set_palette_color(int index, uint8_t r, uint8_t g, uint8_t b);

    [[nodiscard]] bool mouse_tracking_enabled() const noexcept { return mouse_mode_ > 0; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] bool title_changed() const noexcept { return title_changed_; }
    void reset_title_changed() noexcept { title_changed_ = false; }

    void scroll_up(int lines);
    void scroll_down(int lines);
    void scroll_to_bottom() noexcept;
    [[nodiscard]] int scroll_offset() const noexcept { return scroll_offset_; }

    static int cb_damage(VTermRect rect, void* user);
    static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void* user);
    static int cb_sb_pushline(int cols, const VTermScreenCell* cells, void* user);
    static int cb_settermprop(VTermProp prop, VTermValue* val, void* user);

private:
    static const VTermScreenCallbacks s_callbacks;

    int rows_{24};
    int cols_{80};
    int scroll_offset_{0};
    size_t max_scrollback_{10000};

    int mouse_mode_{0};
    int cursor_shape_{1};
    std::string title_{"Vectis"};
    bool title_changed_{false};

    std::deque<std::vector<Cell>> scrollback_;
    VTerm* vt_{nullptr};
    VTermScreen* vts_{nullptr};
    Cursor cursor_{};
};

} // namespace vectis::vt
