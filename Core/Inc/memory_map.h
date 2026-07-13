/**
  ******************************************************************************
  * @file    memory_map.h
  * @author  gvigelet
  * @brief   Single source of truth for the STM32L443 (256 KB flash, 2 KB pages)
  *          secure-bootloader flash partition map.
  *
  * The values below MUST be kept in sync with (linker scripts cannot include
  * C headers, so the same literals are repeated there with cross-references):
  *   - STM32L443xx_FLASH.ld / Linker/mapping_sbsfu.ld  (bootloader + SE regions)
  *   - Linker/mapping_fwimg.ld                          (application slot)
  *   - SECoreBin/Scripts/STM32L443xx.ld                 (secure engine binary)
  *   - USB_DEVICE/App/usbd_dfu_if.c                     (DFU descriptor + bounds)
  *   - SBSFU/App/Inc/sfu_fwimg_regions.h                (SFU_IMG_IMAGE_OFFSET)
  *   - py-tools/ (sign_firmware.py / flash_firmware.py defaults)
  *
  * Flash partition (128 pages x 2 KB = 256 KB):
  *   pages 0-31    0x08000000  64 KB   bootloader (SBSFU + SE, read-only via DFU)
  *   pages 32-125  0x08010000 188 KB   APP SLOT 1 (SBSFU header @ +0, app @ +0x400)
  *   page  126     0x0803F000   2 KB   anti-rollback version log (not DFU writable)
  *   page  127     0x0803F800   2 KB   user config (app-owned, not DFU writable)
  ******************************************************************************
  */

#ifndef INC_MEMORY_MAP_H_
#define INC_MEMORY_MAP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flash geometry ------------------------------------------------------------*/
#define MEM_FLASH_BASE           0x08000000UL
#define MEM_FLASH_SIZE           0x00040000UL   /* 256 KB */
#define MEM_FLASH_END            (MEM_FLASH_BASE + MEM_FLASH_SIZE)
#define MEM_PAGE_SIZE            0x00000800UL   /* 2 KB erase page */

/* Bootloader (SBSFU + SECoreBin), pages 0-31 --------------------------------*/
#define MEM_BOOTLOADER_BASE      0x08000000UL
#define MEM_BOOTLOADER_SIZE      0x00010000UL   /* 64 KB */
#define MEM_BOOTLOADER_END       (MEM_BOOTLOADER_BASE + MEM_BOOTLOADER_SIZE)

/* Application slot 1 (SBSFU active slot), pages 32-125 ----------------------
 * Signed image header (SFU1, 0x140 bytes) at slot base; firmware body starts
 * at slot base + MEM_APP_IMAGE_OFFSET. The application must be linked at
 * MEM_APP_RUN_ADDRESS with its vector table there (VTOR). */
#define MEM_SLOT1_BASE           0x08010000UL
#define MEM_SLOT1_SIZE           0x0002F000UL   /* 188 KB */
#define MEM_SLOT1_END            (MEM_SLOT1_BASE + MEM_SLOT1_SIZE)  /* exclusive */

/* Anti-rollback version log, page 126 ---------------------------------------
 * Append-only log of accepted firmware versions (see SBSFU/Target/Src/
 * anti_rollback.c). Outside the DFU writable window so DFU cannot erase it. */
#define MEM_ANTIROLLBACK_BASE    0x0803F000UL
#define MEM_ANTIROLLBACK_SIZE    MEM_PAGE_SIZE

#define MEM_APP_IMAGE_OFFSET     0x00000400UL
#define MEM_APP_RUN_ADDRESS      (MEM_SLOT1_BASE + MEM_APP_IMAGE_OFFSET)

/* User configuration page (application-owned), page 127 ---------------------*/
#define MEM_USER_CONFIG_BASE     0x0803F800UL
#define MEM_USER_CONFIG_SIZE     MEM_PAGE_SIZE

/* USB DFU writable window: exactly the application slot ---------------------*/
#define MEM_DFU_WRITABLE_BASE    MEM_SLOT1_BASE
#define MEM_DFU_WRITABLE_END     MEM_SLOT1_END  /* exclusive */

/* SRAM ----------------------------------------------------------------------*/
#define MEM_SRAM1_BASE           0x20000000UL
#define MEM_SRAM1_SIZE           0x0000C000UL   /* 48 KB */
#define MEM_SRAM2_BASE           0x10000000UL
#define MEM_SRAM2_SIZE           0x00004000UL   /* 16 KB */

#ifdef __cplusplus
}
#endif

#endif /* INC_MEMORY_MAP_H_ */
