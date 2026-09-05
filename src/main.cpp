/**
 * @file src/main.cpp
 * @brief 全项目唯一的 `main()`
 *
 * 课只提供一个 render 函数（见 `mr/lesson.hpp`），样板全在这里：
 * 参数解析、信号、环境变量、开显示、帧循环、落盘、统计。
 *
 * ## 这里是唯一认识 `mw::` 的地方
 *
 * `#include <mw/...>` 在本项目里只允许出现在这个文件。
 * `make lint` 会检查。理由见 `mr/surface.hpp` 开头。
 *
 * ## 用现成的，不要手写
 *
 * 参数解析用 `internal::parse_args`，信号用 `internal::sig::guard`，
 * 环境变量用 `internal::env`，终端着色用 `internal::color` ——
 * 全部来自 mini-wayland 的 `mw/internal/`。
 *
 * 这四个东西在 mini-wayland 里躺了很久没人用，原因不是纪律问题：
 * 它们**编译不过**那边的告警集合，谁想用谁先撞一堵 `-Werror` 的墙，
 * 然后退回去手写。那边已经修好了（见其 `docs/internal-lib.md`），
 * 这边则用 `make lint` 把"手写"这条路直接堵死。
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include <mw/core/log.hpp>
#include <mw/display/screen.hpp>
#include <mw/internal/color.hpp>
#include <mw/internal/env.hpp>
#include <mw/internal/parse_args.hpp>
#include <mw/internal/signal.hpp>
#include <mw/version.hpp>

#include "mr/input.hpp"
#include "mr/lesson.hpp"
#include "mr/surface.hpp"
#include "mr/terminal.hpp"

namespace {

volatile sig_atomic_t g_should_stop = 0;

void on_signal(int /*signum*/) {
    g_should_stop = 1;
}

/// 崩溃路径。析构不会跑，所以在这里先把终端恢复回去 ——
/// 否则 ssh 会话变成没有回显、没有行编辑的状态，
/// 而那一刻你看不见自己在打什么（盲敲 `reset` 回车能修回来）。
/// `MR_BUG` 在 debug 下会 abort，开发期撞上的概率不低。
void on_fatal(int signum) {
    mr::Terminal::restore_all();
    // 恢复默认处置后重新发一次，保留 core dump 与原始退出码
    ::signal(signum, SIG_DFL);
    ::raise(signum);
}

// ---------------------------------------------------------------------------
// 环境变量：开发期的旋钮，不进命令行
// ---------------------------------------------------------------------------
// 刻意和 CLI 分开。这里放的是"改一次、之后一直生效"的东西，
// 混进 CLI 只会让 --help 越来越长，而每次都要重打一遍。

struct Env {
    std::string dump_dir{};
    int seed = 0;
    bool no_color = false;

    ENV_SCHEMA(
        ENV_FIELD(dump_dir, "MR_DUMP_DIR"), 
        ENV_FIELD(seed,     "MR_SEED"),
        ENV_FIELD(no_color, "MR_NO_COLOR")
    )
};

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------

struct Options {
    std::string command = "list";
    bool no_input = false;

    /// `std::optional` 而不是 `std::string` —— parse_args 把非 optional 的
    /// positional 当作**必填**。写成 std::string 的话 `mini-render list`
    /// 会因为缺 lesson 而报错，而 list 本来就不需要 lesson。
    std::optional<std::string> lesson{};
    std::string backend = "offscreen";
    std::string size = "1280x720";
    std::string device{};
    uint64_t frames = 0;
    bool no_pace = false;
    bool help = false;
};

// ---------------------------------------------------------------------------

