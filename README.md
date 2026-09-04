# mini-render

经典图形学算法，一节课一个算法，从 **Bresenham 画线（1962）** 一路到
**Cornell Box**。全部 CPU 实现，实时上屏。

显示部分不自己做，依赖 [mini-wayland](../mini-wayland) 的 `mw::display::Screen`。
这边只关心"像素怎么算出来"。

## 快速开始

```sh
# 1. 先装 mini-wayland
cd third_party/mini-wayland && make install PREFIX=$HOME/.local && cd -
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig

# 2. 构建
make -j

# 3. 跑
./build/debug/bin/mini-render list
./build/debug/bin/mini-render run 01 -f 300        # 不需要 root
sudo ./build/debug/bin/mini-render run 01 -b kms   # 真上屏，Ctrl+C 停
```

键盘来自**运行程序的那个终端**（通常是 ssh 会话）。板子上那套键鼠不参与 ——
它连着板子自己的 tty，敲了这个程序收不到。`q` 或 `Esc` 退出。
不需要交互时加 `--no-input`；stdin 不是 tty（管道、CI）时自动关掉。

`offscreen` 是默认后端：不碰 DRM、不需要任何权限、开发机上直接跑。
**但它验证不了显示链路的任何东西**（没有 stride 对齐、没有真 vblank），
算法调完了要上真硬件跑一次。

```sh
MR_DUMP_DIR=/tmp/frames ./build/debug/bin/mini-render run 01 -f 3 --no-pace
# 每帧一张 PPM，用来做逐像素比对或者贴进笔记
```

## 构建与检查

```sh
make                    # debug
make BUILD=release
make SANITIZE=1         # ASan + UBSan
make check              # check-headers + lint + test  ← 提交前跑这个
make test               # 不变量检查
make lint               # 课的规则，grep 强制
make run LESSON=01 FRAMES=300
```

## 目录

```
include/mr/          算法与公共类型。lesson 只 include 这里
├── surface.hpp      可写像素平面 —— 与 mini-wayland 之间唯一的接缝
├── lesson.hpp       一节课的契约与注册宏
├── input.hpp        一帧的按键状态；课只读这个
├── terminal.hpp     stdin raw 模式与转义序列解码（只有 harness 用）
├── bug.hpp          MR_BUG：标记"只可能由实现错误到达"的分支
└── raster/line.hpp  Bresenham
src/                 实现；src/main.cpp 是全项目唯一的 main()
lessons/NN-slug/     一节课一个目录，加了即被构建、被 list 列出
tests/               不变量检查
scripts/new-lesson.sh   生成新课骨架
scripts/lint.sh         把约定变成构建期的硬失败
```

## 课

| 课 | 年份 | 主题 |
| --- | --- | --- |
| `01-bresenham` | 1962 | 整数增量判别式画线；八卦限对称形式；平凡拒绝 |

`01` 的键：`space` 暂停 / `←→` 调转速 / `↑↓` 加减辐条数 / `r` 复位。

按年份读这些算法是有意义的：能看出整个领域怎么从"只用整数加减法"
一步步走到"解渲染方程"。

## 两条值得先知道的约定

**1. 算法不写在 `lesson.cpp` 里。** `lesson.cpp` 只负责把算法画成
一个人能看出对错的画面；算法本身进 `include/mr/` + `src/`，
这样 `tests/` 能在没有屏幕的情况下直接调它。

**2. 写完算法先把它写错一次。** 加了不变量测试之后，故意注入一个 bug
确认测试真的会红。`tests/test_line.cpp` 对 Bresenham 注入了六种典型错误，
六种全被抓到 —— 其中四种表现为**根本不返回**而不是画错，
这个比例说明了为什么"跑一下看画面对不对"不够。

完整规约见 `CLAUDE.md`，架构取舍见 `docs/architecture.md`。