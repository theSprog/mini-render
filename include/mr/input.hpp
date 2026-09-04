/**
 * @file mr/input.hpp
 * @brief 一帧的按键状态
 *
 * ## 为什么输入是从终端来的，而不是 libinput
 *
 * 实际的开发方式是两套设备：
 *
 *   **编码设备** —— 写代码、跑 rsync、开着 ssh 会话。手放在这里。
 *   **开发板**   —— 自带键鼠和显示器。画面出在这块屏幕上。
 *
 * 循环是：编码设备上改代码 → rsync 到板子 → 切终端 tab → ssh 进去
 * `make` → `sudo ./bin/mini-render run ...` → **转头看板子的显示器**。
 *
 * 注意"转头"这个动作：**只用来看，不用来操作。**
 *
 * 所以这里的取舍不是"evdev 不可行" —— 板子上确实插着键盘，
 * `libinput` / evdev 完全读得到，Step 5 也正是要走那条路。
 * 取舍是**手放在哪**：走 evdev 就得整个人挪到板子那边去按键，
 * 而代码、编辑器、日志、git 全在编码设备上。为了按一下空格暂停
 * 就得换一套键盘，这个循环撑不了几次。
 *
 * ssh 会话本身就是一个一直在手边的键盘。stdin 切成 raw 模式、
 * 每帧非阻塞读一次，就有了完整的键盘输入：零依赖（termios 属于 libc）、
 * 不需要额外权限（stdin 本来就是你的）、和 `make` 在同一个 tab 里。
 *
 * 还有一个附带的好处：它不占用板子的键盘。板子上那套键鼠仍然连着
 * 板子的 VT，可以同时用来干别的（切 VT、看 dmesg、应急登录）。
 *
 * ## 一个会让人困惑的地方
 *
 * 板子上的键盘敲下去，字符进的是**板子的 tty**，不是你的 ssh 会话。
 * 所以隔着桌子伸手去按板子的键盘，这个程序收不到 —— 画面在那块屏幕上
 * 会让人下意识去按那边的键盘。要用板子的键盘控制，得在板子的 tty 上
 * 直接跑这个程序（那时 stdin 就是那个 tty，一样能用）。
 *
 * ## 这和 `mw/input/`（Step 5）不冲突
 *
 * 两者目的不同，会长期共存：
 *
 *   本文件      —— 遥控一个 demo。走 ssh 会话的 stdin。
 *   `mw/input/` —— 合成器的 seat，把事件路由给 wayland 客户端、
 *                  驱动 cursor plane。走板子上那套真键鼠。
 *
 * 后者不是可选项：Step 5 的验收标准里写着"鼠标移动（Cursor Plane）"，
 * 那必须是板子上真的那只鼠标产生的 evdev 事件。而板子上确实有，
 * 所以那条验收跑得起来。
 *
 * Step 5 做出 `mw/input/` 之后，harness 可以改成从那边填同一个
 * `Input` 结构，**课一行都不用改** —— 这正是把输入放在 harness
 * 而不是课里的价值。
 *
 * ## 终端输入拿不到的东西
 *
 * 这些是这个方案的真实代价，写在这里而不是等人踩：
 *
 *  1. **没有"抬起"事件。** 终端只报"某个键产生了一个字符"。
 *     所以没有 `released()`，`down()` 只能用"最后一次事件之后的
 *     一小段时间内算按住"来近似。
 *  2. **按住是靠自动重复模拟的。** 终端的自动重复有一个初始延迟
 *     （典型 ~500ms）然后才开始按重复率发送。所以按住方向键时，
 *     相机会先动一下、停一下、然后才连续动。这不是 bug，
 *     是终端的行为，evdev 那条路才没有这个问题。
 *  3. **没有修饰键状态。** 拿不到"Shift 正被按着"，只能拿到
 *     大写字母这个结果。
 *  4. **没有鼠标。** 终端鼠标上报（`\e[?1000h`）能做，但那是另一件事，
 *     等真的需要再说。
 *
 * 这四条决定了适合用它做什么：**切模式、调参数、暂停、单步、
 * 粗粒度转视角**。不适合做需要精细连续控制的东西。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mr {

/**
 * @brief 键码
 *
 * 可打印 ASCII 直接用自己的编码（`Key{'a'}` 就是 a 键），
 * 特殊键从 256 往上排，避开 ASCII 区间。
 *
 * 这样做的好处是 `input.pressed(key('w'))` 读起来就是它的意思，
 * 不需要为每个字母写一个枚举值。
 */
