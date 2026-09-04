/**
 * @file tests/test_input.cpp
 * @brief 终端按键解码与 Input 状态语义的检查
 *
 * 这一块的失败形态和光栅化不一样：解码器认错了不会崩，只会表现为
 * "按一次方向键触发了三次动作"或者"偶尔按了没反应"。
 *
 * 而这个项目的观察条件让这两种症状特别难查 —— 画面在板子的屏幕上，
 * 人在另一台机器的 ssh 终端里，中间隔着一次转头。所以宁可在这里多写几条。
 */
#include <cstdio>
#include <iostream>
#include <string>

#include <mw/internal/color.hpp>

#include "mr/terminal.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (! condition) {
        ++g_failures;
        std::printf("  FAIL %s\n", what.c_str());
    }
}

mr::detail::DecodeResult decode(const std::string& bytes) {
    return mr::detail::decode_key(bytes.data(), bytes.size());
}

void expect_key(const std::string& bytes, mr::Key expected, size_t consumed,
                const std::string& what) {
    const mr::detail::DecodeResult got = decode(bytes);
    const bool ok = got.key == expected && got.consumed == consumed && ! got.need_more;
    check(ok, what + " (got key=" + mr::to_string(got.key) +
                  " consumed=" + std::to_string(got.consumed) + ")");
}

// ---------------------------------------------------------------------------

void test_plain_keys() {
    expect_key("a", mr::key('a'), 1, "plain 'a'");
    expect_key("Z", mr::key('Z'), 1, "plain 'Z'");
    expect_key("7", mr::key('7'), 1, "digit '7'");
    expect_key(" ", mr::Key::Space, 1, "space");
    expect_key("\r", mr::Key::Enter, 1, "CR is Enter");
    expect_key("\n", mr::Key::Enter, 1, "LF is Enter");
    expect_key("\t", mr::Key::Tab, 1, "tab");
    expect_key("\x7f", mr::Key::Backspace, 1, "DEL is Backspace");
}

void test_arrow_sequences() {
    expect_key("\x1b[A", mr::Key::Up, 3, "CSI A is Up");
    expect_key("\x1b[B", mr::Key::Down, 3, "CSI B is Down");
    expect_key("\x1b[C", mr::Key::Right, 3, "CSI C is Right");
    expect_key("\x1b[D", mr::Key::Left, 3, "CSI D is Left");
    expect_key("\x1b[H", mr::Key::Home, 3, "CSI H is Home");
    expect_key("\x1b[F", mr::Key::End, 3, "CSI F is End");
    expect_key("\x1b[5~", mr::Key::PageUp, 4, "CSI 5~ is PageUp");
    expect_key("\x1b[6~", mr::Key::PageDown, 4, "CSI 6~ is PageDown");

    // SS3 形式（应用光标键模式）。有些终端在某些模式下发这个。
    expect_key("\x1bOA", mr::Key::Up, 3, "SS3 A is Up");

    // 带参数的形式（Ctrl+方向键等）。识别不出修饰键，但**必须整段消耗掉** ——
    // 否则剩下的字节会被当成一串普通按键，那就是"按一次触发三次"的来源。
    const mr::detail::DecodeResult modified = decode("\x1b[1;5C");
    check(modified.consumed == 6, "a modified arrow sequence is consumed whole");
}

void test_partial_sequences() {
    const mr::detail::DecodeResult half = decode("\x1b[");
    check(half.need_more, "a truncated CSI reports need_more");

    const mr::detail::DecodeResult partial_param = decode("\x1b[1;");
    check(partial_param.need_more, "a CSI cut inside its parameters reports need_more");

    // 单独一个 ESC：分不出是 Escape 键还是序列开头，交给调用方判断
    const mr::detail::DecodeResult lone = decode("\x1b");
    check(lone.key == mr::Key::Escape && lone.need_more,
          "a lone ESC is reported as Escape but flagged need_more");
}

void test_stream_consumption() {
    const std::string stream = "ab\x1b[Cq";
    size_t offset = 0;
    int decoded_count = 0;
    mr::Key last = mr::Key::None;

    while (offset < stream.size() && decoded_count < 16) {
        const mr::detail::DecodeResult got =
            mr::detail::decode_key(stream.data() + offset, stream.size() - offset);
        if (got.need_more || got.consumed == 0) {
            break;
        }
        offset += got.consumed;
        last = got.key;
        ++decoded_count;
    }
    check(offset == stream.size(), "a mixed stream is consumed exactly");
    check(decoded_count == 4, "a mixed stream yields four keys");
    check(last == mr::key('q'), "the last key of the mixed stream is 'q'");
}

