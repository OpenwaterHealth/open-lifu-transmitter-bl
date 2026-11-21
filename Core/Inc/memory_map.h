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
  * Flash layout (2 KB pages). The same in the application's memory_map.h
  * (openlifu-transmitter-fw): change both together.
  *
  *   pages   0- 31  0x08000000  secure bootloader (64 KB)
  *   pages  32- 95  0x08010000  application slot (128 KB): signed header, then
  *                              the image at +0x400
  *   pages  96-125  0x08030000  presets (60 KB): 15 slots of 4 KB (two
  *                              pages), preset N at 0x08030000 + N x 0x1000
  *   page      126  0x0803F000  the bootloader's anti-rollback version log
  *   page      127  0x0803F800  runtime config (the application's lifu_config.h)
  *
  * Only the slot is DFU writable. The presets, the anti-rollback log and the
  * runtime config sit outside it, so slot verification ignores them and a
  * firmware update never erases them.
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

/* Application slot 1 (SBSFU active slot), pages 32-95 -----------------------
 * Signed image header (SFU1, 0x140 bytes) at slot base; firmware body starts
 * at slot base + MEM_APP_IMAGE_OFFSET. The application must be linked at
 * MEM_APP_RUN_ADDRESS with its vector table there (VTOR). */
#define MEM_SLOT1_BASE           0x08010000UL
#define MEM_SLOT1_SIZE           0x00020000UL   /* 128 KB */
#define MEM_SLOT1_END            (MEM_SLOT1_BASE + MEM_SLOT1_SIZE)  /* exclusive */

/* Presets, pages 96-125 -----------------------------------------------------
 * The application's preset store. Outside the slot: not DFU writable, and
 * not part of slot verification. */
#define MEM_PRESET_BASE          0x08030000UL
#define MEM_PRESET_SIZE          0x0000F000UL   /* 60 KB */
#define MEM_PRESET_END           (MEM_PRESET_BASE + MEM_PRESET_SIZE)  /* exclusive */
#define MEM_PRESET_SLOT_SIZE     0x00001000UL   /* 4 KB: two pages */
#define MEM_PRESET_SLOT_COUNT    15U
#define MEM_PRESET_SLOT_BASE(n)  (MEM_PRESET_BASE + ((uint32_t)(n) * MEM_PRESET_SLOT_SIZE))
/* A slot holds one signed preset: its PRS1 header (the SFU1 header layout,
 * sizeof(SE_FwRawHeaderTypeDef)), then the preset blob. DFU writes a slot only
 * once that header verified against this bootloader's key (dfu_preset.c). */
#define MEM_PRESET_BODY_OFFSET   0x00000140UL
#define MEM_PRESET_BODY_MAX      (MEM_PRESET_SLOT_SIZE - MEM_PRESET_BODY_OFFSET)

/* Anti-rollback version log, page 126 ---------------------------------------
 * Append-only log of accepted firmware versions (see SBSFU/Target/Src/
 * anti_rollback.c). Outside the DFU writable window so DFU cannot erase it. */
#define MEM_ANTIROLLBACK_BASE    0x0803F000UL
#define MEM_ANTIROLLBACK_SIZE    MEM_PAGE_SIZE

#define MEM_APP_IMAGE_OFFSET     0x00000400UL
#define MEM_APP_RUN_ADDRESS      (MEM_SLOT1_BASE + MEM_APP_IMAGE_OFFSET)

/* Runtime config (application-owned: lifu_config.h), page 127 --------------*/
#define MEM_APP_CONFIG_BASE      0x0803F800UL
#define MEM_APP_CONFIG_SIZE      MEM_PAGE_SIZE

_Static_assert(MEM_SLOT1_END == MEM_PRESET_BASE, "the presets sit right above the slot");
_Static_assert(MEM_PRESET_SLOT_COUNT * MEM_PRESET_SLOT_SIZE == MEM_PRESET_SIZE,
               "the preset slots fill the preset pages exactly");
_Static_assert(MEM_PRESET_END == MEM_ANTIROLLBACK_BASE, "then the anti-rollback log");
_Static_assert(MEM_ANTIROLLBACK_BASE + MEM_ANTIROLLBACK_SIZE == MEM_APP_CONFIG_BASE,
               "then the runtime config");
_Static_assert(MEM_APP_CONFIG_BASE + MEM_APP_CONFIG_SIZE == MEM_FLASH_END, "which is the last page");

/* USB DFU writable window: exactly the application slot ---------------------*/
#define MEM_DFU_WRITABLE_BASE    MEM_SLOT1_BASE
#define MEM_DFU_WRITABLE_END     MEM_SLOT1_END  /* exclusive */

/* Bootloader info block ------------------------------------------------------
 * 32 bytes at a fixed address at the end of the vector-table area (bl_info.c,
 * STM32L443xx_FLASH.ld), where the application reads this layout. Bootloaders
 * before 1.1.0-rc.1 have only fill bytes here, so the magic tells them apart:
 * the application stores presets only under a bootloader that has one. */
#define BL_INFO_ADDRESS          0x080001E0UL
#define BL_INFO_MAGIC            0x4942574FUL   /* 'OWBI' */
#define BL_INFO_LAYOUT           1U

/* DFU capability word: served behind "OWC1" at the USB DFU virtual capability
 * address and by the I2C GETCAPS command, and copied into the info block. */
#define BL_CAP_VERIFY_BEFORE_ERASE   0x00000001UL  /* the SFU1 header is checked before any erase */
#define BL_CAP_SIGNED_PRESET_WRITE   0x00000002UL  /* preset slots take signed PRS1 presets */
#define BL_CAPS                      (BL_CAP_VERIFY_BEFORE_ERASE | BL_CAP_SIGNED_PRESET_WRITE)

typedef struct
{
  uint32_t magic;          /* BL_INFO_MAGIC */
  uint16_t layout;         /* BL_INFO_LAYOUT */
  uint16_t reserved;
  uint32_t slot_base;
  uint32_t slot_end;       /* exclusive */
  uint32_t preset_base;
  uint32_t preset_size;    /* MEM_PRESET_SLOT_COUNT slots of MEM_PRESET_SLOT_SIZE */
  uint32_t caps;           /* BL_CAPS */
  uint32_t reserved2;
} bl_info_t;               /* 32 bytes */

/* SRAM ----------------------------------------------------------------------*/
#define MEM_SRAM1_BASE           0x20000000UL
#define MEM_SRAM1_SIZE           0x0000C000UL   /* 48 KB */
#define MEM_SRAM2_BASE           0x10000000UL
#define MEM_SRAM2_SIZE           0x0000A000UL   /* 40 KB */

#ifdef __cplusplus
}
#endif

#endif /* INC_MEMORY_MAP_H_ */
