/**
 * @file dfu_guard.c
 * @brief Pre-erase header check shared by the USB and I2C DFU transports.
 *        See dfu_guard.h for the scheme.
 */
#include "dfu_guard.h"
#include "main.h"
#include "memory_map.h"
#include "anti_rollback.h"            /* AntiRollback_GetFloor */
#include "se_def_metadata.h"          /* SE_FwRawHeaderTypeDef, SE_FW_HEADER_TOT_LEN */
#include "se_interface_bootloader.h"  /* SE_VerifyHeaderSignature */
#include "sfu_fwimg_regions.h"        /* SFU_IMG_IMAGE_OFFSET */
#include <string.h>

#define GUARD_BASE        MEM_DFU_WRITABLE_BASE                       /* slot start / header */
#define GUARD_END         MEM_DFU_WRITABLE_END                        /* exclusive */
#define GUARD_PAGE_SIZE   MEM_PAGE_SIZE
#define GUARD_NUM_PAGES   ((GUARD_END - GUARD_BASE) / GUARD_PAGE_SIZE) /* 94 */
#define GUARD_MASK_WORDS  ((GUARD_NUM_PAGES + 31U) / 32U)

extern IWDG_HandleTypeDef hiwdg;

static uint8_t  s_header_accepted = 0U;
static uint32_t s_pending[GUARD_MASK_WORDS];
static uint8_t  s_min_ver_latched = 0U;
static uint16_t s_min_version     = 0U;
/* Copy of the received header: the Secure Engine reads it from SBSFU RAM. */
static SE_FwRawHeaderTypeDef s_header;

static uint32_t page_index(uint32_t addr)
{
  return (addr - GUARD_BASE) / GUARD_PAGE_SIZE;
}

static uint8_t in_window(uint32_t addr)
{
  return ((addr >= GUARD_BASE) && (addr < GUARD_END)) ? 1U : 0U;
}

static void set_pending(uint32_t idx)
{
  s_pending[idx / 32U] |= (1UL << (idx % 32U));
}

static void clear_pending(uint32_t idx)
{
  s_pending[idx / 32U] &= ~(1UL << (idx % 32U));
}

static uint8_t is_pending(uint32_t idx)
{
  return ((s_pending[idx / 32U] & (1UL << (idx % 32U))) != 0U) ? 1U : 0U;
}

/* Installed slot version (unverified read of the header in flash), or 0. */
static uint16_t installed_version(void)
{
  const uint8_t *hdr = (const uint8_t *)GUARD_BASE;

  if ((hdr[0] != 'S') || (hdr[1] != 'F') || (hdr[2] != 'U') || (hdr[3] != '1'))
  {
    return 0U;
  }
  return (uint16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8));
}

/* Latch the downgrade floor before anything can change the slot. */
static void latch_min_version(void)
{
  if (s_min_ver_latched == 0U)
  {
    uint32_t floor = AntiRollback_GetFloor();
    uint16_t inst  = installed_version();

    if (floor > 0xFFFFU)
    {
      floor = 0xFFFFU;   /* FwVersion is 16 bits: nothing can pass a larger floor */
    }
    s_min_version     = (floor > (uint32_t)inst) ? (uint16_t)floor : inst;
    s_min_ver_latched = 1U;
  }
}

/* 0 = header acceptable, 1 = refused. */
static uint8_t check_header(const SE_FwRawHeaderTypeDef *p_hdr)
{
  SE_StatusTypeDef se_status = SE_KO;
  const uint8_t   *magic     = (const uint8_t *)p_hdr->SFUMagic;

  if ((magic[0] != 'S') || (magic[1] != 'F') || (magic[2] != 'U') || (magic[3] != '1'))
  {
    return 1U;
  }
  if (SE_VerifyHeaderSignature(&se_status, (SE_FwRawHeaderTypeDef *)p_hdr) != SE_SUCCESS)
  {
    return 1U;    /* wrong key (other board, other trust domain) or tampered */
  }
  /* The fields below are authenticated from here on. */
  if ((p_hdr->FwVersion == 0U) || (p_hdr->FwVersion < s_min_version))
  {
    return 1U;    /* downgrade: SBSFU would refuse it at boot */
  }
  if ((p_hdr->FwSize == 0U) ||
      (p_hdr->FwSize > ((GUARD_END - GUARD_BASE) - SFU_IMG_IMAGE_OFFSET)))
  {
    return 1U;    /* does not fit the slot */
  }
  return 0U;
}

void DFU_Guard_StartSession(void)
{
  s_header_accepted = 0U;
  (void)memset(s_pending, 0, sizeof(s_pending));
}

