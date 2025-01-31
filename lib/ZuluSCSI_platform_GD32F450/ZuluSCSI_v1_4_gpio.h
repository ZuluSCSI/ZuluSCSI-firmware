// GPIO definitions for ZuluSCSI v1.4

#pragma once

#include "gd32f4xx_syscfg.h"

// SCSI data output port.
// The output data is written using BSRR mechanism, so all data pins must be on same GPIO port.
// The output pins are open-drain in hardware, using separate buffer chips for driving.
#define SCSI_OUT_PORT GPIOD
#define SCSI_OUT_DB7  GPIO_PIN_9
#define SCSI_OUT_DB6  GPIO_PIN_10
#define SCSI_OUT_DB5  GPIO_PIN_11
#define SCSI_OUT_DB4  GPIO_PIN_12
#define SCSI_OUT_DB3  GPIO_PIN_13
#define SCSI_OUT_DB2  GPIO_PIN_14
#define SCSI_OUT_DB1  GPIO_PIN_0
#define SCSI_OUT_DB0  GPIO_PIN_1
#define SCSI_OUT_DBP  GPIO_PIN_8
#define SCSI_OUT_REQ  GPIO_PIN_4
#define SCSI_OUT_DATA_MASK (SCSI_OUT_DB0 | SCSI_OUT_DB1 | SCSI_OUT_DB2 | SCSI_OUT_DB3 | SCSI_OUT_DB4 | SCSI_OUT_DB5 | SCSI_OUT_DB6 | SCSI_OUT_DB7 | SCSI_OUT_DBP)
#define SCSI_OUT_REQ_IDX 4

// Control signals to optional PLD device
#define SCSI_OUT_PLD1 GPIO_PIN_15
#define SCSI_OUT_PLD2 GPIO_PIN_3
#define SCSI_OUT_PLD3 GPIO_PIN_5
#define SCSI_OUT_PLD4 GPIO_PIN_7

// Control signals for timer based DMA acceleration
// TIMER0_CH1 triggers DMACHA
// TIMER0_CH3 triggers DMACHB
#define SCSI_TIMER TIMER0
#define SCSI_TIMER_RCU RCU_TIMER0
#define SCSI_TIMER_OUT_PORT GPIOB
#define SCSI_TIMER_OUT_PIN GPIO_PIN_15
#define SCSI_TIMER_OUT_AF GPIO_AF_1
#define SCSI_TIMER_IN_PORT GPIOA
#define SCSI_TIMER_IN_PIN GPIO_PIN_8
#define SCSI_TIMER_IN_AF GPIO_AF_1
#define SCSI_TIMER_DMA DMA1
#define SCSI_TIMER_DMA_RCU RCU_DMA1
#define SCSI_TIMER_DMACHA DMA_CH2
#define SCSI_TIMER_DMACHA_SUB_PERIPH DMA_SUBPERI6
#define SCSI_TIMER_DMACHB DMA_CH4
#define SCSI_TIMER_DMACHB_SUB_PERIPH DMA_SUBPERI6
#define SCSI_TIMER_DMACHA_IRQ DMA1_Channel2_IRQHandler
#define SCSI_TIMER_DMACHA_IRQn DMA1_Channel2_IRQn
#define SCSI_TIMER_DMACHB_IRQ DMA1_Channel4_IRQHandler
#define SCSI_TIMER_DMACHB_IRQn DMA1_Channel4_IRQn

// GreenPAK logic chip pins
#define GREENPAK_I2C_ADDR 0x10
#define GREENPAK_I2C_PORT GPIOB
#define GREENPAK_I2C_SCL GPIO_PIN_8
#define GREENPAK_I2C_SDA GPIO_PIN_9
#define GREENPAK_PLD_PORT GPIOD
#define GREENPAK_PLD_IO1 GPIO_PIN_15
#define GREENPAK_PLD_IO2 GPIO_PIN_3
#define GREENPAK_PLD_IO3 GPIO_PIN_5
#define GREENPAK_PLD_IO4 GPIO_PIN_7
#define GREENPAK_PLD_IO2_EXTI EXTI_3
#define GREENPAK_PLD_IO2_EXTI_SOURCE_PORT EXTI_SOURCE_GPIOD
#define GREENPAK_PLD_IO2_EXTI_SOURCE_PIN  EXTI_SOURCE_PIN3
#define GREENPAK_IRQ  EXTI3_IRQHandler
#define GREENPAK_IRQn EXTI3_IRQn

