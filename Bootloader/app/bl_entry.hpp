/**
 * @file    bl_entry.hpp
 * @brief   Bootloader 入口 —— 库对外的唯一动作接口
 */
#ifndef BL_ENTRY_HPP
#define BL_ENTRY_HPP

namespace bl {

/**
 * @brief 启动 Bootloader：自检 → 判固件 → 跳 APP 或进入 IAP 等待
 *
 * 永不返回。调用前不需要做任何准备：时钟、时基、串口、Flash 都由它自己
 * 建立（见 port/bl_port.hpp 的 platform_init）。
 *
 * 两种接法：
 *   1. 直接用库的 main。BL_PROVIDE_MAIN 为 1（默认）时，
 *      app/bl_entry.cpp 会自带 main()，你的工程把自带的 main.c 排除即可。
 *   2. 接到已有工程。把 BL_PROVIDE_MAIN 设为 0，然后在自己的 main() 里
 *      调用 bl_entry()（放在时钟/串口初始化之后更稳，但并非必须 ——
 *      platform_init() 是幂等的，重复配置不会出问题）。
 */
[[noreturn]] void bl_entry() noexcept;

} // namespace bl

#endif /* BL_ENTRY_HPP */
