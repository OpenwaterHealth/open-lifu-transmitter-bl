/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "common.h"
#include "memory_map.h"

#ifdef DEBUG_ENABLED
#include "logging.h"
#endif

#include "utils.h"

#include "sfu_boot.h"     /* SFU_BOOT_RunSecureBootService */
#include "usbd_dfu_if.h"  /* DFU_ImageDownloadComplete / rollback helpers */
#include "i2c_dfu_if.h"   /* I2C slave DFU transport (no USB host present) */
#include "ow_backchannel.h" /* Phase 2 one-wire discovery (Release build only) */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>


/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* Application -> bootloader "enter USB DFU" request.
 * The running application writes BL_FORCE_DFU_MAGIC into RTC backup register 7
 * and issues a system reset. RTC backup registers live in the always-on backup
 * domain and survive a software reset, so the bootloader can detect the request
 * on the next boot, skip launching the application, and enter the USB DFU
 * download path so a new signed image can be installed without a debugger.
 * Application side:
 *     __HAL_RCC_PWR_CLK_ENABLE();
 *     HAL_PWR_EnableBkUpAccess();
 *     __HAL_RCC_RTC_ENABLE();
 *     RTC->BKP7R = BL_FORCE_DFU_MAGIC;
 *     NVIC_SystemReset();
 */
#define BL_FORCE_DFU_MAGIC      0xB007C0DEU   /* "BOOT CODE" — request marker, RTC->BKP7R */

/*
 * Failsafe boot counter (RTC->BKP6R).
 * The bootloader increments this on every boot attempt BEFORE launching the
 * application, and arms the IWDG just before the jump. The application must
 * clear it to 0 once it has booted successfully. If the application hangs it
 * never clears the counter and never refreshes the IWDG, so the watchdog resets
 * the device and the count keeps rising across attempts. After BL_BOOT_FAIL_MAX
 * failed attempts the bootloader stops trying to launch and falls through to
 * USB DFU so a known-good image can be flashed. A clean power cycle (no VBAT)
 * clears the backup domain and resets the count.
 */
#define BL_BOOT_FAIL_MAX        3U            /* force DFU after this many failed boots */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CRC_HandleTypeDef hcrc;

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;

IWDG_HandleTypeDef hiwdg;

RTC_HandleTypeDef hrtc;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart2_rx;
DMA_HandleTypeDef hdma_usart2_tx;

/* USER CODE BEGIN PV */
I2C_HandleTypeDef* GLOBAL_I2C_DEVICE = NULL;

/* Set true by the USB PCD resume callback (usbd_conf.c) when a host actively
 * drives the bus — the "USB host present" signal for DFU transport selection. */
volatile bool isUSBConnected = false;

extern USBD_HandleTypeDef hUsbDeviceFS;   /* defined in usb_device.c */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_CRC_Init(void);
static void MX_IWDG_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint8_t force_dfu = 0U;   /* set when the application requested USB DFU (see RTC->BKP7R) */
  uint8_t boot_fail = 0U;   /* set when too many failed boot attempts forced DFU (see RTC->BKP6R) */
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /*
   * Application-requested USB DFU.
   * RTC backup registers reside in the always-on backup domain and are NOT
   * cleared by a software reset, so an application can hand control back to the
   * bootloader's DFU path by writing BL_FORCE_DFU_MAGIC to RTC->BKP7R and
   * resetting. Enable backup-domain write access (PWR DBP) and the RTC before
   * touching the backup register. RTCEN/RTCSEL programmed by a prior boot
   * persist across the software reset, so the register reads back the value the
   * application stored.
   */
  __HAL_RCC_PWR_CLK_ENABLE();   /* PWR is APB1-clocked on STM32L4 */
  HAL_PWR_EnableBkUpAccess();   /* DBP = 1: unlock the backup domain */
  /* Ensure the RTC kernel clock (LSI) is running. LSION in RCC_CSR is cleared
   * by a system reset even though the backup-domain BDCR (RTCSEL/RTCEN) is not,
   * so re-enable it before accessing the RTC. (SystemClock_Config also turns it
   * on; this keeps the sequence self-contained.) */
  __HAL_RCC_LSI_ENABLE();
  { uint32_t to = 0U; while ((__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == 0U) && (++to < 0x00100000U)) { } }
  /* Select LSI as the RTC clock only if no source is selected yet. Writing
   * RTCSEL when it already holds a (non-zero) value would require a backup-
   * domain reset, which would erase the request, so we never do that here.
   * (HAL_RTC_MspInit also selects LSI, so the sources always agree.) */
  if ((RCC->BDCR & RCC_BDCR_RTCSEL) == 0U)
  {
    MODIFY_REG(RCC->BDCR, RCC_BDCR_RTCSEL, RCC_BDCR_RTCSEL_1);   /* RTCSEL = LSI */
  }
  __HAL_RCC_RTC_ENABLE();       /* enable RTC register / backup-register access */