// SCSI input data port
#define SCSI_IN_PORT  GPIOE
#define SCSI_IN_DB7   GPIO_PIN_15
#define SCSI_IN_DB6   GPIO_PIN_14
#define SCSI_IN_DB5   GPIO_PIN_13
#define SCSI_IN_DB4   GPIO_PIN_12
#define SCSI_IN_DB3   GPIO_PIN_11
#define SCSI_IN_DB2   GPIO_PIN_10
#define SCSI_IN_DB1   GPIO_PIN_9
#define SCSI_IN_DB0   GPIO_PIN_8
#define SCSI_IN_DBP   GPIO_PIN_7
#define SCSI_IN_MASK  (SCSI_IN_DB7|SCSI_IN_DB6|SCSI_IN_DB5|SCSI_IN_DB4|SCSI_IN_DB3|SCSI_IN_DB2|SCSI_IN_DB1|SCSI_IN_DB0|SCSI_IN_DBP)
#define SCSI_IN_SHIFT 8

// SCSI output status lines
#define SCSI_OUT_IO_PORT  GPIOA
#define SCSI_OUT_IO_PIN   GPIO_PIN_4
#define SCSI_OUT_CD_PORT  GPIOF
#define SCSI_OUT_CD_PIN   GPIO_PIN_12
#define SCSI_OUT_SEL_PORT GPIOA
#define SCSI_OUT_SEL_PIN  GPIO_PIN_6
#define SCSI_OUT_MSG_PORT GPIOG
#define SCSI_OUT_MSG_PIN  GPIO_PIN_1
#define SCSI_OUT_RST_PORT GPIOB
#define SCSI_OUT_RST_PIN  GPIO_PIN_14
#define SCSI_OUT_BSY_PORT GPIOG
#define SCSI_OUT_BSY_PIN  GPIO_PIN_0
#define SCSI_OUT_REQ_PORT SCSI_OUT_PORT
#define SCSI_OUT_REQ_PIN  SCSI_OUT_REQ

// SCSI input status signals
#define SCSI_ACK_PORT GPIOA
#define SCSI_ACK_PIN  GPIO_PIN_0
#define SCSI_ATN_PORT GPIOF
#define SCSI_ATN_PIN  GPIO_PIN_14
#define SCSI_IN_ACK_IDX 0

// Extra signals used with EXMC for synchronous mode
#define SCSI_IN_ACK_EXMC_NWAIT_PORT GPIOD
#define SCSI_IN_ACK_EXMC_NWAIT_PIN  GPIO_PIN_6
#define SCSI_OUT_REQ_EXMC_NOE_PORT  GPIOD
#define SCSI_OUT_REQ_EXMC_NOE_PIN   GPIO_PIN_4
#define SCSI_OUT_REQ_EXMC_NOE_IDX   4
#define SCSI_EXMC_DATA_SHIFT 5
// DMA1 is the only controller that allows memory to memory transfers
#define SCSI_EXMC_DMA DMA1  
#define SCSI_EXMC_DMA_RCU RCU_DMA1
#define SCSI_EXMC_DMACH DMA_CH0
#define SCSI_SYNC_TIMER TIMER1
#define SCSI_SYNC_TIMER_RCU RCU_TIMER1

// SEL pin uses EXTI interrupt
#define SCSI_SEL_PORT GPIOB
#define SCSI_SEL_PIN  GPIO_PIN_11
#define SCSI_SEL_EXTI EXTI_11
#define SCSI_SEL_EXTI_SOURCE_PORT EXTI_SOURCE_GPIOB
#define SCSI_SEL_EXTI_SOURCE_PIN EXTI_SOURCE_PIN11
#define SCSI_SEL_IRQ EXTI10_15_IRQHandler
#define SCSI_SEL_IRQn EXTI10_15_IRQn

