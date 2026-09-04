#include "mr/terminal.hpp"

#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mr {

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

const char* to_string(Key k) noexcept {
    switch (k) {
        case Key::None:      return "none";
        case Key::Space:     return "space";
        case Key::Enter:     return "enter";
        case Key::Escape:    return "esc";
        case Key::Tab:       return "tab";
        case Key::Backspace: return "backspace";
        case Key::Left:      return "left";
        case Key::Right:     return "right";
        case Key::Up:        return "up";
        case Key::Down:      return "down";
        case Key::Home:      return "home";
        case Key::End:       return "end";
        case Key::PageUp:    return "pgup";
        case Key::PageDown:  return "pgdn";
        case Key::Count:     return "?";
    }
    // 可打印 ASCII 落到这里。返回一个静态缓冲区，够日志用。
    static char one[2] = {0, 0};
    const auto code = static_cast<uint16_t>(k);
    if (code >= 33u && code <= 126u) {
        one[0] = static_cast<char>(code);
        return one;
    }
    return "?";
}

void Input::begin_frame(uint64_t now_ns) noexcept {
    now_ns_ = now_ns;
    // 只清边沿，不清 last_event_ns_ —— down() 靠后者做窗口判定
    std::memset(pressed_, 0, sizeof(pressed_));
}

void Input::feed(Key k, uint64_t now_ns) noexcept {
    if (! in_range(k) || k == Key::None) {
        return;
    }
    const size_t slot = static_cast<size_t>(k);
    pressed_[slot] = true;
    last_event_ns_[slot] = now_ns;
}

bool Input::pressed(Key k) const noexcept {
    return in_range(k) && pressed_[static_cast<size_t>(k)];
}

bool Input::down(Key k) const noexcept {
    if (! in_range(k)) {
        return false;
    }
    const uint64_t last = last_event_ns_[static_cast<size_t>(k)];
    if (last == 0) {
        return false;
    }
    return now_ns_ >= last && (now_ns_ - last) <= kHoldWindowMs * 1000000ULL;
}

int Input::axis_x() const noexcept {
    const bool left = down(Key::Left) || down(key('a'));
    const bool right = down(Key::Right) || down(key('d'));
    return (right ? 1 : 0) - (left ? 1 : 0);
}

int Input::axis_y() const noexcept {
    // 向上为正。屏幕坐标 y 向下增长，但"轴"是给逻辑用的，
    // 让 W 是 +1 比让它是 -1 少一个心智转换。
    const bool up = down(Key::Up) || down(key('w'));
    const bool downward = down(Key::Down) || down(key('s'));
    return (up ? 1 : 0) - (downward ? 1 : 0);
}

int Input::digit_pressed() const noexcept {
    for (int d = 0; d <= 9; ++d) {
        if (pressed(key(static_cast<char>('0' + d)))) {
            return d;
        }
    }
    return -1;
}

bool Input::quit_requested() const noexcept {
    return pressed(key('q')) || pressed(Key::Escape);
}