enum class Key : uint16_t {
    None = 0,

    // 32..126 是可打印 ASCII，用 key() 构造
    Space = 32,

    // 特殊键
    Enter = 256,
    Escape,
    Tab,
    Backspace,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,

    Count
};

constexpr Key key(char c) noexcept {
    return static_cast<Key>(static_cast<unsigned char>(c));
}

const char* to_string(Key k) noexcept;

/**
 * @brief 一帧的按键状态
 *
 * 由 harness 每帧填好后交给课。课**只读**。
 *
 * 默认构造出来的 `Input` 是"什么都没按" —— 单测里不接终端时
 * 拿到的就是它，所以用到输入的课在单测里也能跑。
 */
class Input {
  public:
    constexpr Input() noexcept = default;

    /// 本帧收到了这个键（边沿）。菜单、切模式、暂停用这个。
    bool pressed(Key k) const noexcept;

    /**
     * @brief 近似的"按住"
     *
     * 判据是"最后一次收到这个键之后还不到 `kHoldWindowMs` 毫秒"。
     * 终端不报抬起，只能这么做 —— 见文件开头第 1、2 条。
     *
     * 连续控制（转相机、调参数）用这个。
     */
    bool down(Key k) const noexcept;

    /// A/D 或 ←/→，返回 -1 / 0 / +1
    int axis_x() const noexcept;

    /// W/S 或 ↑/↓，返回 -1 / 0 / +1（**向上为正**）
    int axis_y() const noexcept;

    /// 本帧按下的数字键，没有则返回 -1。切模式用。
    int digit_pressed() const noexcept;

    /// q 或 Escape。harness 会据此退出，课一般不用管。
    bool quit_requested() const noexcept;

    /// 本帧有没有任何按键。用来决定要不要打一行状态日志。
    bool any_pressed() const noexcept;

    /// "按住"的判定窗口。比终端的自动重复间隔宽，比人眼能察觉的停顿短。
    static constexpr uint64_t kHoldWindowMs = 120;

    // ---- 写入端：harness 用，课不要调 ----
    //
    // 本来是 private + friend Terminal 的，改成公开是因为那样单测就
    // 完全够不着这两个函数 —— 而"边沿只活一帧""保持窗口到点失效"
    // 这两条语义正是最该被测的东西。
    //
    // 课误用它们会怎样：把自己伪造的按键喂进去。这不会崩，也不会
    // 影响别的课（Input 是 harness 每帧新填的），所以没有为它加
    // lint 规则 —— 规则集要留给真正会造成损害的事情。

    /// 每帧开头调：清掉边沿状态，记下当前时刻。
    /// **即使没有终端也必须调**，否则 `pressed()` 会卡在上一帧的值上。
    void begin_frame(uint64_t now_ns) noexcept;

    /// 解码出一个键时调
    void feed(Key k, uint64_t now_ns) noexcept;

  private:
    static constexpr size_t kSlots = static_cast<size_t>(Key::Count);

    bool in_range(Key k) const noexcept {
        return static_cast<size_t>(k) < kSlots;
    }

    uint64_t last_event_ns_[kSlots] = {};
    bool pressed_[kSlots] = {};
    uint64_t now_ns_ = 0;
};

} // namespace mr