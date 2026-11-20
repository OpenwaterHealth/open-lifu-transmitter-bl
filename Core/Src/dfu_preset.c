/**
 * @file dfu_preset.c
 * @brief Signed preset writes into the preset slots, shared by the USB and the
 *        I2C DFU transports. See dfu_preset.h for the scheme.
 */
#include "dfu_preset.h"
#include "main.h"
#include "memory_map.h"
#include "se_def_metadata.h"          /* SE_FwRawHeaderTypeDef */
#include "sfu_fwimg_services.h"       /* SFU_IMG_VerifyPresetHeader / _Body */
#include <string.h>

#define PRESET_HDR_LEN  ((uint32_t)sizeof(SE_FwRawHeaderTypeDef))
#define NO_SLOT         0xFFU

_Static_assert(sizeof(SE_FwRawHeaderTypeDef) == MEM_PRESET_BODY_OFFSET,
               "the preset blob starts right after its header");

extern IWDG_HandleTypeDef hiwdg;

static uint8_t  s_staged    = 0U;
static uint8_t  s_slot      = NO_SLOT;   /* slot the staged header is bound to */
static uint32_t s_body_size = 0U;
/* The staged header: word-aligned for the Secure Engine, which reads it here. */
static SE_FwRawHeaderTypeDef s_hdr;

static uint32_t slot_base(uint8_t slot)
{
  return MEM_PRESET_SLOT_BASE(slot);
}

static uint32_t rd_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void unstage(void)
{
  s_staged    = 0U;
  s_slot      = NO_SLOT;
  s_body_size = 0U;
}

/* Erase both pages of a slot. Preserves the caller's flash LOCK state. */
static DFU_PresetStatus erase_slot(uint8_t slot)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t page_error = 0U;
  uint8_t  was_locked = (READ_BIT(FLASH->CR, FLASH_CR_LOCK) != 0U) ? 1U : 0U;
  HAL_StatusTypeDef status;

  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.Banks     = FLASH_BANK_1;
  erase.Page      = (slot_base(slot) - MEM_FLASH_BASE) / MEM_PAGE_SIZE;
  erase.NbPages   = MEM_PRESET_SLOT_SIZE / MEM_PAGE_SIZE;

  (void)HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_WRPERR | FLASH_FLAG_PROGERR);
  status = HAL_FLASHEx_Erase(&erase, &page_error);
  if (was_locked != 0U)
  {
    (void)HAL_FLASH_Lock();
  }
  (void)HAL_IWDG_Refresh(&hiwdg);
  return (status == HAL_OK) ? DFU_PRESET_OK : DFU_PRESET_FLASH_ERR;
}

/* Program doublewords, a short last one 0xFF-padded, reading each back. */
static DFU_PresetStatus program(const uint8_t *src, uint32_t addr, uint32_t len)
{
  uint8_t  was_locked = (READ_BIT(FLASH->CR, FLASH_CR_LOCK) != 0U) ? 1U : 0U;
  DFU_PresetStatus result = DFU_PRESET_OK;

  (void)HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_WRPERR | FLASH_FLAG_PROGERR);
  for (uint32_t i = 0U; (i < len) && (result == DFU_PRESET_OK); i += 8U)
  {
    uint64_t dw = 0xFFFFFFFFFFFFFFFFULL;
    uint32_t n  = ((len - i) >= 8U) ? 8U : (len - i);

    (void)memcpy(&dw, &src[i], n);
    if ((HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) != HAL_OK) ||
        (*(volatile uint64_t *)(addr + i) != dw))
    {
      result = DFU_PRESET_FLASH_ERR;
    }
  }
  if (was_locked != 0U)
  {
    (void)HAL_FLASH_Lock();
  }
  return result;
}

/* The block's bytes must be the staged header's where they overlap it, and
 * 0xFF past the end of the blob. */
static uint8_t block_matches(const uint8_t *src, uint32_t addr, uint32_t len)
{
  const uint32_t base = slot_base(s_slot);
  const uint32_t end  = base + MEM_PRESET_BODY_OFFSET + s_body_size;
  const uint8_t *hdr  = (const uint8_t *)&s_hdr;

  for (uint32_t i = 0U; i < len; i++)
  {
    uint32_t a = addr + i;
    if ((a < (base + PRESET_HDR_LEN)) && (src[i] != hdr[a - base]))
    {
      return 0U;
    }
    if ((a >= end) && (src[i] != 0xFFU))
    {
      return 0U;
    }
  }
  return 1U;
}

