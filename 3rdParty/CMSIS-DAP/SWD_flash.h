#ifndef __SWD_FLASH_H__
#define __SWD_FLASH_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "error.h"
#include "flash_blob.h"

error_t target_flash_init(uint32_t flash_start);
error_t target_flash_uninit(void);
error_t target_flash_program_page(uint32_t addr, const uint8_t *buf, uint32_t size);
error_t target_flash_erase_sector(uint32_t addr);
error_t target_flash_erase_chip(void);

// 动态选择flash算法
void swd_flash_select_algo(const program_target_t *algo);
const program_target_t *swd_flash_get_selected_algo(void);

#ifdef __cplusplus
}
#endif

#endif // __SWD_FLASH_H__
