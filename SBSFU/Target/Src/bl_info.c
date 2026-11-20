/**
  * @file    bl_info.c
  * @brief   Bootloader info block: the flash layout and DFU capabilities this
  *          bootloader was built with, at a fixed address (BL_INFO_ADDRESS, the
  *          end of the vector-table area; STM32L443xx_FLASH.ld places it). The
  *          application reads it to know it may use the preset slots.
  */
#include "memory_map.h"

__attribute__((section(".bl_info"), used))
const bl_info_t g_bl_info =
{
  .magic       = BL_INFO_MAGIC,
  .layout      = BL_INFO_LAYOUT,
  .reserved    = 0U,
  .slot_base   = MEM_SLOT1_BASE,
  .slot_end    = MEM_SLOT1_END,
  .preset_base = MEM_PRESET_BASE,
  .preset_size = MEM_PRESET_SIZE,
  .caps        = BL_CAPS,
  .reserved2   = 0U,
};

_Static_assert(sizeof(bl_info_t) == 32U, "bl_info_t is part of the app contract: 32 bytes");
_Static_assert(BL_INFO_ADDRESS + sizeof(bl_info_t) == MEM_FLASH_BASE + 0x200U,
               "the info block ends where the Secure Engine starts");
