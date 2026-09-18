#include "vectis/core/Pty.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <system_error>

namespace vectis::core {

Pty::Pty() = default;

Pty::~Pty() {
    if (master_fd_ >= 0) close(master_fd_);
    if (child_pid_ > 0) {
        int status;
        waitpid(child_pid_, &status, WNOHANG);
    }
}

Pty::Pty(Pty&& other) noexcept
    : master_fd_(other.master_fd_), child_pid_(other.child_pid_) {
    other.master_fd_ = -1;
    other.child_pid_ = -1;
}

Pty& Pty::operator=(Pty&& other) noexcept {
    if (this != &other) {
        if (master_fd_ >= 0) close(master_fd_);
        master_fd_ = other.master_fd_;
        child_pid_ = other.child_pid_;
        other.master_fd_ = -1;
        other.child_pid_ = -1;
    }
    return *this;
}

void Pty::spawn(std::string_view shell_path, std::string_view working_dir) {
    master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "posix_openpt failed");
    }

    if (grantpt(master_fd_) < 0 || unlockpt(master_fd_) < 0) {
        throw std::system_error(errno, std::generic_category(), "grantpt/unlockpt failed");
    }

    const char* slave_name = ptsname(master_fd_);
    if (!slave_name) {
        throw std::system_error(errno, std::generic_category(), "ptsname failed");
    }

    child_pid_ = fork();
    if (child_pid_ < 0) {
        throw std::system_error(errno, std::generic_category(), "fork failed");
    }

    if (child_pid_ == 0) {
        close(master_fd_);
        setsid();

        int slave_fd = open(slave_name, O_RDWR);
        if (slave_fd < 0) _exit(EXIT_FAILURE);

        #if defined(TIOCSCTTY)
        ioctl(slave_fd, TIOCSCTTY, 0);
        #endif

        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);

        // Управление стартовой папкой шелла
        if (!working_dir.empty() && working_dir != "inherit") {
            std::string dir(working_dir);
            if (dir == "~" || dir == "home") {
                const char* h = getenv("HOME");
                if (h) dir = h;
            }
            chdir(dir.c_str());
        }

        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        std::string path(shell_path);
        execlp(path.c_str(), path.c_str(), nullptr);
        _exit(EXIT_FAILURE);
    }

    int flags = fcntl(master_fd_, F_GETFL, 0);
    fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
}

void Pty::set_size(int rows, int cols, int pixel_width, int pixel_height) {
    if (master_fd_ < 0) return;
    winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = static_cast<unsigned short>(pixel_width);
    ws.ws_ypixel = static_cast<unsigned short>(pixel_height);
    ioctl(master_fd_, TIOCSWINSZ, &ws);
}

ssize_t Pty::read_bytes(std::span<char> buffer) {
    if (master_fd_ < 0) return -1;
    return read(master_fd_, buffer.data(), buffer.size());
}

ssize_t Pty::write_bytes(std::string_view bytes) {
    if (master_fd_ < 0) return -1;
    return write(master_fd_, bytes.data(), bytes.size());
}

bool Pty::is_alive() const noexcept {
    if (child_pid_ <= 0) return false;
    int status;
    pid_t res = waitpid(child_pid_, &status, WNOHANG);
    return res == 0;
}

} // namespace vectis::core
