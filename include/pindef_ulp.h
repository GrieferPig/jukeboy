#pragma once
// Use READ_RTC_REG (RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S, 16)
// As ULP register can only read 16 bits at a time,
// for exception pins, use READ_RTC_REG (RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S + 16, 2)
// Inputs are low-active.
#define BTN1_RTC_GPIO_MASK 1 << 14         // RTC_GPIO14
#define BTN2_RTC_GPIO_MASK 1 << (16 - 16)  // RTC_GPIO16 (exception)
#define BTN3_RTC_GPIO_MASK 1 << (17 - 16)  // RTC_GPIO17 (exception)
#define BTN4_RTC_GPIO_MASK 1 << 7          // RTC_GPIO7
#define BTN5_RTC_GPIO_MASK 1 << 6          // RTC_GPIO6
#define BTN6_RTC_GPIO_MASK 1 << 9          // RTC_GPIO9
#define CART_PRESENCE_RTC_GPIO_MASK 1 << 5 // RTC_GPIO5
// Detect cart presence separately
#define LOW_BTNS_RTC_GPIO_MASK (BTN1_RTC_GPIO_MASK | BTN4_RTC_GPIO_MASK | BTN5_RTC_GPIO_MASK | BTN6_RTC_GPIO_MASK)
#define HIGH_BTNS_RTC_GPIO_MASK (BTN2_RTC_GPIO_MASK | BTN3_RTC_GPIO_MASK)

#define LDO_EN_RTC_GPIO_OFFSET 13 // RTC_GPIO13

// ADC pads
// From espressif docs: If the user passes Mux value 1, then ADC pad 0 gets used.
#define BAT_ADC_PAD (0 + 1)   // Pad 1, channel 0
#define STDBY_ADC_PAD (3 + 1) // Pad 4, channel 3

#define ADC_UNIT 0 // ADC unit 1
#define BAT_ADC_WAKEUP_THRESHOLD 2700