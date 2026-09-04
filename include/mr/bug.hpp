/**
 * @file mr/bug.hpp
 * @brief `MR_BUG` —— 标记"只可能由实现错误到达"的分支
 *
 * 和错误处理不是一回事。错误处理面对的是**环境**：文件打不开、
 * 设备不支持、参数非法。`MR_BUG` 面对的是**我们自己写错了**。
 *
 * 两者要分开，因为处置方式相反：环境错误要优雅返回并报告，
 * 实现错误要尽可能响亮 —— 越早、越吵，越省事。
 *
 * debug 下 abort 并打出位置；release 下什么也不做，
 * 由调用点自己走一条安全的退路（通常是 return）。
 *
 * 这个项目里很多代码是 AI 写的，而 AI 写出来的循环最典型的
 * 失败形态就是"看起来完全正确但终止条件不成立"。
 * `MR_BUG` 是把那类失败从"挂死"变成"一行明确的报错"的手段。
 */
#pragma once

#include <cstdio>
#include <cstdlib>

#if defined(MR_DEBUG)
#define MR_BUG(msg)                                                                  \
    do {                                                                             \
        std::fprintf(stderr, "BUG %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
        std::abort();                                                                \
    } while (false)
#else
#define MR_BUG(msg) ((void)(msg))
#endif
