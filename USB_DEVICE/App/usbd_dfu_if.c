/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usbd_dfu_if.c
  * @brief          : Usb device for Download Firmware Update.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "usbd_dfu_if.h"

/* USER CODE BEGIN INCLUDE */
#include "main.h"
#include "memory_map.h"
#include "version.h"   /* FW_VERSION (CMake-generated git describe) */
#include <string.h>
/* USER CODE END INCLUDE */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* Set once a DFU image has actually been programmed this session. Gates the
 * post-manifestation reboot so we never reset spuriously at power-on (where
 * manif_state already reads DFU_MANIFEST_COMPLETE). */
static volatile uint8_t s_dfu_image_written = 0U;

/* Anti-rollback (DFU-time enforcement). Before the first erase wipes the active
 * slot, we latch the version of the currently-installed image; after the new
 * image is downloaded we compare. A downgrade is rejected and the image is
 * destroyed so it can never boot. The floor is the currently-installed version
 * (0 if the slot has no valid "SFU1" header). NOTE: this guards the DFU update
 * path only; a direct SWD/debugger reflash bypasses it, so production units must
 * keep the debug port locked (RDP). The boot path additionally enforces the
 * persistent floor (see SBSFU/Target/Src/anti_rollback.c). */
static uint8_t  s_cur_ver_captured  = 0U;   /* 1 once s_current_fw_version is latched */
static uint16_t s_current_fw_version = 0U;  /* installed FwVersion at DFU entry (the floor) */

/* Set when the host writes to DFU_RESET_VIRT_ADDR (see MEM_If_Write_FS). The
 * bootloader main loop polls DFU_ResetRequested() and performs the reset. */
static volatile uint8_t s_dfu_reset_requested = 0U;

/* USER CODE END PV */

/** @addtogroup STM32_USB_OTG_DEVICE_LIBRARY
  * @brief Usb device.
  * @{
  */

/** @defgroup USBD_DFU
  * @brief Usb DFU device module.
  * @{
  */

/** @defgroup USBD_DFU_Private_TypesDefinitions
  * @brief Private types.
  * @{
  */

/* USER CODE BEGIN PRIVATE_TYPES */

/* USER CODE END PRIVATE_TYPES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Defines
  * @brief Private defines.
  * @{
  */

/* STM32L443: 256KB flash, single bank, 128 x 2KB pages. See Core/Inc/memory_map.h.
 * DfuSe sector type letters encode access (bit0=read, bit1=erase, bit2=write):
 *   'a' = 1 = read-only,  'g' = 7 = read+erase+write.
 *   pages 0-31    bootloader (SBSFU + SE)   read-only    -> 32*002Ka
 *   pages 32-125  active app slot           r/w/erase    -> 94*002Kg
 *   page  126     anti-rollback floor log   read-only  \
 *   page  127     user config               read-only  / -> 02*002Ka
 * The DFU writable window is clamped to the SBSFU active slot
 * (SLOT_ACTIVE_1: 0x08010000-0x0803EFFF, see Linker/mapping_fwimg.ld) so that
 * ALL DFU-writable flash is covered by secure-boot slot verification. */
#define FLASH_DESC_STR      "@Internal Flash/0x08000000/32*002Ka,94*002Kg,02*002Ka"

/* DFU writable window: the active application slot only. The bootloader and the
 * anti-rollback log (below APP_FLASH_BASE) and the user-config page (at/above
 * FLASH_END_ADDR) are excluded and cannot be erased or written over DFU.
 * FLASH_END_ADDR is the active-slot end + 1 (exclusive). */
#define APP_FLASH_BASE      MEM_DFU_WRITABLE_BASE   /* 0x08010000 (active slot start) */
#define FLASH_END_ADDR      MEM_DFU_WRITABLE_END    /* 0x0803F000 (active slot end + 1) */
#define DFU_PAGE_SIZE       MEM_PAGE_SIZE           /* 2KB erase page */

