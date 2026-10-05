/**
 * @file    bl.hpp
 * @brief   LUMOS-bootloader 总头文件（宿主工程只需要认识这一个）
 *
 * 用法：
 *   - 用库自带入口 → 编译 app/bl_entry.cpp，保持 BL_PROVIDE_MAIN=1
 *   - 接到自己的 main 里 → #include "bl.hpp"，调 bl::bl_entry()
 */
#ifndef BL_HPP
#define BL_HPP

#include "bl_config.h"

#include "core/bl_types.hpp"
#include "core/bl_meta.hpp"
#include "port/bl_port.hpp"

#include "app/bl_entry.hpp"

#endif /* BL_HPP */
