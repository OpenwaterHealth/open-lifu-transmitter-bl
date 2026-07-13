/*
*****************************************************************************
**  File        : mapping_fwimg.ld
**  Abstract    : Firmware image slot mapping for single-image SBSFU on
**                STM32L443 (256 KB flash, 2 KB pages).
*****************************************************************************
*/

/* Slots must be aligned on the 2 KB page size.
 * See Core/Inc/memory_map.h for the full flash partition map. Layout:
 *   pages 0-31    0x08000000   64K  bootloader (SBSFU + SE)
 *   pages 32-125  0x08010000  188K  APP SLOT 1 (active)   <-- this file
 *   page  126     0x0803F000    2K  anti-rollback version log
 *   page  127     0x0803F800    2K  user config (app-owned) */

/* Active slot #1: pages 32-125 = 94 x 2 KB = 188 KB */
__ICFEDIT_SLOT_Active_1_header__ = 0x08010000;
__ICFEDIT_SLOT_Active_1_start__  = 0x08010000;
__ICFEDIT_SLOT_Active_1_end__    = 0x0803EFFF;

/* Slots 2 and 3 are not managed by SBSFU in this single-image configuration. */
__ICFEDIT_SLOT_Active_2_header__ = 0x00000000;
__ICFEDIT_SLOT_Active_2_start__  = 0x00000000;
__ICFEDIT_SLOT_Active_2_end__    = 0x00000000;
__ICFEDIT_SLOT_Active_3_header__ = 0x00000000;
__ICFEDIT_SLOT_Active_3_start__  = 0x00000000;
__ICFEDIT_SLOT_Active_3_end__    = 0x00000000;
