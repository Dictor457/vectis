#pragma once

#include <sys/types.h>
#include <string>
#include <string_view>
#include <span>

namespace vectis::core {

class Pty {
public:
    Pty();
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    Pty(Pty&& other) noexcept;
    Pty& operator=(Pty&& other) noexcept;

    void spawn(std::string_view shell_path = "/bin/zsh", std::string_view working_dir = "inherit");
    void set_size(int rows, int cols, int pixel_width = 0, int pixel_height = 0);

    ssize_t read_bytes(std::span<char> buffer);
    ssize_t write_bytes(std::string_view bytes);

    [[nodiscard]] int fd() const noexcept { return master_fd_; }
    [[nodiscard]] pid_t child_pid() const noexcept { return child_pid_; }
    [[nodiscard]] bool is_alive() const noexcept;

private:
    int master_fd_{-1};
    pid_t child_pid_{-1};
};

} // namespace vectis::core
