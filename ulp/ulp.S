#include "soc/rtc_cntl_reg.h"
#include "soc/rtc_io_reg.h"
#include "soc/soc_ulp.h"

#include "../include/pindef_ulp.h"

.global gpio_status_low
gpio_status_low: .long 0

.global gpio_status_high
gpio_status_high: .long 0

.global adc_value
adc_value: .long 0

.global prog_status
prog_status: .long 0

// Wake conditions:
// (any button pressed && battery reading > BAT_ADC_WAKEUP_THRESHOLD ) || is charging
.global entry
entry:
    // reset program status
    MOVE r0, 0
    MOVE r1, prog_status
    ST r0, r1, 0

    // Read from ADC
    ADC r0, ADC_UNIT, BAT_ADC_PAD
    // Load ADC value address
    MOVE r1, adc_value
    // Check if ADC value is below critical threshold
    JUMPR suicide, BAT_ADC_CRITICAL_THRESHOLD, LT
    // JUMP suicide
    // Check if ADC value is above wakeup threshold
    JUMPR exit, BAT_ADC_WAKEUP_THRESHOLD, LT
    // Store ADC value
    ST r0, r1, 0
    
    // check for low byte buttons
    READ_RTC_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S, 16)
    
    // Isolate low buttons, low-active
    AND R0, R0, LOW_BTNS_RTC_GPIO_MASK
    // Store the low byte button status
    MOVE r1, gpio_status_low
    ST r0, r1, 0
    // Subtract the mask to see if equal
    SUB R0, R0, LOW_BTNS_RTC_GPIO_MASK
    // If equal, no buttons pressed
    JUMP check_high, eq
    // else, wake up the main processor
    JUMP wakeup
check_high:
    // Similarly, check for high byte buttons
    READ_RTC_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S+16, 2)
    AND R0, R0, HIGH_BTNS_RTC_GPIO_MASK
    MOVE r1, gpio_status_high
    ST r0, r1, 0
    SUB R0, R0, HIGH_BTNS_RTC_GPIO_MASK
    JUMP exit, eq
    // JUMP call can be omitted as wakeup procedure follows next

wakeup:
    STAGE_INC 1
    JUMPS wakeup_exit, 10, GT
    // Test whether can wakeup
    READ_RTC_FIELD(RTC_CNTL_LOW_POWER_ST_REG, RTC_CNTL_RDY_FOR_WAKEUP)
    AND R0, R0,1
    JUMP wakeup, eq
    WAKE
    // Stop Wakeup timer
    WRITE_RTC_FIELD(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN, 0)
    HALT

wakeup_exit:
    // Wakeup attempted
    MOVE r0, 0xbeef
    MOVE r1, prog_status
    ST r0, r1, 0
    HALT

exit:
    // set program status to indicate exit
    MOVE r0, 0xdead
    MOVE r1, prog_status
    ST r0, r1, 0
    HALT

suicide:
    // disable LDO EN hold
    WRITE_RTC_FIELD(ADC_RTCIO_REG, RTCIO_RTC_GPIO_PIN12_HOLD, 0)
    // turn off LDO EN pin
    WRITE_RTC_REG(RTC_GPIO_OUT_W1TC_REG, LDO_EN_RTC_GPIO_OFFSET + RTC_GPIO_OUT_DATA_W1TC_S, 1, 1)
    WRITE_RTC_FIELD(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN, 0)
    HALT