bool parse_size(const std::string& text, mw::drm::Size& out) {
    unsigned width = 0;
    unsigned height = 0;
    if (std::sscanf(text.c_str(), "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
        return false;
    }
    out = mw::drm::Size{width, height};
    return true;
}

int do_list() {
    const mr::LessonList all = mr::lessons();
    if (all.count == 0) {
        std::cout << "no lessons are linked into this binary\n";
        return 1;
    }

    std::cout << internal::color::bold("lessons") << " (" << all.count << ")\n\n";
    for (const mr::Lesson* lesson : all) {
        std::cout << "  " << internal::color::cyan(lesson->id) << "  "
                  << internal::color::dim(lesson->year) << "  " << lesson->title << "  "
                  << internal::color::dim(mr::to_string(lesson->cadence)) << "\n";
    }
    std::cout << "\n  run one with: mini-render run <id> [--backend kms]\n";
    return 0;
}

/// 把 Frame 变成 Surface。**全项目唯一一处**类型转换的接缝。
mr::Surface surface_from(const mw::display::Frame& frame) noexcept {
    return mr::Surface(frame.pixels.data(), frame.size.width, frame.size.height, frame.stride);
}

int do_run(const Options& options, const Env& env) {
    const mr::Lesson* lesson = mr::find_lesson(*options.lesson);
    if (lesson == nullptr) {
        std::cerr << internal::color::red("error") << ": no lesson matches '" << *options.lesson
                  << "' (ambiguous prefixes are rejected on purpose)\n";
        return do_list() == 0 ? 2 : 2;
    }

    mw::display::ScreenConfig config;
    config.device_path = options.device;
    config.offscreen_pace = ! options.no_pace;
    config.dump_dir = env.dump_dir;
    config.dump_every = env.dump_dir.empty() ? 0u : 1u;

    if (options.backend == "kms") {
        config.backend = mw::display::Backend::Kms;
    } else if (options.backend == "offscreen") {
        config.backend = mw::display::Backend::Offscreen;
    } else {
        std::cerr << internal::color::red("error") << ": unknown backend '" << options.backend
                  << "'\n";
        return 2;
    }
    if (! parse_size(options.size, config.offscreen_size)) {
        std::cerr << internal::color::red("error") << ": bad size '" << options.size
                  << "', expected WxH\n";
        return 2;
    }

    auto opened = mw::display::Screen::open(config);
    if (! opened) {
        mw::log_error_object(opened.error(), "Screen::open");
        return 1;
    }
    mw::display::Screen screen = std::move(opened).value();

    LOG_INFO("{}", screen.to_string());
    LOG_INFO("lesson {} ({}) -- {}", lesson->id, lesson->year, lesson->title);

    // Static 的课画一次就定了，但仍然每帧调用：双缓冲下"只画了一块 buffer"
    // 是个很常见的 bug，每帧重画让这个可能性整个消失。开销可以忽略。
    // 终端在 Screen 之后构造、之前析构。顺序无关紧要（两者不相干），
    // 但放在这里是为了让 open 失败时根本不碰终端。
    mr::Terminal terminal(! options.no_input);
    mr::Input input;
    LOG_INFO("{}", terminal.describe());
    if (terminal.interactive()) {
        LOG_INFO("keys: q/Esc quit, and whatever this lesson binds");
    }

    const uint64_t limit = options.frames;
    const auto started = std::chrono::steady_clock::now();
    double previous_s = 0.0;

    for (uint64_t i = 0; g_should_stop == 0 && (limit == 0 || i < limit); ++i) {
        const double now_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto now_ns = static_cast<uint64_t>(now_s * 1e9);

        terminal.poll(input, now_ns);
        if (input.quit_requested()) {
            LOG_INFO("quit requested from the terminal");
            break;
        }

        auto frame = screen.begin_frame();
        if (! frame) {
            mw::log_error_object(frame.error(), "begin_frame");
            return 1;
        }

        mr::LessonParams params;
        params.frame = i;
        params.time_s = now_s;
        // 第一帧 dt 为 0：那一帧没有"上一帧"，给一个猜出来的值
        // （比如标称帧长）会让所有靠 dt 积分的东西在第一帧跳一下。
        params.dt_s = (i == 0) ? 0.0 : (now_s - previous_s);
        params.seed = static_cast<uint32_t>(env.seed);
        params.input_ptr = &input;
        previous_s = now_s;

        lesson->render(surface_from(frame.value()), params);

        if (auto status = screen.present(); ! status) {
            mw::log_error_object(status.error(), "present");
            return 1;
        }
    }

    LOG_INFO("{}", screen.stats().to_line());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto signals = internal::sig::bind({
        {SIGINT, on_signal},
        {SIGTERM, on_signal},
        {SIGSEGV, on_fatal},
        {SIGABRT, on_fatal},
    });

    if (! mw::check_abi()) {
        std::cerr << "error: mini-wayland header/library version mismatch\n";
        return 2;
    }

    const std::optional<Env> env = internal::env::read<Env>();
    if (! env.has_value()) {
        std::cerr << internal::color::red("error")
                  << ": bad value in one of MR_DUMP_DIR / MR_SEED / MR_NO_COLOR\n";
        return 2;
    }

    auto parser = internal::parse_args::parser<Options>(
                      "mini-render -- classic graphics algorithms, one lesson at a time")
                      .bind(&Options::command, "command", "list | run")
                      .bind(&Options::lesson, "lesson", "lesson id or unique prefix")
                      .bind(&Options::backend, "-b", "--backend", "kms | offscreen")
                      .bind(&Options::size, "-s", "--size", "offscreen size, WxH")
                      .bind(&Options::device, "-D", "--device", "KMS device node; empty = auto")
                      .bind(&Options::frames, "-f", "--frames", "frame count, 0 = until Ctrl+C")
                      .bind(&Options::no_pace, "--no-pace", "offscreen: do not sleep between frames")
                      .bind(&Options::no_input, "--no-input", "do not put the terminal in raw mode")
                      .bind(&Options::help, "-h", "--help", "this text")
                      .example("mini-render list", "what is available")
                      .example("mini-render run 01 -f 300", "offscreen, no privileges needed")
                      .example("sudo mini-render run 01 -b kms", "real scanout, Ctrl+C to stop")
                      .note("Keyboard control comes from this terminal, not from a keyboard "
                            "plugged into the board. Press q or Esc to quit.")
                      .note("MR_DUMP_DIR=<dir> writes every frame as PPM. "
                            "MR_SEED=<n> seeds randomised lessons.");

    auto options = parser.parse(argc, argv);

    if (options.command == "list") {
        return do_list();
    }
    if (options.command == "run") {
        if (! options.lesson.has_value() || options.lesson->empty()) {
            std::cerr << internal::color::red("error") << ": 'run' needs a lesson id\n";
            return do_list() == 0 ? 2 : 2;
        }
        return do_run(options, *env);
    }

    return 2;
}