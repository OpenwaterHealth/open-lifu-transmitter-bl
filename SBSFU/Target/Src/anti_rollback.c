/**
  ******************************************************************************
  * @file    anti_rollback.c
  * @brief   Persistent monotonic firmware anti-rollback floor (flash-backed).
  *
  * Storage: an append-only log of version entries in flash page 126
  * (0x0803F000, 2 KB — MEM_ANTIROLLBACK_BASE in Core/Inc/memory_map.h). This
  * page sits above the bootloader's DFU writable window (MEM_DFU_WRITABLE_END
  * in usbd_dfu_if.c / i2c_dfu_if.c), so an update over USB or I2C DFU cannot
  * erase or overwrite it.
  *
  * Each log entry is one STM32L4 flash doubleword (8 bytes, the minimum
  * programmable unit, programmable once after erase):
  *     word[0] = version
  *     word[1] = ~version           (integrity check)
  * A free entry reads all-0xFF; a valid entry has word[1] == ~word[0]. The floor
  * is the maximum version across all valid entries, so it only ever increases.
  *
  * Updating the floor appends a new entry to the first free slot — no erase in
  * the common case. The page is erased only if it is full (after 256 distinct
  * version increases) or contains stale/garbage data (e.g. never provisioned to
  * the erased state), in which case the new entry is written at slot 0. The new
  * version is >= every prior valid entry, so erasing loses no floor information.
  ******************************************************************************
  */

#include "main.h"          /* HAL (flash) */
#include "memory_map.h"
#include "anti_rollback.h"

/* Flash page 126. MUST stay outside the DFU writable window
 * (usbd_dfu_if.c: APP_FLASH_BASE .. FLASH_END_ADDR) so DFU cannot erase it. */
#define AR_PAGE_BASE        MEM_ANTIROLLBACK_BASE            /* 0x0803F000 */
#define AR_PAGE_SIZE        MEM_ANTIROLLBACK_SIZE            /* 2 KB */
#define AR_PAGE_INDEX       ((AR_PAGE_BASE - MEM_FLASH_BASE) / MEM_PAGE_SIZE)  /* 126 */

#define AR_ENTRY_SIZE       8U                 /* one STM32L4 flash doubleword */
#define AR_WORDS_PER_ENTRY  (AR_ENTRY_SIZE / 4U)
#define AR_NUM_ENTRIES      (AR_PAGE_SIZE / AR_ENTRY_SIZE)   /* 256 */
#define AR_ERASED_U32       0xFFFFFFFFU

/**
  * @brief  Single pass over the log: report the floor (max valid version) and
  *         the index of the first free (erased) slot.
  * @param  p_floor    [out] highest valid version found, 0 if none.
  * @param  p_free_idx [out] first free slot index, or AR_NUM_ENTRIES if full.
  */
static void ar_scan(uint32_t *p_floor, uint32_t *p_free_idx)
{
  const volatile uint32_t *base = (const volatile uint32_t *)AR_PAGE_BASE;
  uint32_t maxv = 0U;
  uint32_t free_idx = AR_NUM_ENTRIES;

  for (uint32_t i = 0U; i < AR_NUM_ENTRIES; i++)
  {
    const volatile uint32_t *e = &base[i * AR_WORDS_PER_ENTRY];
    uint32_t v   = e[0];
    uint32_t inv = e[1];

    if ((v == AR_ERASED_U32) && (inv == AR_ERASED_U32))
    {
      if (free_idx == AR_NUM_ENTRIES)
      {
        free_idx = i;                 /* first free slot */
      }
    }
    else if ((v != AR_ERASED_U32) && (inv == (uint32_t)(~v)))
    {
      if (v > maxv)
      {
        maxv = v;                     /* valid entry */
      }
    }
    /* else: stale/garbage entry -> ignored (neither free nor valid) */
  }

  *p_floor    = maxv;
  *p_free_idx = free_idx;
}

/**
  * @brief  Erase the floor page (page 126, bank 1).
  */
static void ar_erase_page(void)
{
  FLASH_EraseInitTypeDef e = {0};
  uint32_t page_error = 0U;

  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Banks     = FLASH_BANK_1;
  e.Page      = AR_PAGE_INDEX;
  e.NbPages   = 1U;

  HAL_FLASH_Unlock();
  (void)HAL_FLASHEx_Erase(&e, &page_error);
  HAL_FLASH_Lock();
}

/**
  * @brief  Program one log entry (one flash doubleword) at slot @p idx.
  */
static void ar_program_entry(uint32_t idx, uint32_t version)
{
  uint64_t entry = ((uint64_t)(uint32_t)(~version) << 32) | (uint64_t)version;

  HAL_FLASH_Unlock();
  (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          AR_PAGE_BASE + (idx * AR_ENTRY_SIZE),
                          entry);
  HAL_FLASH_Lock();
}

uint32_t AntiRollback_GetFloor(void)
{
  uint32_t floor;
  uint32_t free_idx;

  ar_scan(&floor, &free_idx);
  return floor;
}

void AntiRollback_UpdateFloor(uint32_t version)
{
  uint32_t floor;
  uint32_t free_idx;

  ar_scan(&floor, &free_idx);

  if (version <= floor)
  {
    return;                           /* floor already at/above this version */
  }

  if (free_idx >= AR_NUM_ENTRIES)
  {
    /* Log full (or stale/garbage): reclaim the page. The new version is >=
     * every prior valid entry, so no floor information is lost. */
    ar_erase_page();
    free_idx = 0U;
  }

  ar_program_entry(free_idx, version);
}
