/**
 * @file mr/terminal.hpp
 * @brief 把 stdin 切成 raw 模式，每帧非阻塞读键
 *
 * 只有 harness 用。课拿到的是解码之后的 `mr::Input`，
 * 不认识终端、不读 stdin（`make lint` 会查）。
 *
 * ## 只关两个标志位，别的都留着
 *
 * 教科书里的 "raw mode" 是 `cfmakeraw()`，它会把一整排标志位全关掉。
 * 这里**只关 `ICANON` 和 `ECHO`**，剩下的必须留着，每一个都有具体理由：
 *
 *   `OPOST` 留着 —— 关掉之后输出的 `\n` 不再自动补 `\r`，
 *                   于是每一行日志都比上一行往右缩进一格，
 *                   几十行之后输出爬到屏幕外面去。
 *                   而这个程序**一直在往 stderr 打日志**。
 *
 *   `ISIG`  留着 —— 关掉之后 Ctrl+C 不再产生 SIGINT，
 *                   现有的退出路径（信号 -> 标志位 -> 跳出帧循环 ->
 *                   Screen 析构 -> 关 CRTC）整条失效。
 *                   在一个占着 DRM master 的程序里，
 *                   失去 Ctrl+C 意味着只能从另一个 ssh 会话 kill。
 *
 *   `IXON`  留着 —— Ctrl+S / Ctrl+Q 流控仍然可用。
 *
 * 换句话说：我们要的只是"不要等回车、不要回显"，不是"接管整个终端"。
 *
 * ## 恢复
 *
 * 析构时恢复。正常退出（含 Ctrl+C，因为 `ISIG` 留着）都会走到。
 *
 * **崩溃不会。** 而这个项目里 `MR_BUG` 会 abort，开发期撞上的概率不低。
 * 所以额外做了两件事：
 *
 *  1. 保存的 termios 存在文件作用域的静态变量里，`restore_all()`
 *     是幂等的、不依赖对象还活着
 *  2. harness 把 `SIGSEGV` / `SIGABRT` 也挂上，在里面先恢复再走默认行为
 *
 * 这是尽力而为，不是保证。真的把终端搞坏了（没回显、没行编辑），
 * 盲敲 `reset` 回车就能修回来 —— 记在这里，因为那一刻你看不见自己在打什么。
 *
 * ## 不是 tty 的情况
 *
 * stdin 被重定向（管道、`< /dev/null`、CI）时不进 raw 模式，
 * `interactive()` 返回 false，`poll()` 什么也不做。
 * 课照跑，只是永远读到"什么都没按" —— 这正是无人值守跑图时想要的。
 */
#pragma once

#include <string>

#include "mr/input.hpp"

namespace mr {

namespace detail {

/// `decode_key()` 的结果
struct DecodeResult {
    Key key = Key::None;
    size_t consumed = 0;   ///< 消耗了几个字节。0 且 need_more 为真 = 序列没读完
    bool need_more = false;
};

/**
 * @brief 从缓冲区头部解出一个键
 *
 * 之所以把它暴露出来（而不是藏在 .cpp 的匿名命名空间里）：
 * **这是这一块里唯一真正容易出错的逻辑**，而它没法通过
 * `Terminal` 间接测 —— `Terminal` 只在 `isatty()` 为真时才读，
 * 而单测里拿不到真终端（pipe 不是 tty，openpty 要多一个依赖）。
 *
 * 转义序列认错了不会崩，只会表现为"按一次方向键触发三次动作"
 * 或者"偶尔按了没反应"。这两种症状在画面位于另一块屏幕、
 * 人在 ssh 终端里的情况下，几乎不可能靠肉眼定位。
 *
 * 只认这个项目用得到的几种。不认识的整段丢掉 —— 一份写了一半的
 * 转义序列解析器比没有更糟，它会把不认识的序列拆成一串"按键"。
 */
DecodeResult decode_key(const char* buf, size_t len) noexcept;

} // namespace detail

class Terminal {
  public:
    /// @param enabled false 时完全不碰终端（`--no-input`）。
    ///                做成构造参数而不是 poll 的分支，是为了让"关掉输入"
    ///                和"stdin 不是 tty"走同一条路径 —— 两者对课来说
    ///                是同一件事：永远读到"什么都没按"。
    explicit Terminal(bool enabled = true) noexcept;
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal&&) = delete;

    /// stdin 是不是一个可以进 raw 模式的终端
    bool interactive() const noexcept {
        return interactive_;
    }

    /**
     * @brief 推进一帧：清掉上一帧的边沿状态，读走所有待处理的输入
     *
     * 非阻塞，每帧调一次。**即使 `interactive()` 是 false 也必须调** ——
     * 清边沿这件事和有没有终端无关，不调的话 `pressed()` 会一直为真。
     *
     * @param now_ns CLOCK_MONOTONIC 纳秒，用于 `Input::down()` 的窗口判定
     */
    void poll(Input& input, uint64_t now_ns) noexcept;

    /// 一行摘要，启动时打一次
    std::string describe() const;

    /**
     * @brief 幂等地把终端恢复原样
     *
     * 析构会调。也可以从信号处理里调 —— `tcsetattr` 不在
     * POSIX 的 async-signal-safe 清单上，但它实质是一次系统调用，
     * 而这里的取舍是"可能不安全地恢复" vs "确定留下一个坏掉的终端"。
     */
    static void restore_all() noexcept;

  private:
    bool interactive_ = false;

    /// 跨读取残留的转义序列前缀。方向键是 3 个字节，
    /// 一次 read 有可能只读到前一半 —— 60fps 下很少见，但会发生，
    /// 而表现是"偶尔按方向键没反应"，非常难查。
    char pending_[8] = {};
    size_t pending_len_ = 0;
};

} // namespace mr