#include "soc/rtc_cntl_reg.h"
#include "soc/soc_ulp.h"

    // Define the variable in RTC slow memory to hold the ADC result
    .data
    .global adc_result
adc_result:
    .long 0

    // Main entry point for the ULP program
    .text
    .global entry
entry:
    // Perform ADC measurement on ADC1, channel 0 (GPIO36)
    // The result is stored in register R0
    ADC r0, 0, 0

    // Load the memory address of adc_result into R1
    MOVE r1, adc_result
    
    // Store the ADC reading from R0 into the adc_result variable
    ST r0, r1, 0

    // Halt the ULP until the next run
    HALT