void test_never_stalls() {
    // 任何输入都必须要么消耗字节、要么报 need_more。
    // 两者皆非的话 poll() 会死循环 —— 而那是一个挂死的帧循环。
    for (int byte = 0; byte < 256; ++byte) {
        const char one = static_cast<char>(byte);
        const mr::detail::DecodeResult got = mr::detail::decode_key(&one, 1);
        if (got.consumed == 0 && ! got.need_more) {
            check(false, "byte " + std::to_string(byte) + " neither consumed nor needed more");
            return;
        }
    }
    ++g_checks;
}

void test_input_state() {
    mr::Input in;
    const uint64_t t0 = 1000000000ULL;

    in.begin_frame(t0);
    check(! in.pressed(mr::key('a')), "a fresh Input reports nothing pressed");
    check(! in.down(mr::key('a')), "a fresh Input reports nothing held");
    check(in.digit_pressed() == -1, "a fresh Input reports no digit");
    check(! in.quit_requested(), "a fresh Input does not request quit");
    check(in.axis_x() == 0 && in.axis_y() == 0, "a fresh Input has zero axes");

    in.feed(mr::key('a'), t0);
    check(in.pressed(mr::key('a')), "feed() marks the key pressed this frame");
    check(in.down(mr::key('a')), "feed() also marks it held");

    in.begin_frame(t0 + 50000000ULL);
    check(! in.pressed(mr::key('a')), "pressed() is an edge and clears on the next frame");
    check(in.down(mr::key('a')), "down() stays true inside the hold window");

    in.begin_frame(t0 + 500000000ULL);
    check(! in.down(mr::key('a')), "down() goes false past the hold window");
}

void test_axes_and_shortcuts() {
    const uint64_t t0 = 1000000000ULL;
    {
        mr::Input in;
        in.begin_frame(t0);
        in.feed(mr::key('d'), t0);
        check(in.axis_x() == 1, "d gives axis_x = +1");
        in.feed(mr::key('a'), t0);
        check(in.axis_x() == 0, "a and d together cancel out");
    }
    {
        mr::Input in;
        in.begin_frame(t0);
        in.feed(mr::Key::Up, t0);
        check(in.axis_y() == 1, "Up gives axis_y = +1 (up is positive)");
        check(in.axis_x() == 0, "Up does not disturb axis_x");
    }
    {
        mr::Input in;
        in.begin_frame(t0);
        in.feed(mr::key('7'), t0);
        check(in.digit_pressed() == 7, "digit_pressed() finds 7");
    }
    {
        mr::Input in;
        in.begin_frame(t0);
        in.feed(mr::Key::Escape, t0);
        check(in.quit_requested(), "Escape requests quit");
    }
    {
        mr::Input in;
        in.begin_frame(t0);
        in.feed(mr::key('q'), t0);
        check(in.quit_requested(), "q requests quit");
    }
}

void test_non_tty_is_silent() {
    // 单测里 stdin 通常不是 tty。这时 Terminal 必须不碰终端属性，
    // 但仍然要清边沿 —— 否则 pressed() 会卡在上一帧的值上。
    mr::Terminal terminal(true);
    check(! terminal.interactive(), "a non-tty stdin is not treated as interactive");

    mr::Input in;
    in.feed(mr::key('z'), 1000000000ULL);
    check(in.pressed(mr::key('z')), "feed() works regardless of tty-ness");

    terminal.poll(in, 1016000000ULL);
    check(! in.pressed(mr::key('z')), "poll() clears the edge even without a tty");
}

} // namespace

int main() {
    std::printf("mini-render input tests\n");

    test_plain_keys();
    test_arrow_sequences();
    test_partial_sequences();
    test_stream_consumption();
    test_never_stalls();
    test_input_state();
    test_axes_and_shortcuts();
    test_non_tty_is_silent();

    if (g_failures == 0) {
        std::cout << "  " << internal::color::green("PASS") << "  " << g_checks << " checks\n";
        return 0;
    }
    std::cout << "  " << internal::color::red("FAIL") << "  " << g_failures << " of " << g_checks
              << " checks\n";
    return 1;
}