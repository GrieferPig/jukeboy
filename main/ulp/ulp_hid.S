/* ULP assembly files are passed through the C preprocessor first. */
#include "soc/soc_ulp.h"

    /* GPIO35 = ADC1 channel 7 -> mux 8. */
    .set main_adc_mux, 8
    /* GPIO34 = ADC1 channel 6 -> mux 7. */
    .set misc_adc_mux, 7

    .set adc_oversampling_factor_log, 4
    .set adc_oversampling_factor, (1 << adc_oversampling_factor_log)

    .set btn_none_low, 1343
    .set btn_none_high, 1443
    .set btn_1_low, 1477
    .set btn_1_high, 1577
    .set btn_2_low, 1664
    .set btn_2_high, 1764
    .set btn_12_low, 1861
    .set btn_12_high, 1961
    .set btn_3_low, 2275
    .set btn_3_high, 2375
    .set btn_13_low, 2640
    .set btn_13_high, 2740
    .set btn_23_low, 3265
    .set btn_23_high, 3365
    .set btn_123_low, 4045

    .bss
    .global main_stable
main_stable:
    .long 0

    .global main_counter
main_counter:
    .long 0

    .global misc_stable
misc_stable:
    .long 0

    .global misc_counter
misc_counter:
    .long 0

    .global debounce_threshold
debounce_threshold:
    .long 0

    .global main_last_raw
main_last_raw:
    .long 0

    .global misc_last_raw
misc_last_raw:
    .long 0

    .global button_state
button_state:
    .long 0

    .global button_generation
button_generation:
    .long 0

    .text
    .global entry
entry:
    move r2, 0

    /* MAIN ladder on GPIO35 / ADC1_CH7. */
    move r0, 0
    stage_rst
main_measure:
    adc r1, 0, main_adc_mux
    add r0, r0, r1
    stage_inc 1
    jumps main_measure, adc_oversampling_factor, lt
    rsh r0, r0, adc_oversampling_factor_log
    move r3, main_last_raw
    st r0, r3, 0

    move r3, btn_none_low
    sub r1, r0, r3
    jump main_same, ov

    move r3, btn_none_high
    sub r1, r3, r0
    jump main_check_1, ov
    move r0, 0
    jump main_debounce

main_check_1:
    move r3, btn_1_low
    sub r1, r0, r3
    jump main_same, ov

    move r3, btn_1_high
    sub r1, r3, r0
    jump main_check_2, ov
    move r0, 1
    jump main_debounce

main_check_2:
    move r3, btn_2_low
    sub r1, r0, r3
    jump main_same, ov

    move r3, btn_2_high
    sub r1, r3, r0
    jump main_check_12, ov
    move r0, 2
    jump main_debounce

main_check_12:
    move r3, btn_12_low
    sub r1, r0, r3
    jump main_same, ov

    move r3, btn_12_high
    sub r1, r3, r0
    jump main_check_3, ov
    move r0, 3
    jump main_debounce

main_check_3:
    move r3, btn_3_low
    sub r1, r0, r3
    jump main_same, ov

    move r3, btn_3_high
    sub r1, r3, r0
    jump main_check_13, ov
    move r0, 4
    jump main_debounce

main_check_13:
    move r3, btn_13_low
    sub r1, r0, r3
    jump main_same, ov

    move r3, btn_13_high
    sub r1, r3, r0
    jump main_check_23, ov
    move r0, 5
    jump main_debounce

main_check_23:
    move r3, btn_23_low
    sub r1, r0, r3
    jump main_same, ov

    move r3, btn_23_high
    sub r1, r3, r0
    jump main_check_123, ov
    move r0, 6
    jump main_debounce

main_check_123:
    move r3, btn_123_low
    sub r1, r0, r3
    jump main_same, ov
    move r0, 7