#if defined(__HAL_RCC_RTCAPB_CLK_ENABLE)
  __HAL_RCC_RTCAPB_CLK_ENABLE();
#endif
  if (RTC->BKP7R == BL_FORCE_DFU_MAGIC)
  {
    RTC->BKP7R = 0U;            /* consume the request (one-shot) */
    force_dfu = 1U;
  }

  /*
   * Failsafe boot counter (RTC->BKP6R). Only evaluated when the application did
   * NOT explicitly request DFU above — a deliberate DFU request is not a boot
   * failure and must not advance the counter. The counter is incremented here,
   * before the IWDG is armed and before the application is launched; the
   * application clears it to 0 once it boots successfully. See BL_BOOT_FAIL_MAX.
   */
  if (force_dfu == 0U)
  {
    uint32_t boot_cnt = RTC->BKP6R;
    if (boot_cnt >= BL_BOOT_FAIL_MAX)
    {
      /* The last BL_BOOT_FAIL_MAX attempts all failed to clear the counter:
       * the installed firmware is not booting. Recover via USB DFU instead of
       * launching it again. Leave the counter as-is so it can still be observed
       * that a boot-failure recovery occurred. */
      force_dfu = 1U;
      boot_fail = 1U;
    }
    else
    {
      RTC->BKP6R = boot_cnt + 1U;   /* record this boot attempt before the jump */
    }
  }

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART2_UART_Init();
  MX_CRC_Init();
  /* USER CODE BEGIN 2 */
#ifdef DEBUG_ENABLED
  init_dma_logging();
#endif

#ifdef DEBUG_ENABLED
  printf("LIFU Transmitter Bootloader\r\n");
  printf("VER: %s (%s)\r\n", FW_VERSION_STRING, FW_SHA_STRING);
  printf("Date: %s\r\n", FW_BUILD_TIME_STRING);
#endif

  FW_DEBUG("boot start\r\n");

  /*
   * Arm the independent watchdog before running the secure boot service.
   *
   * The IWDG runs from the LSI and, once started, keeps running across the
   * reset it triggers (only a power-on reset stops it). So on a retry boot the
   * bootloader is already running under a live IWDG: HAL_IWDG_Init() reloads
   * the counter, giving the bootloader a fresh ~8 s window for its
   * (un-refreshed) signature verification; the application must take over
   * refreshing once launched. The DFU download loop below also refreshes it.
   */
  MX_IWDG_Init();

  /*
   * Run the SBSFU Secure Boot Service — skipped when the application explicitly
   * requested DFU mode (force_dfu), in which case we fall through to the USB
   * DFU download path below.
   *
   * This initializes the Secure Engine (SECoreBin), configures security, and
   * runs the SBSFU state machine:
   *  - If a valid signed firmware is found at SLOT 1 (0x08010000), SBSFU
   *    verifies and launches it — this function never returns in that case.
   *  - If no valid firmware exists (SECBOOT_USE_NO_LOADER), the state machine
   *    exits gracefully and this function returns, allowing the USB DFU path
   *    below to accept a new signed image.
   *
   * SBSFU trace output goes through the same logging layer as printf (see the
   * SFU_LL_UART_* stubs in SBSFU/Target/Src/sfu_low_level.c), so the console
   * (USART2 + DMA) stays alive across the service call.
   */
  if (force_dfu == 0U)
  {
    SFU_BOOT_InitErrorTypeDef e_boot = SFU_BOOT_RunSecureBootService();
    (void)e_boot;  /* non-zero only on critical SE/security-IP init failure */
  }

  /*
   * Execution only reaches this point when no valid signed application will run
   * (SFU_BOOT_RunSecureBootService() launches a valid image and never returns).
   *
   * The inter-board READY line (PA1) is held LOW by MX_GPIO_Init while we are in
   * the bootloader. We deliberately KEEP it low through the USB-detection window
   * below so the master's WaitForAllSlavesReady() waits for this node instead of
   * enumerating past it. It is released to Hi-Z inside each transport branch:
   *   - I2C (slave) path: after OW_BL_Init() has armed the back-channel, so the
   *     node is already listening for discovery when the master starts its walk
   *     (Release build). In Debug there is no back-channel, so the release simply
   *     lets the master skip a DFU-stuck slave.
   *   - USB path: immediately (this node has a host and is being programmed).
   * Bounded either way (~2 s max), so a DFU node never hangs the master forever.
   */

