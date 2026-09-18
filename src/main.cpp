#include "vectis/core/Pty.hpp"
#include "vectis/vt/Terminal.hpp"
#include "vectis/font/FontAtlas.hpp"
#include "vectis/config/Config.hpp"

#include <epoxy/gl.h>
#include <GLFW/glfw3.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <cstdio>
#include <memory>
#include <compare>
#include <algorithm>

static vectis::core::Pty* g_pty = nullptr;
static vectis::vt::Terminal* g_term = nullptr;
static std::unique_ptr<vectis::font::FontAtlas> g_atlas;
static vectis::config::Config g_cfg;
static int g_initial_font_size = 18;

static GLFWcursor* g_cursor_ibeam = nullptr;
static GLFWcursor* g_cursor_hand = nullptr;

static bool g_needs_redraw = true;
static double g_last_input_time = 0.0;

struct Point {
    int col{0};
    int row{0};

    auto operator<=>(const Point&) const = default;
};

struct Selection {
    Point start{};
    Point end{};
    bool active{false};
    bool dragging{false};

    [[nodiscard]] bool contains(int r, int c) const {
        if (!active) return false;
        auto [s, e] = (start <= end) ? std::pair{start, end} : std::pair{end, start};

        if (r < s.row || r > e.row) return false;
        if (s.row == e.row) return c >= s.col && c <= e.col;
        if (r == s.row) return c >= s.col;
        if (r == e.row) return c <= e.col;
        return true;
    }

    void clear() {
        active = false;
        dragging = false;
    }
};

static Selection g_sel;

struct HoveredUrl {
    int row{-1};
    int col_start{-1};
    int col_end{-1};
    std::string url;

    void clear() {
        row = -1;
        col_start = -1;
        col_end = -1;
        url.clear();
    }
};

static HoveredUrl g_url;

const char* vs_src = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec3 aColor;

out vec2 TexCoord;
out vec3 Color;

uniform mat4 uProj;

void main() {
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
    Color = aColor;
}
)";

const char* fs_src = R"(
#version 330 core
in vec2 TexCoord;
in vec3 Color;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform int uIsText;
uniform float uOpacity;

void main() {
    if (uIsText == 1) {
        float alpha = texture(uTexture, TexCoord).r;
        FragColor = vec4(Color, alpha);
    } else {
        FragColor = vec4(Color, uOpacity);
    }
}
)";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "[Shader Error]: " << log << '\n';
    }
    return s;
}

struct PipeCloser {
    void operator()(FILE* fp) const {
        if (fp) pclose(fp);
    }
};

std::string get_clipboard_text(GLFWwindow* window) {
    const char* clip = glfwGetClipboardString(window);
    if (clip && *clip) return std::string(clip);

    std::array<char, 256> buffer{};
    std::string result;
    std::unique_ptr<FILE, PipeCloser> pipe(popen("wl-paste --no-newline 2>/dev/null", "r"));
    if (pipe) {
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
    }
    return result;
}

void set_clipboard_text(GLFWwindow* window, const std::string& text) {
    if (text.empty()) return;
    glfwSetClipboardString(window, text.c_str());

    std::unique_ptr<FILE, PipeCloser> pipe(popen("wl-copy 2>/dev/null", "w"));
    if (pipe) {
        fwrite(text.data(), 1, text.size(), pipe.get());
    }
}

std::string get_selected_text(const vectis::vt::Terminal& term, const Selection& sel) {
    if (!sel.active) return "";
    auto [s, e] = (sel.start <= sel.end) ? std::pair{sel.start, sel.end} : std::pair{sel.end, sel.start};

    std::string result;
    for (int r = s.row; r <= e.row; ++r) {
        int len = term.line_length(r);
        if (len == 0) {
            if (r < e.row) result += '\n';
            continue;
        }

        int c_start = (r == s.row) ? s.col : 0;
        int c_end = (r == e.row) ? std::min(e.col, len - 1) : len - 1;

        if (c_start <= c_end) {
            for (int c = c_start; c <= c_end; ++c) {
                auto cell = term.get_cell(r, c);
                char32_t cp = cell.codepoint;
                if (cp == 0 || cp == ' ') {
                    result += ' ';
                } else if (cp < 0x80) {
                    result += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    result += static_cast<char>(0xC0 | (cp >> 6));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    result += static_cast<char>(0xE0 | (cp >> 12));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    result += static_cast<char>(0xF0 | (cp >> 18));
                    result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
        }
        while (!result.empty() && result.back() == ' ') result.pop_back();
        if (r < e.row) result += '\n';
    }
    return result;
}

void open_url_safely(const std::string& url) {
    if (url.empty()) return;
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        setsid();
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, WNOHANG);
    }
}