/* USER CODE BEGIN PRIVATE_DEFINES */

/* Virtual DFU UPLOAD address (outside flash). A host that points the DfuSe
 * address pointer here and uploads receives the bootloader version string
 * (FW_VERSION from version.h), null-padded to DFU_VERSION_READ_LEN bytes.
 * This is a read-only query: nothing is written and no flash is touched. */
#define DFU_VERSION_VIRT_ADDR   0xFFFFFF00U
#define DFU_VERSION_READ_LEN    64U

/* Virtual DFU DNLOAD address (outside flash). A host that points the DfuSe
 * address pointer here and downloads any payload requests a device reset —
 * the clean way to leave DFU mode without flashing (e.g. the SDK aborting an
 * update after its pre-flight downgrade check). No flash is touched: the
 * request is only latched here and the bootloader main loop performs the
 * reset after the host's final GETSTATUS handshake completes. SBSFU fully
 * re-verifies the slot on the way back up, so this can never launch an
 * unverified image. */
#define DFU_RESET_VIRT_ADDR     0xFFFFFF08U

/* USER CODE END PRIVATE_DEFINES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Macros
  * @brief Private macros.
  * @{
  */

/* USER CODE BEGIN PRIVATE_MACRO */

/* USER CODE END PRIVATE_MACRO */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Variables
  * @brief Private variables.
  * @{
  */

/* USER CODE BEGIN PRIVATE_VARIABLES */

/* USER CODE END PRIVATE_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Exported_Variables
  * @brief Public variables.
  * @{
  */

extern USBD_HandleTypeDef hUsbDeviceFS;

/* USER CODE BEGIN EXPORTED_VARIABLES */

/* USER CODE END EXPORTED_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_FunctionPrototypes
  * @brief Private functions declaration.
  * @{
  */

static uint16_t MEM_If_Init_FS(void);
static uint16_t MEM_If_Erase_FS(uint32_t Add);
static uint16_t MEM_If_Write_FS(uint8_t *src, uint8_t *dest, uint32_t Len);
static uint8_t *MEM_If_Read_FS(uint8_t *src, uint8_t *dest, uint32_t Len);
static uint16_t MEM_If_DeInit_FS(void);
static uint16_t MEM_If_GetStatus_FS(uint32_t Add, uint8_t Cmd, uint8_t *buffer);

/* USER CODE BEGIN PRIVATE_FUNCTIONS_DECLARATION */
static uint16_t dfu_read_slot_version(void);
/* USER CODE END PRIVATE_FUNCTIONS_DECLARATION */

/**
  * @}
  */

#if defined ( __ICCARM__ ) /* IAR Compiler */
  #pragma data_alignment=4
#endif
__ALIGN_BEGIN USBD_DFU_MediaTypeDef USBD_DFU_fops_FS __ALIGN_END =
{
   (uint8_t*)FLASH_DESC_STR,
    MEM_If_Init_FS,
    MEM_If_DeInit_FS,
    MEM_If_Erase_FS,
    MEM_If_Write_FS,
    MEM_If_Read_FS,
    MEM_If_GetStatus_FS
};

/* Private functions ---------------------------------------------------------*/
/**
  * @brief  Memory initialization routine.
  * @retval USBD_OK if operation is successful, MAL_FAIL else.
  */
uint16_t MEM_If_Init_FS(void)
{
  /* USER CODE BEGIN 0 */
  return (USBD_OK);
  /* USER CODE END 0 */
}

/**
  * @brief  De-Initializes Memory
  * @retval USBD_OK if operation is successful, MAL_FAIL else
  */
uint16_t MEM_If_DeInit_FS(void)
{
  /* USER CODE BEGIN 1 */
  return (USBD_OK);
  /* USER CODE END 1 */
}

/**
  * @brief  Erase page.
  * @param  Add: Address of page to be erased.
  * @retval USBD_OK if operation is successful, USBD_FAIL else.
  */
uint16_t MEM_If_Erase_FS(uint32_t Add)
{
  /* USER CODE BEGIN 2 */

  /* Reject erase outside the active application slot (protects the bootloader,
   * the anti-rollback log page and the user-config page). */
  if (Add < APP_FLASH_BASE || Add >= FLASH_END_ADDR)
  {
    return (USBD_FAIL);
  }

  /* Anti-rollback: latch the currently-installed version BEFORE any erase wipes
   * the active-slot header. Done once per DFU session, on the first erase (the
   * slot is still intact at this point regardless of which page erases first). */
  if (s_cur_ver_captured == 0U)
  {
    s_cur_ver_captured   = 1U;
    s_current_fw_version = dfu_read_slot_version();
  }

  FLASH_EraseInitTypeDef eraseInit = {0};
  uint32_t pageError = 0;

  eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
  eraseInit.Banks     = FLASH_BANK_1;
  eraseInit.Page      = (uint32_t)((Add - FLASH_BASE) / DFU_PAGE_SIZE);
  eraseInit.NbPages   = 1;

  HAL_FLASH_Unlock();
  HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&eraseInit, &pageError);
  HAL_FLASH_Lock();

  if (status == HAL_OK)
  {
    /* A new application image is being installed: clear the failsafe boot
     * counter (RTC->BKP6R) so the freshly flashed firmware gets a clean set of
     * boot attempts. Without this, if we entered DFU because the counter hit
     * BL_BOOT_FAIL_MAX, it would still be at the limit after the download and
     * the bootloader would refuse to launch the new image until a power cycle.
     * Backup-domain write access was already enabled in main(); re-assert DBP
     * defensively as this is a one-shot, low-cost operation. */
    HAL_PWR_EnableBkUpAccess();
    RTC->BKP6R = 0U;
    __DSB();
  }

  return (status == HAL_OK) ? USBD_OK : USBD_FAIL;

  /* USER CODE END 2 */
}