#ifdef DEBUG_ENABLED
  if (force_dfu != 0U)
  {
    printf("[BL] firmware update requested - entering USB DFU download mode\r\n");
    if (boot_fail != 0U)
    {
      printf("[BL] reason: application failed to boot after repeated attempts (boot-counter recovery)\r\n");
    }
  }
#else
  (void)boot_fail;
#endif

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  MX_USB_DEVICE_Init();

  /*
   * DFU transport selection: USB when a host is present, otherwise I2C.
   *
   * "USB host present" is detected from real bus activity, exactly like the
   * legacy (non-secure) bootloader: the PCD resume callback sets
   * isUSBConnected when a host drives the bus, and dev_state advances past
   * DEFAULT once enumeration starts. Poll for ~2 s for enumeration to begin,
   * then a settle delay, and branch on isUSBConnected. With no USB data lines
   * (a slave) neither fires, and the bootloader falls back to listening as an
   * I2C DFU slave at DFU_I2C_ADDRESS (0x72).
   *
   * IMPORTANT: every delay on this path uses delay_ms() (a CPU busy-loop in
   * utils.c) and iteration counts, NOT HAL_Delay()/HAL_GetTick(). On this DFU
   * fallback path the HAL TIM6 tick timebase is not reliably advancing (the
   * app-requested reset path can leave it stalled), so HAL_Delay() and any
   * HAL_GetTick() deadline can hang forever. delay_ms(), the interrupt-driven
   * I2C/USB stacks, and the hardware-polled flash driver do not need the tick.
   */
  {
    for (uint32_t i = 0U; i < 100U; i++)   /* ~2 s of enumeration polling */
    {
      if (hUsbDeviceFS.dev_state >= USBD_STATE_ADDRESSED)
      {
        break;
      }
      (void)HAL_IWDG_Refresh(&hiwdg);
      delay_ms(20U);
    }
    delay_ms(200U);
  }

  if (!isUSBConnected)
  {
    /* ---------------- I2C DFU transport ---------------- */
#ifdef DEBUG_ENABLED
    printf("[BL] no USB host detected - I2C DFU slave mode (addr 0x%02X)\r\n",
           (unsigned int)DFU_I2C_ADDRESS);
#endif
    MX_USB_DEVICE_DeInit();
    I2C_DFU_Init(&hi2c1);

#ifndef DEBUG_ENABLED
    /* Phase 2: arm the one-wire back-channel so this bootloader-mode node can
     * answer the master's discovery walk and take a unique I2C address. */
    OW_BL_Init();
#endif

    /* Now that we are listening (or, in Debug, giving up), release the READY line
     * so the master proceeds with enumeration. */
    {
      GPIO_InitTypeDef ready_pin = {0};
      ready_pin.Pin  = RST_Pin;
      ready_pin.Mode = GPIO_MODE_INPUT;
      ready_pin.Pull = GPIO_NOPULL;
      HAL_GPIO_Init(RST_GPIO_Port, &ready_pin);
    }

    uint32_t blink = 0U;
    while (1)
    {
      if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
      {
        Error_Handler();
      }

      /* Execute any pending flash command received over I2C (also performs
       * the deferred NVIC_SystemReset after an I2C_DFU_CMD_RESET). */
      I2C_DFU_Process();

#ifndef DEBUG_ENABLED
      /* Service one-wire discovery / clear-config / relay on the back-channel. */
      OW_BL_Process();
#endif

      if (++blink >= 60U)   /* heartbeat LED (tick-free) */
      {
        HAL_GPIO_TogglePin(LD_HB_GPIO_Port, LD_HB_Pin);
        blink = 0U;
      }
      delay_ms(5U);   /* short yield; master polls status every ~20 ms */
    }
  }

  /* USB path: this node has a host and is being programmed — release the READY
   * line immediately (it is not a ready-signaling slave). */
  {
    GPIO_InitTypeDef ready_pin = {0};
    ready_pin.Pin  = RST_Pin;
    ready_pin.Mode = GPIO_MODE_INPUT;
    ready_pin.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(RST_GPIO_Port, &ready_pin);
  }

  /* ---------------- USB DFU transport ---------------- */