bool find_url_at(int row, int col) {
    if (!g_term || row < 0 || row >= g_term->rows() || col < 0 || col >= g_term->cols()) {
        g_url.clear();
        return false;
    }

    int len = g_term->line_length(row);
    if (col >= len) {
        g_url.clear();
        return false;
    }

    std::string line;
    line.reserve(static_cast<size_t>(len));
    for (int c = 0; c < len; ++c) {
        char32_t cp = g_term->get_cell(row, c).codepoint;
        if (cp >= 32 && cp < 127) {
            line += static_cast<char>(cp);
        } else {
            line += ' ';
        }
    }

    auto is_delim = [](char ch) {
        return ch == ' ' || ch == '\"' || ch == '\'' || ch == '`' ||
               ch == '<' || ch == '>' || ch == '(' || ch == ')' ||
               ch == '[' || ch == ']' || ch == '{' || ch == '}';
    };

    size_t c_idx = static_cast<size_t>(col);
    if (is_delim(line[c_idx])) {
        g_url.clear();
        return false;
    }

    size_t start = c_idx;
    while (start > 0 && !is_delim(line[start - 1])) {
        start--;
    }

    size_t end = c_idx;
    while (end < line.length() && !is_delim(line[end])) {
        end++;
    }

    std::string candidate = line.substr(start, end - start);

    while (!candidate.empty() && (candidate.back() == '.' || candidate.back() == ',' ||
                                 candidate.back() == ';' || candidate.back() == ':')) {
        candidate.pop_back();
        end--;
    }

    if (candidate.starts_with("http://") || candidate.starts_with("https://") ||
        candidate.starts_with("git@") || candidate.starts_with("ftp://")) {
        g_url.row = row;
        g_url.col_start = static_cast<int>(start);
        g_url.col_end = static_cast<int>(end);
        g_url.url = candidate;
        return true;
    }

    g_url.clear();
    return false;
}

void recalculate_dimensions(GLFWwindow* window) {
    if (!window || !g_atlas || !g_term || !g_pty) return;

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return;

    int usable_w = std::max(100, fb_w - g_cfg.padding_x * 2);
    int usable_h = std::max(50, fb_h - g_cfg.padding_y * 2);

    int cols = std::max(10, usable_w / g_atlas->cell_width());
    int rows = std::max(5, usable_h / g_atlas->cell_height());

    g_term->resize(rows, cols);
    g_pty->set_size(rows, cols, usable_w, usable_h);
    g_needs_redraw = true;
}

void update_font_size(int new_size, GLFWwindow* window) {
    if (new_size < 8 || new_size > 48 || !window) return;
    g_cfg.font_size = new_size;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    int scaled_font_size = static_cast<int>(g_cfg.font_size * yscale);

    g_atlas = std::make_unique<vectis::font::FontAtlas>(g_cfg.font_family, scaled_font_size);
    recalculate_dimensions(window);
    g_needs_redraw = true;
}