void DFU_Preset_StartSession(void)
{
  unstage();
}

uint8_t DFU_Preset_InRegion(uint32_t addr)
{
  return ((addr >= MEM_PRESET_BASE) && (addr < MEM_PRESET_END)) ? 1U : 0U;
}

uint8_t DFU_Preset_Staged(void)
{
  return s_staged;
}

uint8_t DFU_Preset_Completes(uint32_t addr, uint32_t len)
{
  if ((s_staged == 0U) || (s_slot == NO_SLOT))
  {
    return 0U;
  }
  return ((addr + len) >= (slot_base(s_slot) + MEM_PRESET_BODY_OFFSET + s_body_size)) ? 1U : 0U;
}

DFU_PresetStatus DFU_Preset_Stage(const uint8_t *src, uint32_t len)
{
  unstage();
  if ((src == NULL) || (len < PRESET_HDR_LEN))
  {
    return DFU_PRESET_REFUSED;
  }
  (void)memcpy(&s_hdr, src, PRESET_HDR_LEN);

  /* FwSize (u32 at 0x008) is the blob size: it must fit a slot after the header. */
  uint32_t body_size = rd_u32(&src[8]);
  if ((body_size == 0U) || (body_size > MEM_PRESET_BODY_MAX))
  {
    return DFU_PRESET_REFUSED;
  }
  if (SFU_IMG_VerifyPresetHeader((const uint8_t *)&s_hdr, PRESET_HDR_LEN) != SFU_SUCCESS)
  {
    return DFU_PRESET_REFUSED;   /* not PRS1, wrong key (other board or domain) or tampered */
  }
  s_body_size = body_size;
  s_staged    = 1U;
  return DFU_PRESET_OK;
}

DFU_PresetStatus DFU_Preset_EraseSlot(uint32_t addr)
{
  if ((s_staged == 0U) || (DFU_Preset_InRegion(addr) == 0U))
  {
    return DFU_PRESET_REFUSED;
  }
  uint8_t slot = (uint8_t)((addr - MEM_PRESET_BASE) / MEM_PRESET_SLOT_SIZE);
  if ((s_slot != NO_SLOT) && (s_slot != slot))
  {
    return DFU_PRESET_REFUSED;   /* one staged header, one slot */
  }
  s_slot = slot;
  return erase_slot(slot);
}

DFU_PresetStatus DFU_Preset_Write(const uint8_t *src, uint32_t addr, uint32_t len)
{
  if ((s_staged == 0U) || (s_slot == NO_SLOT) || (len == 0U) || ((addr & 7U) != 0U))
  {
    return DFU_PRESET_REFUSED;
  }
  const uint32_t base = slot_base(s_slot);
  const uint32_t end  = base + MEM_PRESET_BODY_OFFSET + s_body_size;
  if ((addr < base) || (addr >= (base + MEM_PRESET_SLOT_SIZE)) ||
      (len > ((base + MEM_PRESET_SLOT_SIZE) - addr)))
  {
    return DFU_PRESET_REFUSED;   /* outside the slot the header is bound to */
  }
  if (block_matches(src, addr, len) == 0U)
  {
    return DFU_PRESET_REFUSED;
  }
  DFU_PresetStatus result = program(src, addr, len);
  if (result != DFU_PRESET_OK)
  {
    return result;
  }

  /* The block that completes the blob: check what is now in the slot. The
   * header in flash must be the staged one, whose signature already verified,
   * so only the blob's hash is left to check. */
  if ((addr + len) >= end)
  {
    uint8_t slot = s_slot;
    if ((memcmp((const void *)base, &s_hdr, PRESET_HDR_LEN) != 0) ||
        (SFU_IMG_VerifyPresetBody((const uint8_t *)&s_hdr, PRESET_HDR_LEN,
                                  base + MEM_PRESET_BODY_OFFSET) != SFU_SUCCESS))
    {
      (void)erase_slot(slot);
      result = DFU_PRESET_REFUSED;
    }
    unstage();   /* one stage, one slot write */
  }
  return result;
}

void DFU_Preset_Caps(uint8_t out[DFU_CAPS_LEN])
{
  const uint32_t caps = BL_CAPS;

  (void)memcpy(&out[0], DFU_CAPS_MAGIC, 4U);
  out[4] = (uint8_t)(caps & 0xFFU);
  out[5] = (uint8_t)((caps >> 8) & 0xFFU);
  out[6] = (uint8_t)((caps >> 16) & 0xFFU);
  out[7] = (uint8_t)((caps >> 24) & 0xFFU);
}