/**
  * @brief  Memory write routine.
  * @param  src:  Buffer containing data to program.
  * @param  dest: Target flash address.
  * @param  Len:  Number of bytes to write.
  * @retval USBD_OK if operation is successful, USBD_FAIL else.
  * @note   STM32L4 requires 64-bit (8-byte) doubleword flash writes.
  *         Partial final doublewords are padded with 0xFF.
  */
uint16_t MEM_If_Write_FS(uint8_t *src, uint8_t *dest, uint32_t Len)
{
  /* USER CODE BEGIN 3 */

  uint32_t addr = (uint32_t)dest;

  /* Virtual reset request: latch and ACK without touching flash. The main
   * loop resets after the host's status handshake completes. Deliberately
   * does NOT set s_dfu_image_written — no image state changes here. */
  if (addr == DFU_RESET_VIRT_ADDR)
  {
    s_dfu_reset_requested = 1U;
    return (USBD_OK);
  }

  /* Protect everything outside the active application slot */
  if (addr < APP_FLASH_BASE || Len > (FLASH_END_ADDR - addr) || addr >= FLASH_END_ADDR)
  {
    return (USBD_FAIL);
  }

  uint32_t remaining = Len;

  HAL_FLASH_Unlock();

  while (remaining > 0U)
  {
    uint64_t dword = 0xFFFFFFFFFFFFFFFFULL;
    uint32_t chunk = (remaining >= 8U) ? 8U : remaining;
    memcpy(&dword, src, chunk);   /* partial final doubleword stays 0xFF-padded */

    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dword) != HAL_OK)
    {
      HAL_FLASH_Lock();
      return (USBD_FAIL);
    }

    src       += chunk;
    addr      += 8U;
    remaining -= chunk;
  }

  HAL_FLASH_Lock();

  s_dfu_image_written = 1U;   /* a new image was programmed: arm post-manifest reboot */

  return (USBD_OK);

  /* USER CODE END 3 */
}