void send_mouse_sgr(int button, int col, int row, bool release) {
    if (!g_pty) return;
    std::string seq = "\x1b[<" + std::to_string(button) + ";" +
                      std::to_string(col + 1) + ";" +
                      std::to_string(row + 1) + (release ? "m" : "M");
    g_pty->write_bytes(seq);
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (!g_atlas || !g_term || !g_pty) return;
    g_last_input_time = glfwGetTime();

    double mx, my;
    glfwGetCursorPos(window, &mx, &my);

    float pad_x = static_cast<float>(g_cfg.padding_x);
    float pad_y = static_cast<float>(g_cfg.padding_y);
    float cw = static_cast<float>(g_atlas->cell_width());
    float ch = static_cast<float>(g_atlas->cell_height());

    int col = std::clamp(static_cast<int>((mx - pad_x) / cw), 0, g_term->cols() - 1);
    int row = std::clamp(static_cast<int>((my - pad_y) / ch), 0, g_term->rows() - 1);

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)) {
        if (find_url_at(row, col)) {
            open_url_safely(g_url.url);
            g_url.clear();
            g_needs_redraw = true;
            return;
        }
    }

    if (g_term->mouse_tracking_enabled() && !(mods & GLFW_MOD_SHIFT)) {
        int btn_code = 0;
        if (button == GLFW_MOUSE_BUTTON_LEFT) btn_code = 0;
        else if (button == GLFW_MOUSE_BUTTON_MIDDLE) btn_code = 1;
        else if (button == GLFW_MOUSE_BUTTON_RIGHT) btn_code = 2;

        send_mouse_sgr(btn_code, col, row, action == GLFW_RELEASE);
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            g_sel.start = {col, row};
            g_sel.end = {col, row};
            g_sel.active = true;
            g_sel.dragging = true;
            g_needs_redraw = true;
        } else if (action == GLFW_RELEASE) {
            g_sel.dragging = false;
            if (g_sel.start == g_sel.end) {
                g_sel.clear();
            } else {
                std::string selected = get_selected_text(*g_term, g_sel);
                if (!selected.empty()) {
                    set_clipboard_text(window, selected);
                }
            }
            g_needs_redraw = true;
        }
    }
}

void cursor_pos_callback(GLFWwindow* window, double mx, double my) {
    if (!g_atlas || !g_term) return;

    float pad_x = static_cast<float>(g_cfg.padding_x);
    float pad_y = static_cast<float>(g_cfg.padding_y);
    float cw = static_cast<float>(g_atlas->cell_width());
    float ch = static_cast<float>(g_atlas->cell_height());

    int col = std::clamp(static_cast<int>((mx - pad_x) / cw), 0, g_term->cols() - 1);
    int row = std::clamp(static_cast<int>((my - pad_y) / ch), 0, g_term->rows() - 1);

    bool ctrl_held = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                      glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);

    if (ctrl_held && find_url_at(row, col)) {
        if (g_cursor_hand) glfwSetCursor(window, g_cursor_hand);
        g_needs_redraw = true;
    } else {
        if (!ctrl_held && !g_url.url.empty()) {
            g_url.clear();
            g_needs_redraw = true;
        }
        if (g_cursor_ibeam) glfwSetCursor(window, g_cursor_ibeam);
    }

    if (g_sel.dragging) {
        g_sel.end = {col, row};
        if (g_sel.start != g_sel.end) {
            g_sel.active = true;
            g_needs_redraw = true;
        }
    }
}

