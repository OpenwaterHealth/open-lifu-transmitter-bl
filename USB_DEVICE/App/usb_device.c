/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_device.c
  * @version        : v2.0_Cube
  * @brief          : This file implements the USB Device
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

#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_dfu.h"
#include "usbd_dfu_if.h"
#include "utils.h"   /* delay_ms (tick-free) */

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

/* USB Device Core handle declaration. */
USBD_HandleTypeDef hUsbDeviceFS;
extern USBD_DescriptorsTypeDef FS_Desc;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * De-Initialize USB device Library, stop activity and disable peripheral.
  * Used when no USB host was detected and the bootloader falls back to the
  * I2C DFU transport (see Core/Src/main.c).
  * @retval None
  */
void MX_USB_DEVICE_DeInit(void)
{
  /* USER CODE BEGIN USB_DEVICE_DeInit_PreTreatment */

  /* USER CODE END USB_DEVICE_DeInit_PreTreatment */

  (void)USBD_Stop(&hUsbDeviceFS);

  /* USBD_Stop clears the D+ pull-up (BCDR.DPPU). Give a (possibly present but
   * silent) host ~20 ms to register the disconnect while the USB clock is
   * still running. delay_ms() is a tick-free CPU busy-loop (utils.c): this
   * runs on the DFU fallback path where the HAL TIM6 tick may be stalled, so
   * HAL_Delay() must not be used here. */
  delay_ms(20U);

  (void)USBD_DeInit(&hUsbDeviceFS);

  /* RCC-level hardware reset of the USB peripheral: clears every USB register
   * (CNTR, ISTR, DADDR, BTABLE, endpoint registers, BCDR, ...) back to
   * power-on values so no DFU session state lingers. */
  __HAL_RCC_USB_FORCE_RESET();
  __HAL_RCC_USB_RELEASE_RESET();

  /* Disable USB interrupt and clock so no USB activity disturbs I2C DFU mode */
  HAL_NVIC_DisableIRQ(USB_IRQn);
  __HAL_RCC_USB_CLK_DISABLE();

  /* USER CODE BEGIN USB_DEVICE_DeInit_PostTreatment */

  /* USER CODE END USB_DEVICE_DeInit_PostTreatment */
}

/**
  * Init USB device Library, add supported class and start the library
  * @retval None
  */
void MX_USB_DEVICE_Init(void)
{
  /* USER CODE BEGIN USB_DEVICE_Init_PreTreatment */

  /* USER CODE END USB_DEVICE_Init_PreTreatment */

  /* Init Device Library, add supported class and start the library. */
  if (USBD_Init(&hUsbDeviceFS, &FS_Desc, DEVICE_FS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_DFU) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_DFU_RegisterMedia(&hUsbDeviceFS, &USBD_DFU_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB_DEVICE_Init_PostTreatment */

  /* USER CODE END USB_DEVICE_Init_PostTreatment */
}

/**
  * @}
  */

/**
  * @}
  */

