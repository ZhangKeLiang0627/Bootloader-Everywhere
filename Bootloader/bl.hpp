/**
 * @file    bl.hpp
 * @brief   LUMOS-bootloader 总头文件（宿主工程只需要认识这一个）
 *
 * 用法：
 *   - 想直接用库自带的入口 → 什么都不用包含，只要把 app/bl_entry.cpp
 *     加入编译、并保证 BL_PROVIDE_MAIN 为 1
 *   - 想接到自己的 main 里 → #include "bl.hpp"，然后调用 bl::bl_entry()
 *   - APP 侧只想用状态确认接口 → #include "bl.hpp" 后调 bl::app_confirm()
 *
 * 注意：bl_config.h 会被下面这些头间接包含，所以它必须能被编译器的
 *       include 路径找到（见 docs/PORTING.md 的「加入工程」一节）。
 */
#ifndef BL_HPP
#define BL_HPP

#include "bl_config.h"

#include "core/bl_types.hpp"
#include "core/bl_meta.hpp"
#include "port/bl_port.hpp"

#include "app/bl_entry.hpp"

#endif /* BL_HPP */