/**
  * @brief  Memory read routine.
  * @param  src: Pointer to the source buffer. Address to be written to.
  * @param  dest: Pointer to the destination buffer.
  * @param  Len: Number of data to be read (in bytes).
  * @retval Pointer to the physical address where data should be read.
  */
uint8_t *MEM_If_Read_FS(uint8_t *src, uint8_t *dest, uint32_t Len)
{
  /* Return a valid address to avoid HardFault */
  /* USER CODE BEGIN 4 */

  /* DFU "get version" command: an UPLOAD from the virtual address returns the
   * bootloader version string instead of reading flash. Reply with FW_VERSION,
   * null-padded to the requested length so the host can strip the padding. */
  if ((uint32_t)src == DFU_VERSION_VIRT_ADDR)
  {
    const char *ver = FW_VERSION;
    uint32_t ver_len = (uint32_t)strlen(ver);
    if (ver_len > DFU_VERSION_READ_LEN) { ver_len = DFU_VERSION_READ_LEN; }
    if (ver_len > Len)                  { ver_len = Len; }
    memset(dest, 0, Len);
    memcpy(dest, ver, ver_len);
    return dest;
  }

  /* Bound DFU UPLOAD/read to the same window as erase/write: the active
   * application slot only. Without this, a host could point the DfuSe address
   * pointer at any address and UPLOAD-read it, exfiltrating the bootloader and
   * the SE key material (se_key.s AES key in the SE key region), the
   * anti-rollback floor, the user-config page, or RAM. Reject anything outside
   * [APP_FLASH_BASE, FLASH_END_ADDR); the read range must also fit entirely
   * inside the window. The comparison against (FLASH_END_ADDR - addr) is written
   * to avoid the unsigned overflow that (addr + Len) would risk. Returning NULL
   * makes the DFU class STALL the packet and NAK the command (usbd_dfu.c). */
  {
    uint32_t addr = (uint32_t)src;

    if ((addr < APP_FLASH_BASE) ||
        (addr >= FLASH_END_ADDR) ||
        (Len  > (FLASH_END_ADDR - addr)))
    {
      return (NULL);
    }
  }

  memcpy(dest, src, Len);
  return dest;
  /* USER CODE END 4 */
}

/**
  * @brief  Get status routine
  * @param  Add: Address to be read from
  * @param  Cmd: Number of data to be read (in bytes)
  * @param  buffer: used for returning the time necessary for a program or an erase operation
  * @retval USBD_OK if operation is successful
  */
uint16_t MEM_If_GetStatus_FS(uint32_t Add, uint8_t Cmd, uint8_t *buffer)
{
  /* USER CODE BEGIN 5 */
  UNUSED(Add);

  uint32_t timeout_ms;
  switch (Cmd)
  {
    case DFU_MEDIA_PROGRAM:
      timeout_ms = 5U;     /* per write block */
      break;

    case DFU_MEDIA_ERASE:
      timeout_ms = 50U;    /* 2 KB page erase is ~25 ms max on STM32L4 */
      break;

    default:
      timeout_ms = 0U;
      break;
  }

  /* wPollTimeout: 24-bit little-endian poll interval in ms */
  buffer[1] = (uint8_t)(timeout_ms & 0xFFU);
  buffer[2] = (uint8_t)((timeout_ms >> 8U) & 0xFFU);
  buffer[3] = (uint8_t)((timeout_ms >> 16U) & 0xFFU);

  return (USBD_OK);
  /* USER CODE END 5 */
}

/* USER CODE BEGIN PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
  * @brief  Reports whether a DFU download has completed and the host has
  *         finished the manifestation phase (device back in dfuIDLE).
  * @note   The DFU class is configured manifestation-tolerant, so it does not
  *         self-reset in DFU_Leave(). The bootloader main loop polls this and
  *         issues the reboot into the freshly programmed image once the host has
  *         cleanly read final status — which is what removes dfu-util's
  *         "Error during download get_status".
  * @retval 1 once an image was written AND manifestation is complete, else 0.
  */