#ifdef DEBUG_ENABLED
  printf("[BL] USB host detected - USB DFU mode\r\n");
#endif

  uint32_t usb_blink = 0U;
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* Refresh IWDG (loop is well under the ~8 s timeout). */
    if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
    {
      /* Refresh Error */
      Error_Handler();
    }

    /* A new image was downloaded and the host has completed manifestation
     * (the DFU class is manifestation-tolerant, so it did NOT self-reset). */
    if (DFU_ImageDownloadComplete() != 0U)
    {
      if (DFU_IsRollback() != 0U)
      {
        /* Anti-rollback: the downloaded image is an older version than what was
         * installed. Destroy it so it can never boot, report, and stay in DFU
         * so a valid (>=) image can be flashed. Clear the completion latch so
         * this is handled only once. */
        DFU_InvalidateImage();
        DFU_ClearDownloadState();
#ifdef DEBUG_ENABLED
        printf("[BL] anti-rollback: rejected older firmware version - image erased, flash a build with an equal or higher version\r\n");
#endif
      }
      else
      {
        /* Reboot so SBSFU verifies and launches the freshly flashed firmware.
         * The brief delay lets the final USB control transfer settle and the
         * host tool exit cleanly before the bus drops. delay_ms() is tick-free
         * (see the note on the DFU transport-selection block above). */
        delay_ms(50U);
        NVIC_SystemReset();
      }
    }

    /* Host-requested reset (DNLOAD to the virtual reset address): the clean
     * way for a host tool to leave DFU without flashing — e.g. the SDK
     * aborting an update after its pre-flight downgrade check. SBSFU fully
     * re-verifies the slot on the way back up, so if the slot is intact the
     * application boots; if not, we simply return to DFU. */
    if (DFU_ResetRequested() != 0U)
    {
#ifdef DEBUG_ENABLED
      printf("[BL] host requested reset - leaving USB DFU mode\r\n");
#endif
      delay_ms(50U);
      NVIC_SystemReset();
    }

    if (++usb_blink >= 100U)   /* heartbeat LED (tick-free) */
    {
      HAL_GPIO_TogglePin(LD_HB_GPIO_Port, LD_HB_Pin);
      usb_blink = 0U;
    }
    delay_ms(5U);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 16;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV4;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_MSI, RCC_MCODIV_2);
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  /* LSI 32 kHz / 64 * 4095 ~= 8.2 s window: must cover SBSFU's un-refreshed
   * ECDSA/SHA-256 verification of the full application slot before launch. */
  hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
  hiwdg.Init.Window = 4095;
  hiwdg.Init.Reload = 4095;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/* NOTE: MX_RTC_Init (HAL RTC calendar init) was removed to save flash for the
 * dual USB/I2C DFU transports. The bootloader only touches the RTC BACKUP
 * REGISTERS (BKP6R boot counter, BKP7R DFU request), which need just the
 * backup-domain access sequence performed in main() SysInit — no calendar
 * configuration and no stm32l4xx_hal_rtc module. */

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_HalfDuplex_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* (Channels 2/3/4 belonged to USART1/USART3, which the bootloader does not
   * use — removed with their IRQ handlers to save flash.) */
  /* DMA1_Channel6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel6_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel6_IRQn);
  /* DMA1_Channel7_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel7_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel7_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, TR1_EN_Pin|REFSEL_Pin|TR3_EN_Pin|HW_SW_CTRL_Pin
                          |TR2_EN_Pin|TR7_EN_Pin|TR6_EN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, TX1_CS_Pin|TX2_CS_Pin|TR8_EN_Pin|TX_STDBY_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, TR4_EN_Pin|LD_HB_Pin|TX_RESET_L_Pin|TX_CW_EN_Pin
                          |TR5_EN_Pin|RDY_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SYSTEM_RDY_GPIO_Port, SYSTEM_RDY_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : INT_Pin */
  GPIO_InitStruct.Pin = INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(INT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : GPIO_1_Pin TX1_SHUTZ_Pin RX_I2C_SDA_Pin PC1
                           RX_I2C_SCL_Pin RX_RDY_Pin PC3 */
  GPIO_InitStruct.Pin = GPIO_1_Pin|TX1_SHUTZ_Pin|RX_I2C_SDA_Pin|GPIO_PIN_1
                          |RX_I2C_SCL_Pin|RX_RDY_Pin|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : TR1_EN_Pin REFSEL_Pin TR3_EN_Pin HW_SW_CTRL_Pin
                           TR2_EN_Pin TR7_EN_Pin TR6_EN_Pin */
  GPIO_InitStruct.Pin = TR1_EN_Pin|REFSEL_Pin|TR3_EN_Pin|HW_SW_CTRL_Pin
                          |TR2_EN_Pin|TR7_EN_Pin|TR6_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PDN_Pin EXT_Pin */
  GPIO_InitStruct.Pin = PDN_Pin|EXT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : TX1_CS_Pin TX2_CS_Pin TR8_EN_Pin TX_STDBY_Pin */
  GPIO_InitStruct.Pin = TX1_CS_Pin|TX2_CS_Pin|TR8_EN_Pin|TX_STDBY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : TR4_EN_Pin LD_HB_Pin TX_RESET_L_Pin TX_CW_EN_Pin
                           TR5_EN_Pin RDY_Pin */
  GPIO_InitStruct.Pin = TR4_EN_Pin|LD_HB_Pin|TX_RESET_L_Pin|TX_CW_EN_Pin
                          |TR5_EN_Pin|RDY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : SYSTEM_RDY_Pin */
  GPIO_InitStruct.Pin = SYSTEM_RDY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SYSTEM_RDY_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : REF_CLK_Pin */
  GPIO_InitStruct.Pin = REF_CLK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF0_MCO;
  HAL_GPIO_Init(REF_CLK_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : TX2_SHUTZ_Pin POWER_GOOD_Pin */
  GPIO_InitStruct.Pin = TX2_SHUTZ_Pin|POWER_GOOD_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : TRIGGER_Pin */
  GPIO_InitStruct.Pin = TRIGGER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF14_TIM15;
  HAL_GPIO_Init(TRIGGER_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : RST_Pin (PA1) — inter-board READY line (shared,
   * open-drain, wired-AND across the master and all slaves). Each board holds
   * it LOW = "not ready"; the master's application (WaitForAllSlavesReady)
   * blocks until the line goes HIGH, i.e. until every slave has released it.
   *
   * Drive it LOW for the entire time we are in the bootloader so the master
   * does not enumerate this board before its application is actually up. The
   * application takes over the line (it also defaults it LOW early in its own
   * MX_GPIO_Init) so there is no window where it floats HIGH across the
   * bootloader->application hand-off. On the DFU fallback path (no application
   * will run) the line is released back to Hi-Z (see main) so a board sitting
   * in DFU does not hang the master forever. */
  GPIO_InitStruct.Pin = RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RST_GPIO_Port, &GPIO_InitStruct);
  HAL_GPIO_WritePin(RST_GPIO_Port, RST_Pin, GPIO_PIN_RESET); /* LOW = not ready */

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{

#ifdef DEBUG_ENABLED
	if(huart->Instance == DEBUG_UART.Instance)
	{
		logging_UART_TxCpltCallback(huart);
    return;
	}
#else
	/* Phase 2 one-wire back-channel: mark CALL_IN/CALL_OUT transmit complete. */
	OW_BL_TxCpltCallback(huart);
#endif
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
// cppcheck-suppress constParameterPointer -- must match pTIM_CallbackTypeDef (non-const TIM_HandleTypeDef *)
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