bool Input::any_pressed() const noexcept {
    for (size_t i = 0; i < kSlots; ++i) {
        if (pressed_[i]) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Terminal
// ---------------------------------------------------------------------------

namespace {

// 文件作用域，好让 restore_all() 不依赖对象还活着 —— 见头文件"恢复"一节。
termios g_saved{};
bool g_saved_valid = false;

Key csi_final_to_key(char final_byte) noexcept {
    switch (final_byte) {
        case 'A': return Key::Up;
        case 'B': return Key::Down;
        case 'C': return Key::Right;
        case 'D': return Key::Left;
        case 'H': return Key::Home;
        case 'F': return Key::End;
        default:  return Key::None;
    }
}

} // namespace

namespace detail {

DecodeResult decode_key(const char* buf, size_t len) noexcept {
    if (len == 0) {
        return DecodeResult{};
    }

    const auto first = static_cast<unsigned char>(buf[0]);

    if (first == 0x1bu) {
        if (len == 1) {
            // 单独一个 ESC，还是一个序列的开头？分不出来。
            // 交给调用方：缓冲区还有别的数据说明后面就是序列；
            // 这是本次读到的最后一个字节则当作 Escape 键。
            return DecodeResult{Key::Escape, 1, true};
        }
        if (buf[1] != '[' && buf[1] != 'O') {
            // ESC 后面跟别的（Alt+键）。本项目不用，整段丢掉。
            return DecodeResult{Key::None, 2, false};
        }
        // CSI: ESC [ [参数] 终止字节
        size_t i = 2;
        while (i < len && (buf[i] == ';' || (buf[i] >= '0' && buf[i] <= '9'))) {
            ++i;
        }
        if (i >= len) {
            return DecodeResult{Key::None, 0, true};  // 序列没读完
        }

        const char final_byte = buf[i];
        if (final_byte == '~') {
            // ESC [ n ~ 形式
            Key k = Key::None;
            if (i == 3 && buf[2] == '5') {
                k = Key::PageUp;
            } else if (i == 3 && buf[2] == '6') {
                k = Key::PageDown;
            }
            return DecodeResult{k, i + 1, false};
        }
        return DecodeResult{csi_final_to_key(final_byte), i + 1, false};
    }

    if (first == '\r' || first == '\n') {
        return DecodeResult{Key::Enter, 1, false};
    }
    if (first == '\t') {
        return DecodeResult{Key::Tab, 1, false};
    }
    if (first == 0x7fu || first == 0x08u) {
        return DecodeResult{Key::Backspace, 1, false};
    }
    if (first >= 32u && first <= 126u) {
        return DecodeResult{static_cast<Key>(first), 1, false};
    }

    // 别的控制字符（Ctrl+字母等）本项目不用
    return DecodeResult{Key::None, 1, false};
}

} // namespace detail

Terminal::Terminal(bool enabled) noexcept {
    if (! enabled) {
        return;
    }
    if (::isatty(STDIN_FILENO) != 1) {
        return;  // 管道 / 重定向 / 无人值守，不动终端
    }

    termios current{};
    if (::tcgetattr(STDIN_FILENO, &current) != 0) {
        return;
    }
    g_saved = current;
    g_saved_valid = true;

    // **只关这两个。** 其余标志位一律保留，理由见头文件。
    current.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));

    // 不阻塞：有多少读多少，没有就立刻返回。
    // 帧循环不能停在 read 上等人按键。
    current.c_cc[VMIN] = 0;
    current.c_cc[VTIME] = 0;

    if (::tcsetattr(STDIN_FILENO, TCSANOW, &current) != 0) {
        g_saved_valid = false;
        return;
    }
    interactive_ = true;
}

Terminal::~Terminal() {
    restore_all();
}

void Terminal::restore_all() noexcept {
    if (! g_saved_valid) {
        return;
    }
    g_saved_valid = false;  // 先清标志，保证幂等（信号可能重入）
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
}

void Terminal::poll(Input& input, uint64_t now_ns) noexcept {
    // 无条件清边沿：不调的话 pressed() 会一直停在上一帧的值上。
    input.begin_frame(now_ns);

    if (! interactive_) {
        return;
    }

    char buf[256];
    // 先把上一帧没解完的残留放到前面
    size_t len = pending_len_;
    if (len > 0) {
        std::memcpy(buf, pending_, len);
    }
    pending_len_ = 0;

    for (;;) {
        const ssize_t got = ::read(STDIN_FILENO, buf + len, sizeof(buf) - len);
        if (got <= 0) {
            break;
        }
        len += static_cast<size_t>(got);
        if (len >= sizeof(buf)) {
            break;  // 缓冲区满了，这一批先处理掉
        }
    }

    size_t offset = 0;
    while (offset < len) {
        const detail::DecodeResult decoded = detail::decode_key(buf + offset, len - offset);

        if (decoded.need_more) {
            const size_t remaining = len - offset;
            const bool is_lone_escape = (decoded.key == Key::Escape && remaining == 1);
            if (is_lone_escape) {
                // 本次读到的最后一个字节就是 ESC —— 当作 Escape 键。
                // 序列被切成两半的话这里会误判一次，代价是多一次 Escape，
                // 比"按 Esc 没反应"好。
                input.feed(Key::Escape, now_ns);
                offset += 1;
                continue;
            }
            // 序列没读完，留到下一帧
            if (remaining <= sizeof(pending_)) {
                std::memcpy(pending_, buf + offset, remaining);
                pending_len_ = remaining;
            }
            return;
        }

        if (decoded.consumed == 0) {
            break;  // 解不出来也不消耗字节，防死循环
        }
        input.feed(decoded.key, now_ns);
        offset += decoded.consumed;
    }
}

std::string Terminal::describe() const {
    if (! interactive_) {
        return "input: off (stdin is not a tty, or --no-input was given)";
    }
    return "input: reading the controlling terminal (ICANON/ECHO off, ISIG and OPOST kept)";
}

} // namespace mr