uint8_t DFU_ImageDownloadComplete(void)
{
  if (s_dfu_image_written == 0U)
  {
    return 0U;
  }

  const USBD_DFU_HandleTypeDef *hdfu =
      (const USBD_DFU_HandleTypeDef *)hUsbDeviceFS.pClassData;
  if (hdfu == NULL)
  {
    return 0U;
  }

  /* Manifestation is finished once DFU_Leave() has run, which sets manif_state
   * to COMPLETE. The host's terminal dev_state varies: dfu-util in DfuSe mode
   * reads status once (seeing dfuMANIFEST) and stops, leaving us in
   * MANIFEST_SYNC; a host that polls through to the end leaves us in dfuIDLE.
   * Accept either. The s_dfu_image_written gate keeps this from matching the
   * power-on default (IDLE + COMPLETE), and the write phase never sits in
   * MANIFEST_SYNC/IDLE (it cycles through the DNLOAD_* states), so neither can
   * trigger a premature reboot. */
  if (hdfu->manif_state != DFU_MANIFEST_COMPLETE)
  {
    return 0U;
  }

  return ((hdfu->dev_state == DFU_STATE_MANIFEST_SYNC) ||
          (hdfu->dev_state == DFU_STATE_IDLE)) ? 1U : 0U;
}

/**
  * @brief  Read the FwVersion from the SBSFU header at the active slot start.
  * @note   Header layout (see py-tools/sign_firmware.py): "SFU1" magic at 0x000,
  *         ProtocolVersion at 0x004, FwVersion (u16, little-endian) at 0x006.
  * @retval The installed FwVersion, or 0 if there is no valid "SFU1" header.
  */
static uint16_t dfu_read_slot_version(void)
{
  const uint8_t *hdr = (const uint8_t *)APP_FLASH_BASE;

  if ((hdr[0] != 'S') || (hdr[1] != 'F') || (hdr[2] != 'U') || (hdr[3] != '1'))
  {
    return 0U;   /* no valid installed image -> no rollback floor */
  }
  return (uint16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8));
}

/**
  * @brief  Anti-rollback check on the just-downloaded image.
  * @note   Compares the new image's FwVersion (read from its freshly written
  *         header) against the version that was installed at DFU entry. The
  *         version field is part of the ECDSA-signed header region, so an
  *         attacker cannot forge a higher value on a genuinely-signed old image;
  *         a forged-high but unsigned image is still rejected by SBSFU at boot.
  * @retval 1 if the new image is a downgrade (must be refused), else 0.
  */
uint8_t DFU_IsRollback(void)
{
  uint16_t new_ver = dfu_read_slot_version();

  if (new_ver == 0U)
  {
    /* No valid header written: not a "rollback" — SBSFU's signature check will
     * reject it at boot anyway. */
    return 0U;
  }
  return (new_ver < s_current_fw_version) ? 1U : 0U;
}

/**
  * @brief  Destroy the image in the active slot by erasing its header page.
  * @note   Used to make a rejected downgrade unbootable: without a valid header
  *         SBSFU refuses to launch it, so it can never run — even after a power
  *         cycle.
  */
void DFU_InvalidateImage(void)
{
  (void)MEM_If_Erase_FS(APP_FLASH_BASE);
}

/**
  * @brief  Clear the post-download state so the main loop stops acting on a
  *         completed download (used after a rollback has been handled).
  */
void DFU_ClearDownloadState(void)
{
  s_dfu_image_written = 0U;
}

/**
  * @brief  Reports whether the host requested a device reset via a DNLOAD to
  *         DFU_RESET_VIRT_ADDR. Polled by the bootloader main loop, which
  *         performs the actual NVIC_SystemReset (after a short delay so the
  *         host's final USB status transaction completes).
  * @retval 1 if a reset was requested, else 0.
  */
uint8_t DFU_ResetRequested(void)
{
  return s_dfu_reset_requested;
}

/* USER CODE END PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
  * @}
  */

/**
  * @}
  */