void key_callback(GLFWwindow* window, int key, int, int action, int mods) {
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    if (!g_pty || !g_term) return;

    g_last_input_time = glfwGetTime();
    g_term->scroll_to_bottom();
    g_needs_redraw = true;

    if ((mods & GLFW_MOD_CONTROL) && (mods & GLFW_MOD_SHIFT)) {
        if (key == GLFW_KEY_C) {
            std::string selected = get_selected_text(*g_term, g_sel);
            if (!selected.empty()) set_clipboard_text(window, selected);
            return;
        }
        if (key == GLFW_KEY_V) {
            std::string text = get_clipboard_text(window);
            if (!text.empty()) g_pty->write_bytes(text);
            return;
        }
    }

    if (mods & GLFW_MOD_CONTROL) {
        if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
            update_font_size(g_cfg.font_size + 1, window);
            return;
        }
        if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
            update_font_size(g_cfg.font_size - 1, window);
            return;
        }
        if (key == GLFW_KEY_0 || key == GLFW_KEY_KP_0) {
            update_font_size(g_initial_font_size, window);
            return;
        }

        if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
            char ctrl = static_cast<char>(key - GLFW_KEY_A + 1);
            g_pty->write_bytes(std::string_view(&ctrl, 1));
            return;
        }
    }

    switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:  g_pty->write_bytes("\r"); g_sel.clear(); break;
        case GLFW_KEY_BACKSPACE: g_pty->write_bytes("\x7f"); g_sel.clear(); break;
        case GLFW_KEY_DELETE:    g_pty->write_bytes("\x1b[3~"); g_sel.clear(); break;
        case GLFW_KEY_TAB:       g_pty->write_bytes("\t"); break;
        case GLFW_KEY_ESCAPE:    g_pty->write_bytes("\x1b"); g_sel.clear(); break;
        case GLFW_KEY_UP:        g_pty->write_bytes("\x1b[A"); break;
        case GLFW_KEY_DOWN:      g_pty->write_bytes("\x1b[B"); break;
        case GLFW_KEY_RIGHT:     g_pty->write_bytes("\x1b[C"); break;
        case GLFW_KEY_LEFT:      g_pty->write_bytes("\x1b[D"); break;
        case GLFW_KEY_PAGE_UP:   g_term->scroll_up(g_term->rows() / 2); break;
        case GLFW_KEY_PAGE_DOWN: g_term->scroll_down(g_term->rows() / 2); break;
        case GLFW_KEY_HOME:      g_pty->write_bytes("\x1b[H"); break;
        case GLFW_KEY_END:       g_pty->write_bytes("\x1b[F"); break;
    }
}

void char_callback(GLFWwindow*, unsigned int cp) {
    if (!g_pty || !g_term) return;
    g_last_input_time = glfwGetTime();
    g_term->scroll_to_bottom();
    g_sel.clear();
    g_needs_redraw = true;

    char buf[4];
    size_t len = 0;

    if (cp < 0x80) {
        buf[0] = static_cast<char>(cp);
        len = 1;
    } else if (cp < 0x800) {
        buf[0] = static_cast<char>(0xC0 | (cp >> 6));
        buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        buf[0] = static_cast<char>(0xE0 | (cp >> 12));
        buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        buf[0] = static_cast<char>(0xF0 | (cp >> 18));
        buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 4;
    }

    g_pty->write_bytes(std::string_view(buf, len));
}

void scroll_callback(GLFWwindow* window, double, double yoffset) {
    if (!g_term || !g_atlas || !g_pty) return;
    g_needs_redraw = true;

    if (g_term->mouse_tracking_enabled()) {
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        int col = std::clamp(static_cast<int>((mx - g_cfg.padding_x) / g_atlas->cell_width()), 0, g_term->cols() - 1);
        int row = std::clamp(static_cast<int>((my - g_cfg.padding_y) / g_atlas->cell_height()), 0, g_term->rows() - 1);

        send_mouse_sgr(yoffset > 0 ? 64 : 65, col, row, false);
        return;
    }

    if (yoffset > 0) {
        g_term->scroll_up(4);
    } else if (yoffset < 0) {
        g_term->scroll_down(4);
    }
}

void framebuffer_size_callback(GLFWwindow* window, int, int) {
    recalculate_dimensions(window);
}