DFU_GuardStatus DFU_Guard_ErasePageNow(uint32_t page_addr)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t page_error = 0U;
  uint8_t  was_locked = (READ_BIT(FLASH->CR, FLASH_CR_LOCK) != 0U) ? 1U : 0U;
  HAL_StatusTypeDef status;

  if (in_window(page_addr) == 0U)
  {
    return DFU_GUARD_FLASH_ERR;
  }
  latch_min_version();

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks     = FLASH_BANK_1;
  erase.Page      = (page_addr - MEM_FLASH_BASE) / GUARD_PAGE_SIZE;
  erase.NbPages   = 1U;

  (void)HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_WRPERR | FLASH_FLAG_PROGERR);
  status = HAL_FLASHEx_Erase(&erase, &page_error);
  if (was_locked != 0U)
  {
    (void)HAL_FLASH_Lock();
  }
  if (status != HAL_OK)
  {
    return DFU_GUARD_FLASH_ERR;
  }

  clear_pending(page_index(page_addr));
  /* A new image is being installed: clear the failsafe boot counter so it gets
   * a clean set of boot attempts (see BL_BOOT_FAIL_MAX in main.c). */
  HAL_PWR_EnableBkUpAccess();
  RTC->BKP6R = 0U;
  __DSB();
  return DFU_GUARD_OK;
}

DFU_GuardStatus DFU_Guard_RequestErase(uint32_t page_addr)
{
  uint32_t idx;

  if (in_window(page_addr) == 0U)
  {
    return DFU_GUARD_FLASH_ERR;
  }
  latch_min_version();
  idx = page_index(page_addr);

  /* Erasing the header page starts a new download: check its header again. */
  if (idx == 0U)
  {
    s_header_accepted = 0U;
  }
  if (s_header_accepted == 0U)
  {
    set_pending(idx);
    return DFU_GUARD_OK;
  }
  return DFU_Guard_ErasePageNow(GUARD_BASE + (idx * GUARD_PAGE_SIZE));
}

DFU_GuardStatus DFU_Guard_PrepareWrite(const uint8_t *src, uint32_t addr, uint32_t len)
{
  uint32_t idx;
  uint32_t last;

  if ((len == 0U) || (in_window(addr) == 0U) || (len > (GUARD_END - addr)))
  {
    return DFU_GUARD_REFUSED;
  }

  if (s_header_accepted == 0U)
  {
    if ((addr != GUARD_BASE) || (len < SE_FW_HEADER_TOT_LEN))
    {
      return DFU_GUARD_REFUSED;   /* a download must start with its header */
    }
    latch_min_version();
    (void)memcpy(&s_header, src, sizeof(s_header));
    if (check_header(&s_header) != 0U)
    {
      return DFU_GUARD_REFUSED;   /* the installed image is untouched */
    }
    s_header_accepted = 1U;
  }

  last = page_index(addr + len - 1U);
  for (idx = page_index(addr); idx <= last; idx++)
  {
    if ((is_pending(idx) != 0U) &&
        (DFU_Guard_ErasePageNow(GUARD_BASE + (idx * GUARD_PAGE_SIZE)) != DFU_GUARD_OK))
    {
      return DFU_GUARD_FLASH_ERR;
    }
  }
  return DFU_GUARD_OK;
}

DFU_GuardStatus DFU_Guard_ErasePendingPages(void)
{
  uint32_t idx;

  if (s_header_accepted == 0U)
  {
    (void)memset(s_pending, 0, sizeof(s_pending));   /* refused/aborted: keep the image */
    return DFU_GUARD_OK;
  }
  for (idx = 0U; idx < GUARD_NUM_PAGES; idx++)
  {
    if (is_pending(idx) != 0U)
    {
      if (DFU_Guard_ErasePageNow(GUARD_BASE + (idx * GUARD_PAGE_SIZE)) != DFU_GUARD_OK)
      {
        return DFU_GUARD_FLASH_ERR;
      }
      (void)HAL_IWDG_Refresh(&hiwdg);
    }
  }
  return DFU_GUARD_OK;
}

uint8_t DFU_Guard_HasPendingPages(void)
{
  uint32_t w;

  if (s_header_accepted == 0U)
  {
    return 0U;
  }
  for (w = 0U; w < GUARD_MASK_WORDS; w++)
  {
    if (s_pending[w] != 0U)
    {
      return 1U;
    }
  }
  return 0U;
}

uint8_t DFU_Guard_IsErasePending(uint32_t addr)
{
  return (in_window(addr) != 0U) ? is_pending(page_index(addr)) : 0U;
}

uint8_t DFU_Guard_HeaderAccepted(void)
{
  return s_header_accepted;
}

uint16_t DFU_Guard_MinVersion(void)
{
  latch_min_version();
  return s_min_version;
}
