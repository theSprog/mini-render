/**
 * @file mr/lesson.hpp
 * @brief 一节课的契约，以及它的注册方式
 *
 * ## 这个文件存在的理由
 *
 * 本项目会有几十节课，从 Bresenham 一路到 Cornell Box。
 * 如果每节课都是一个独立 `main()`，那么每节课都要各自写一遍：
 * 参数解析、信号处理、开显示、帧循环、落盘、统计、错误处理。
 * 结果必然是几十份互相略有不同的样板 —— 而且**每一份都有机会写错**，
 * 尤其是当其中很多是 AI 写的时候。
 *
 * 所以这里反过来：**harness 是唯一的 `main()`，课只提供一个函数。**
 *
 * @code
 *   static void render(mr::Surface& out, const mr::LessonParams& p) {
 *       out.clear(mr::kBlack);
 *       // ... 画东西 ...
 *   }
 *   MR_LESSON("01-bresenham", "Bresenham 画线", 1962, Cadence::Animated, render);
 * @endcode
 *
 * 一节课就是一个目录一个 `.cpp`，加了就被构建、被 `list` 列出、被 `run` 找到，
 * 不用改 Makefile，不用注册到任何列表里。
 *
 * ## 课不允许做的事
 *
 * 这几条由 `make lint` 用 grep 强制，不靠自觉（见 `scripts/lint.sh`）：
 *
 *   - 不写 `main()`
 *   - 不碰 `argc` / `argv` / `getenv` / `sigaction`
 *   - 不写 ANSI 转义序列（要彩色输出用 `internal::color`）
 *   - 不 include `<mw/...>`（认识 mini-wayland 的只有 harness）
 *
 * 前三条是"已经有现成的了"，第四条是隔离。都不是风格洁癖：
 * 违反任何一条都会让这节课没法在单测里跑（单测里没有 argv，也没有屏幕）。
 *
 * ## Cadence：三种课，一套循环
 *
 * harness 的帧循环对三者是同一份代码，差别只在**怎么解释 render 的结果**：
 *
 *   `Static`      —— 画一次就定了。harness 仍然每帧调用它（画一次的开销
 *                    可以忽略，而"每帧重画"消除了双缓冲下"只画了一块 buffer"
 *                    这类 bug 的整个可能性）。
 *   `Animated`    —— 用 `params.time_s` / `params.frame` 做动画。
 *   `Progressive` —— 每次调用往前算一点，自己在内部累积，把当前结果画出来。
 *                    路径追踪就是这种：屏幕上看着噪点一点点收敛。
 *
 * ## render 拿到的是哪块内存
 *
 * **直接就是要被扫描出去的那块 buffer**（KMS 后端下是 dumb buffer 的
 * mmap 指针，offscreen 后端下是堆内存）。中间没有拷贝。
 *
 * 代价是：那是双缓冲里轮转的两块之一，**跨帧不保留内容**。
 * 需要累积的课自己带一个 `OwnedSurface`，算完 `blit()` 过来 ——
 * `Progressive` 的课都该这么写。
 *
 * 好处是：`Static` / `Animated` 的课真的是在往显存里写像素，
 * 中间没有一层善意的抽象。这个项目的一半意义就在这里。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mr/surface.hpp"

namespace mr {

enum class Cadence {
    Static,       ///< 画一次就定了
    Animated,     ///< 用 time_s 做动画
    Progressive,  ///< 每帧往前算一点，自己累积
};

const char* to_string(Cadence cadence) noexcept;

/// 每帧传给课的东西。**只读**，而且刻意很少 ——
/// 课需要的输入越少，越容易在单测里构造。
struct LessonParams {
    uint64_t frame = 0;   ///< 从 0 开始
    double time_s = 0.0;  ///< 第一帧以来的秒数
    uint32_t seed = 0;    ///< 随机算法的种子。默认固定，让结果可复现
};

using RenderFn = void (*)(const Surface&, const LessonParams&);

struct Lesson {
    /// `NN-slug`，两位数字开头。`list` 按它排序，所以顺序即课号顺序。
    const char* id = nullptr;
    const char* title = nullptr;

    /// 算法发表年份。放在这里不是为了好看 —— 按年份读这些算法，
    /// 能看出整个领域是怎么从"整数加减法"一步步走到"解渲染方程"的。
    int year = 0;

    Cadence cadence = Cadence::Static;
    RenderFn render = nullptr;
};

/// 注册表的只读视图。刻意不用 `mw::span` —— 见文件开头第四条：
/// 除了 harness，本项目任何地方都不认识 `mw::`，这个头文件也不例外。
struct LessonList {
    const Lesson* const* first = nullptr;
    size_t count = 0;

    const Lesson* const* begin() const noexcept {
        return first;
    }
    const Lesson* const* end() const noexcept {
        return first + count;
    }
};

/// 全部已注册的课，按 `id` 排序。**不要在静态初始化期间调用**（顺序未定义）。
LessonList lessons() noexcept;

/// 找不到返回 nullptr。支持前缀匹配：`run 01` 命中 `01-bresenham`。
const Lesson* find_lesson(std::string_view id) noexcept;

namespace detail {
/// `MR_LESSON` 用。返回值只是为了能初始化一个 static 变量。
bool register_lesson(const Lesson& lesson) noexcept;
} // namespace detail

/**
 * @brief 注册一节课
 *
 * 放在文件作用域，分号结尾。
 *
 * @warning 注册靠静态初始化，而静态初始化**只有当那个 TU 被链接进来时
 *          才会发生**。所以 lesson 的 `.o` 必须直接链进可执行文件，
 *          不能先打包成 `.a` —— 链接器不会从静态库里拉一个没有任何
 *          符号被引用的目标文件进来，课会静默消失。Makefile 里
 *          lesson 对象是单独一组，注释写明了这一点。
 */
#define MR_LESSON(id_, title_, year_, cadence_, fn_)                            \
    namespace {                                                                 \
    const bool mr_registered_##fn_ = ::mr::detail::register_lesson(             \
        ::mr::Lesson{(id_), (title_), (year_), (cadence_), (&fn_)});            \
    }                                                                           \
    static_assert(true, "MR_LESSON needs a trailing semicolon")

} // namespace mr