int main() {
    g_cfg = vectis::config::Config::load();
    g_initial_font_size = g_cfg.font_size;

    if (!glfwInit()) {
        std::cerr << "[Vectis]: GLFW init failed\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);

    #if defined(GLFW_WAYLAND_APP_ID)
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "vectis");
    #endif
    #if defined(GLFW_X11_CLASS_NAME)
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "vectis");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "vectis");
    #endif

    GLFWwindow* window = glfwCreateWindow(g_cfg.window_width, g_cfg.window_height, "Vectis", nullptr, nullptr);
    if (!window) {
        std::cerr << "[Vectis]: Window creation failed\n";
        glfwTerminate();
        return 1;
    }

    g_cursor_ibeam = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    g_cursor_hand = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);
    if (g_cursor_ibeam) glfwSetCursor(window, g_cursor_ibeam);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    int scaled_font_size = static_cast<int>(g_cfg.font_size * yscale);

    g_atlas = std::make_unique<vectis::font::FontAtlas>(g_cfg.font_family, scaled_font_size);

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0) fb_w = g_cfg.window_width;
    if (fb_h <= 0) fb_h = g_cfg.window_height;

    int usable_w = std::max(100, fb_w - g_cfg.padding_x * 2);
    int usable_h = std::max(50, fb_h - g_cfg.padding_y * 2);

    int cols = std::max(20, usable_w / g_atlas->cell_width());
    int rows = std::max(10, usable_h / g_atlas->cell_height());

    vectis::vt::Terminal term(rows, cols);
    g_term = &term;

    for (int i = 0; i < 16; ++i) {
        term.set_palette_color(i, g_cfg.colors[static_cast<size_t>(i)].r_byte(),
                                  g_cfg.colors[static_cast<size_t>(i)].g_byte(),
                                  g_cfg.colors[static_cast<size_t>(i)].b_byte());
    }

    vectis::core::Pty pty;
    pty.spawn("/bin/zsh", g_cfg.working_directory);
    pty.set_size(rows, cols, usable_w, usable_h);
    g_pty = &pty;

    glfwSetKeyCallback(window, key_callback);
    glfwSetCharCallback(window, char_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_pos_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(2 * sizeof(float)));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(4 * sizeof(float)));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLint uProj = glGetUniformLocation(prog, "uProj");
    GLint uIsText = glGetUniformLocation(prog, "uIsText");
    GLint uOpacity = glGetUniformLocation(prog, "uOpacity");

    std::array<char, 8192> read_buf{};
    bool last_blink_state = true;
    g_last_input_time = glfwGetTime();

    while (!glfwWindowShouldClose(window) && pty.is_alive()) {
        pollfd pfd{};
        pfd.fd = pty.fd();
        pfd.events = POLLIN;
        int poll_ret = poll(&pfd, 1, 7);

        if (poll_ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t bytes = pty.read_bytes(read_buf);
            if (bytes > 0) {
                term.feed(std::string_view(read_buf.data(), static_cast<size_t>(bytes)));
                g_needs_redraw = true;
            }
        }

        glfwPollEvents();

        if (term.title_changed()) {
            glfwSetWindowTitle(window, term.title().c_str());
            term.reset_title_changed();
            g_needs_redraw = true;
        }

        bool blink_visible = true;
        if (g_cfg.cursor_blink) {
            double elapsed = glfwGetTime() - g_last_input_time;
            blink_visible = (elapsed < 0.5) || (static_cast<int>(elapsed * 2.0) % 2 == 0);
            if (blink_visible != last_blink_state) {
                last_blink_state = blink_visible;
                g_needs_redraw = true;
            }
        }

        if (!g_needs_redraw) {
            continue;
        }
        g_needs_redraw = false;

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w <= 0 || fb_h <= 0) continue;

        glViewport(0, 0, fb_w, fb_h);
        glClearColor(g_cfg.background.r, g_cfg.background.g, g_cfg.background.b, g_cfg.opacity);
        glClear(GL_COLOR_BUFFER_BIT);

        float proj[16] = {
            2.0f / fb_w,  0.0f,         0.0f, 0.0f,
            0.0f,        -2.0f / fb_h,  0.0f, 0.0f,
            0.0f,         0.0f,        -1.0f, 0.0f,
           -1.0f,         1.0f,         0.0f, 1.0f
        };

        glUseProgram(prog);
        glUniformMatrix4fv(uProj, 1, GL_FALSE, proj);

        std::vector<float> bg_verts;
        std::vector<float> line_verts;
        std::vector<float> sel_verts;
        std::vector<float> text_verts;

        bg_verts.reserve(term.rows() * term.cols() * 42);
        line_verts.reserve(term.rows() * term.cols() * 42);
        sel_verts.reserve(term.rows() * term.cols() * 42);
        text_verts.reserve(term.rows() * term.cols() * 42);

        float pad_x = static_cast<float>(g_cfg.padding_x);
        float pad_y = static_cast<float>(g_cfg.padding_y);
        float cw = static_cast<float>(g_atlas->cell_width());
        float ch = static_cast<float>(g_atlas->cell_height());

        for (int r = 0; r < term.rows(); ++r) {
            int line_len = term.line_length(r);

            for (int c = 0; c < term.cols(); ++c) {
                auto cell = term.get_cell(r, c);

                float cell_x0 = pad_x + c * cw;
                float cell_y0 = pad_y + r * ch;
                float cell_x1 = cell_x0 + cw;
                float cell_y1 = cell_y0 + ch;

                if (cell.has_bg) {
                    float br = cell.bg.r / 255.0f;
                    float bg = cell.bg.g / 255.0f;
                    float bb = cell.bg.b / 255.0f;

                    float bg_quad[] = {
                        cell_x0, cell_y0, 0.0f, 0.0f, br, bg, bb,
                        cell_x1, cell_y0, 0.0f, 0.0f, br, bg, bb,
                        cell_x1, cell_y1, 0.0f, 0.0f, br, bg, bb,

                        cell_x0, cell_y0, 0.0f, 0.0f, br, bg, bb,
                        cell_x1, cell_y1, 0.0f, 0.0f, br, bg, bb,
                        cell_x0, cell_y1, 0.0f, 0.0f, br, bg, bb
                    };
                    bg_verts.insert(bg_verts.end(), std::begin(bg_quad), std::end(bg_quad));
                }

                if (g_sel.contains(r, c) && c < line_len) {
                    float sr = 0.22f, sg = 0.44f, sb = 0.88f;
                    float sel_quad[] = {
                        cell_x0, cell_y0, 0.0f, 0.0f, sr, sg, sb,
                        cell_x1, cell_y0, 0.0f, 0.0f, sr, sg, sb,
                        cell_x1, cell_y1, 0.0f, 0.0f, sr, sg, sb,

                        cell_x0, cell_y0, 0.0f, 0.0f, sr, sg, sb,
                        cell_x1, cell_y1, 0.0f, 0.0f, sr, sg, sb,
                        cell_x0, cell_y1, 0.0f, 0.0f, sr, sg, sb
                    };
                    sel_verts.insert(sel_verts.end(), std::begin(sel_quad), std::end(sel_quad));
                }

                bool is_url_hover = (r == g_url.row && c >= g_url.col_start && c < g_url.col_end);
                if (cell.underline || is_url_hover) {
                    float ur = is_url_hover ? 0.35f : (cell.fg.r / 255.0f);
                    float ug = is_url_hover ? 0.65f : (cell.fg.g / 255.0f);
                    float ub = is_url_hover ? 0.98f : (cell.fg.b / 255.0f);

                    float uy0 = cell_y1 - 1.8f;
                    float uy1 = cell_y1;

                    float line_quad[] = {
                        cell_x0, uy0, 0.0f, 0.0f, ur, ug, ub,
                        cell_x1, uy0, 0.0f, 0.0f, ur, ug, ub,
                        cell_x1, uy1, 0.0f, 0.0f, ur, ug, ub,

                        cell_x0, uy0, 0.0f, 0.0f, ur, ug, ub,
                        cell_x1, uy1, 0.0f, 0.0f, ur, ug, ub,
                        cell_x0, uy1, 0.0f, 0.0f, ur, ug, ub
                    };
                    line_verts.insert(line_verts.end(), std::begin(line_quad), std::end(line_quad));
                }

                if (cell.codepoint <= 32) continue;

                // Выбор гарнитуры на лету (Regular, Bold, Italic, Bold Italic)
                vectis::font::FontStyle style = vectis::font::REGULAR;
                if (cell.bold && cell.italic) style = vectis::font::BOLD_ITALIC;
                else if (cell.bold) style = vectis::font::BOLD;
                else if (cell.italic) style = vectis::font::ITALIC;

                if (!g_atlas->has_glyph(cell.codepoint, style)) continue;

                const auto& g = g_atlas->glyph(cell.codepoint, style);
                float gx0 = cell_x0 + g.bearing_x;
                float gy0 = cell_y0 + (ch - g.bearing_y);
                float gx1 = gx0 + g.width;
                float gy1 = gy0 + g.height;
                float u0 = g.uv_x0, v0 = g.uv_y0;
                float u1 = g.uv_x1, v1 = g.uv_y1;

                float cr = is_url_hover ? 0.35f : (cell.fg.r / 255.0f);
                float cg = is_url_hover ? 0.65f : (cell.fg.g / 255.0f);
                float cb = is_url_hover ? 0.98f : (cell.fg.b / 255.0f);

                float text_quad[] = {
                    gx0, gy0, u0, v0, cr, cg, cb,
                    gx1, gy0, u1, v0, cr, cg, cb,
                    gx1, gy1, u1, v1, cr, cg, cb,

                    gx0, gy0, u0, v0, cr, cg, cb,
                    gx1, gy1, u1, v1, cr, cg, cb,
                    gx0, gy1, u0, v1, cr, cg, cb
                };
                text_verts.insert(text_verts.end(), std::begin(text_quad), std::end(text_quad));
            }
        }

        if (!bg_verts.empty()) {
            glUniform1i(uIsText, 0);
            glUniform1f(uOpacity, g_cfg.opacity);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, bg_verts.size() * sizeof(float), bg_verts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, bg_verts.size() / 7);
        }

        if (!sel_verts.empty()) {
            glUniform1i(uIsText, 0);
            glUniform1f(uOpacity, 0.45f);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sel_verts.size() * sizeof(float), sel_verts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, sel_verts.size() / 7);
        }

        if (!line_verts.empty()) {
            glUniform1i(uIsText, 0);
            glUniform1f(uOpacity, 1.0f);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, line_verts.size() * sizeof(float), line_verts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, line_verts.size() / 7);
        }

        auto cur = term.cursor();
        if (cur.visible && blink_visible) {
            float cur_x0 = pad_x + cur.col * cw;
            float cur_y0 = pad_y + cur.row * ch;
            float cur_x1 = cur_x0 + cw;
            float cur_y1 = cur_y0 + ch;

            bool is_beam = (g_cfg.cursor_shape == "beam" && cur.shape == 1) || (cur.shape == 3);
            bool is_underline = (g_cfg.cursor_shape == "underline" && cur.shape == 1) || (cur.shape == 2);

            if (is_beam) {
                cur_x1 = cur_x0 + 2.0f;
            } else if (is_underline) {
                cur_y0 = cur_y1 - 2.5f;
            }

            float cursor_quad[] = {
                cur_x0, cur_y0, 0.0f, 0.0f, g_cfg.cursor.r, g_cfg.cursor.g, g_cfg.cursor.b,
                cur_x1, cur_y0, 0.0f, 0.0f, g_cfg.cursor.r, g_cfg.cursor.g, g_cfg.cursor.b,
                cur_x1, cur_y1, 0.0f, 0.0f, g_cfg.cursor.r, g_cfg.cursor.g, g_cfg.cursor.b,

                cur_x0, cur_y0, 0.0f, 0.0f, g_cfg.cursor.r, g_cfg.cursor.g, g_cfg.cursor.b,
                cur_x1, cur_y1, 0.0f, 0.0f, g_cfg.cursor.r, g_cfg.cursor.g, g_cfg.cursor.b,
                cur_x0, cur_y1, 0.0f, 0.0f, g_cfg.cursor.r, g_cfg.cursor.g, g_cfg.cursor.b
            };

            glUniform1i(uIsText, 0);
            glUniform1f(uOpacity, 1.0f);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(cursor_quad), cursor_quad, GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        if (!text_verts.empty()) {
            glUniform1i(uIsText, 1);
            glBindTexture(GL_TEXTURE_2D, g_atlas->texture_id());
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, text_verts.size() * sizeof(float), text_verts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, text_verts.size() / 7);
        }

        glfwSwapBuffers(window);
    }

    if (g_cursor_ibeam) glfwDestroyCursor(g_cursor_ibeam);
    if (g_cursor_hand) glfwDestroyCursor(g_cursor_hand);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