main_debounce:
    move r3, main_stable
    ld r1, r3, 0
    sub r1, r0, r1
    jump main_same, eq

    move r3, main_counter
    ld r1, r3, 0
    add r1, r1, 1
    st r1, r3, 0
    move r3, debounce_threshold
    ld r3, r3, 0
    sub r1, r1, r3
    jump main_commit, eq
    jump process_misc

main_same:
    move r3, main_counter
    move r1, 0
    st r1, r3, 0
    jump process_misc

main_commit:
    move r3, main_stable
    st r0, r3, 0
    move r3, main_counter
    move r1, 0
    st r1, r3, 0
    move r2, 1

process_misc:
    /* MISC ladder on GPIO34 / ADC1_CH6. */
    move r0, 0
    stage_rst
misc_measure:
    adc r1, 0, misc_adc_mux
    add r0, r0, r1
    stage_inc 1
    jumps misc_measure, adc_oversampling_factor, lt
    rsh r0, r0, adc_oversampling_factor_log
    move r3, misc_last_raw
    st r0, r3, 0

    move r3, btn_none_low
    sub r1, r0, r3
    jump misc_same, ov

    move r3, btn_none_high
    sub r1, r3, r0
    jump misc_check_1, ov
    move r0, 0
    jump misc_debounce

misc_check_1:
    move r3, btn_1_low
    sub r1, r0, r3
    jump misc_same, ov

    move r3, btn_1_high
    sub r1, r3, r0
    jump misc_check_2, ov
    move r0, 1
    jump misc_debounce

misc_check_2:
    move r3, btn_2_low
    sub r1, r0, r3
    jump misc_same, ov

    move r3, btn_2_high
    sub r1, r3, r0
    jump misc_check_12, ov
    move r0, 2
    jump misc_debounce

misc_check_12:
    move r3, btn_12_low
    sub r1, r0, r3
    jump misc_same, ov

    move r3, btn_12_high
    sub r1, r3, r0
    jump misc_check_3, ov
    move r0, 3
    jump misc_debounce

misc_check_3:
    move r3, btn_3_low
    sub r1, r0, r3
    jump misc_same, ov

    move r3, btn_3_high
    sub r1, r3, r0
    jump misc_check_13, ov
    move r0, 4
    jump misc_debounce

misc_check_13:
    move r3, btn_13_low
    sub r1, r0, r3
    jump misc_same, ov

    move r3, btn_13_high
    sub r1, r3, r0
    jump misc_check_23, ov
    move r0, 5
    jump misc_debounce

misc_check_23:
    move r3, btn_23_low
    sub r1, r0, r3
    jump misc_same, ov

    move r3, btn_23_high
    sub r1, r3, r0
    jump misc_check_123, ov
    move r0, 6
    jump misc_debounce

misc_check_123:
    move r3, btn_123_low
    sub r1, r0, r3
    jump misc_same, ov
    move r0, 7

misc_debounce:
    move r3, misc_stable
    ld r1, r3, 0
    sub r1, r0, r1
    jump misc_same, eq

    move r3, misc_counter
    ld r1, r3, 0
    add r1, r1, 1
    st r1, r3, 0
    move r3, debounce_threshold
    ld r3, r3, 0
    sub r1, r1, r3
    jump misc_commit, eq
    jump update_state

misc_same:
    move r3, misc_counter
    move r1, 0
    st r1, r3, 0
    jump update_state

misc_commit:
    move r3, misc_stable
    st r0, r3, 0
    move r3, misc_counter
    move r1, 0
    st r1, r3, 0
    move r2, 1

update_state:
    move r3, main_stable
    ld r0, r3, 0

    move r3, misc_stable
    ld r1, r3, 0
    add r1, r1, r1
    add r1, r1, r1
    add r1, r1, r1
    add r0, r0, r1

store_state:
    move r3, button_state
    st r0, r3, 0

    add r2, r2, 0
    jump done, eq

    move r3, button_generation
    ld r0, r3, 0
    add r0, r0, 1
    st r0, r3, 0
    wake

done:
    halt