// BSY pin uses EXTI interrupt
#define SCSI_BSY_PORT GPIOF
#define SCSI_BSY_PIN  GPIO_PIN_13
#define SCSI_BSY_EXTI EXTI_13
#define SCSI_BSY_EXTI_SOURCE_PORT EXTI_SOURCE_GPIOF
#define SCSI_BSY_EXTI_SOURCE_PIN  EXTI_SOURCE_PIN13
#define SCSI_BSY_IRQ  EXTI10_15_IRQHandler
#define SCSI_BSY_IRQn EXTI10_15_IRQn

// RST pin uses EXTI interrupt
#define SCSI_RST_PORT GPIOF
#define SCSI_RST_PIN  GPIO_PIN_15
#define SCSI_RST_EXTI EXTI_15
#define SCSI_RST_EXTI_SOURCE_PORT EXTI_SOURCE_GPIOF
#define SCSI_RST_EXTI_SOURCE_PIN EXTI_SOURCE_PIN15
#define SCSI_RST_IRQ  EXTI10_15_IRQHandler
#define SCSI_RST_IRQn EXTI10_15_IRQn

// Terminator enable/disable config, active low
#define SCSI_TERM_EN_PORT GPIOA
#define SCSI_TERM_EN_PIN  GPIO_PIN_10

// SD card pins
#define SD_USE_SDIO 1
#define SD_SDIO_DATA_PORT GPIOC
#define SD_SDIO_D0        GPIO_PIN_8
#define SD_SDIO_D1        GPIO_PIN_9
#define SD_SDIO_D2        GPIO_PIN_10
#define SD_SDIO_D3        GPIO_PIN_11
#define SD_SDIO_CLK_PORT  GPIOC
#define SD_SDIO_CLK       GPIO_PIN_12
#define SD_SDIO_CMD_PORT  GPIOD
#define SD_SDIO_CMD       GPIO_PIN_2

// DIP switches
#define DIP_PORT     GPIOB
#define DIPSW1_PIN   GPIO_PIN_4
#define DIPSW2_PIN   GPIO_PIN_6
#define DIPSW3_PIN   GPIO_PIN_7

// Status LED pins
#define LED_PORT     GPIOF
#define LED_I_PIN    GPIO_PIN_4
#define LED_E_PIN    GPIO_PIN_5
#define LED_PINS     (LED_I_PIN | LED_E_PIN)

// User LEDs
#define USER_LED_PORT       GPIOC
#define USER_LED_1_PIN      GPIO_PIN_14
#define USER_LED_2_PIN      GPIO_PIN_15
#define USER_LED_1_ON()     gpio_bit_reset(USER_LED_PORT, USER_LED_1_PIN);
#define USER_LED_1_OFF()    gpio_bit_set(USER_LED_PORT, USER_LED_1_PIN);
#define USER_LED_2_ON()     gpio_bit_reset(USER_LED_PORT, USER_LED_2_PIN);
#define USER_LED_2_OFF()    gpio_bit_set(USER_LED_PORT, USER_LED_2_PIN);

// USB HS
#define USB_HS_ULPI_STP_PORT GPIOC
#define USB_HS_ULPI_STP_PIN  GPIO_PIN_0
#define USB_HS_ULPI_CLK_PORT GPIOA
#define USB_HS_ULPI_CLK_PIN  GPIO_PIN_5
#define USB_HS_ULPI_NXT_PORT GPIOC
#define USB_HS_ULPI_NXT_PIN  GPIO_PIN_3 
#define USB_HS_ULPI_DIR_PORT GPIOC
#define USB_HS_ULPI_DIR_PIN  GPIO_PIN_2


#define USB_HS_ULPI_D0_PORT GPIOA
#define USB_HS_ULPI_D0_PIN  GPIO_PIN_3 
#define USB_HS_ULPI_D1_TO_7_PORT GPIOB
#define USB_HS_ULPI_D1_TO_7_PIN   GPIO_PIN_0  \
                                | GPIO_PIN_1  \
                                | GPIO_PIN_10 \
                                | GPIO_PIN_2  \
                                | GPIO_PIN_12 \
                                | GPIO_PIN_13 \
                                | GPIO_PIN_5
#define USB_HS_ULPI_AF 10

