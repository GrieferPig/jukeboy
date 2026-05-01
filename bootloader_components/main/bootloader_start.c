/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/reent.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "rom/uart.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"

#define CUSTOM_DOWNLOAD_MAGIC_WORD 0xDEADBEEF
#define CUSTOM_FLAG_ADDR ((volatile uint32_t *)0x3FF81FFC)

static const char *TAG = "boot";

static int select_partition_number(bootloader_state_t *bs);

static void bootloader_gpio_make_disabled(int gpio_num)
{
    esp_rom_gpio_pad_select_gpio(gpio_num);
    esp_rom_gpio_connect_out_signal(gpio_num, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_pullup_dis(&GPIO, gpio_num);
    gpio_ll_pulldown_dis(&GPIO, gpio_num);
    gpio_ll_input_disable(&GPIO, gpio_num);
    gpio_ll_output_disable(&GPIO, gpio_num);
}

static void bootloader_gpio_make_input_nopull(int gpio_num)
{
    esp_rom_gpio_pad_select_gpio(gpio_num);
    esp_rom_gpio_connect_out_signal(gpio_num, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_pullup_dis(&GPIO, gpio_num);
    gpio_ll_pulldown_dis(&GPIO, gpio_num);
    gpio_ll_output_disable(&GPIO, gpio_num);
    gpio_ll_input_enable(&GPIO, gpio_num);
}

static void bootloader_gpio_make_output_low(int gpio_num)
{
    esp_rom_gpio_pad_select_gpio(gpio_num);
    esp_rom_gpio_connect_out_signal(gpio_num, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_pullup_dis(&GPIO, gpio_num);
    gpio_ll_pulldown_dis(&GPIO, gpio_num);
    gpio_ll_input_disable(&GPIO, gpio_num);
    gpio_ll_set_level(&GPIO, gpio_num, 0);
    gpio_ll_output_enable(&GPIO, gpio_num);
}

void bootloader_user_gpio_init(void)
{
    /* Disable input and output */
    bootloader_gpio_make_input_nopull(25);
    bootloader_gpio_make_input_nopull(26);
    bootloader_gpio_make_input_nopull(27);
    // bootloader_gpio_make_input_nopull(14);
    bootloader_gpio_make_input_nopull(13);
    bootloader_gpio_make_input_nopull(22);
    bootloader_gpio_make_input_nopull(18);
    // bootloader_gpio_make_input_nopull(2);
    // bootloader_gpio_make_input_nopull(15);

    /* Enable input, no pull-up */
    bootloader_gpio_make_input_nopull(34);
    bootloader_gpio_make_input_nopull(35);

    /* Enable output low */
    bootloader_gpio_make_output_low(21);
    bootloader_gpio_make_output_low(32);
    bootloader_gpio_make_output_low(33);
    bootloader_gpio_make_output_low(23);
    bootloader_gpio_make_output_low(19);
    bootloader_gpio_make_output_low(5);
    bootloader_gpio_make_output_low(4);
    bootloader_gpio_make_output_low(12);
}

/* =====================================================================
 * esptool-compatible SLIP/download protocol
 *
 * Implements the minimum command set required for esptool to upload and
 * run its stub flasher:
 *   SYNC (0x08), READ_REG (0x0A), WRITE_REG (0x09),
 *   MEM_BEGIN (0x05), MEM_DATA (0x07), MEM_END (0x06)
 *
 * Protocol reference:
 *   https://docs.espressif.com/projects/esptool/en/latest/
 *   esp32/advanced-topics/serial-protocol.html
 * ===================================================================== */

/* ---- SLIP framing constants ---- */
#define SLIP_END 0xC0u
#define SLIP_ESC 0xDBu
#define SLIP_ESC_END 0xDCu
#define SLIP_ESC_ESC 0xDDu

/* ---- Packet direction bytes ---- */
#define DIR_REQ 0x00u
#define DIR_RESP 0x01u

/* ---- Command opcodes ---- */
#define CMD_MEM_BEGIN 0x05u
#define CMD_MEM_END 0x06u
#define CMD_MEM_DATA 0x07u
#define CMD_SYNC 0x08u
#define CMD_WRITE_REG 0x09u
#define CMD_READ_REG 0x0Au

/* ---- ROM-compatible error codes ---- */
#define ROM_ERR_INVALID_FORMAT 0x05u
#define ROM_ERR_CHECKSUM 0x07u

/* ---- Checksum seed (XOR starting value, per esptool spec) ---- */
#define CHKSUM_SEED 0xEFu

/*
 * Receive buffer.
 * Max MEM_DATA size = 8-byte header + 16-byte data prefix + ESP_RAM_BLOCK.
 * ESP_RAM_BLOCK for ESP32 is 0x1800 (6 KB); 8 KB is sufficient with margin.
 * Lives in BSS – does not increase the flash image size.
 */
#define DL_RECV_BUF_SIZE (8u * 1024u)
static uint8_t s_dl_recv_buf[DL_RECV_BUF_SIZE];

/* RAM download state set by MEM_BEGIN */
static uint32_t s_mem_base;
static uint32_t s_mem_blksz;

/* ---- Low-level UART I/O (ROM functions) ---- */

static inline uint8_t dl_uart_getc(void)
{
    uint8_t c;
    while (uart_rx_one_char(&c) != ETS_OK)
    { /* spin until byte available */
    }
    return c;
}

static inline void dl_uart_putc(uint8_t c)
{
    uart_tx_one_char(c);
}

/* ---- SLIP transmit helpers ---- */

/* Transmit one SLIP-escaped byte */
static void dl_slip_putc(uint8_t c)
{
    if (c == SLIP_END)
    {
        dl_uart_putc(SLIP_ESC);
        dl_uart_putc(SLIP_ESC_END);
    }
    else if (c == SLIP_ESC)
    {
        dl_uart_putc(SLIP_ESC);
        dl_uart_putc(SLIP_ESC_ESC);
    }
    else
    {
        dl_uart_putc(c);
    }
}

/* ---- SLIP receive ---- */

/*
 * Receive one SLIP-decoded packet into buf[0..buflen-1].
 * Blocks until a complete packet arrives.
 * Returns the decoded byte count.  If the decoded packet exceeded buflen,
 * the extra bytes are counted but not stored; the caller can detect overflow
 * by comparing the return value against buflen.
 */
static int dl_slip_recv(uint8_t *buf, int buflen)
{
    uint8_t c;

    /* Wait for the opening END marker, discarding any preceding garbage */
    do
    {
        c = dl_uart_getc();
    } while (c != (uint8_t)SLIP_END);

    int len = 0;
    bool in_esc = false;

    for (;;)
    {
        c = dl_uart_getc();

        if (in_esc)
        {
            in_esc = false;
            if (c == (uint8_t)SLIP_ESC_END)
                c = (uint8_t)SLIP_END;
            else if (c == (uint8_t)SLIP_ESC_ESC)
                c = (uint8_t)SLIP_ESC;
            /* else: unknown escape – pass raw byte through */
        }
        else if (c == (uint8_t)SLIP_END)
        {
            if (len > 0)
                return len; /* valid end-of-packet */
            /* consecutive END bytes (inter-frame gap) – restart */
            continue;
        }
        else if (c == (uint8_t)SLIP_ESC)
        {
            in_esc = true;
            continue;
        }

        if (len < buflen)
            buf[len] = c;
        len++;
    }
}

/* ---- Response builder ---- */

/*
 * Send a ROM-compatible response packet in SLIP framing.
 *
 * Response layout (per spec):
 *   [0]    direction = 0x01
 *   [1]    op        = echo of command opcode
 *   [2-3]  size      = payload_len + 4  (LE16)
 *   [4-7]  value     = 32-bit return value (LE32, used by READ_REG)
 *   [8..8+payload_len-1]  optional payload bytes
 *   [last 4 bytes]  ROM status: [status, error, 0, 0]
 */
static void dl_send_resp(uint8_t op, uint32_t value,
                         const uint8_t *payload, uint16_t payload_len,
                         uint8_t status, uint8_t error)
{
    uint16_t total_size = payload_len + 4u; /* 4 ROM status bytes */

    uint8_t hdr[8] = {
        DIR_RESP,
        op,
        (uint8_t)(total_size & 0xFFu),
        (uint8_t)((total_size >> 8) & 0xFFu),
        (uint8_t)(value & 0xFFu),
        (uint8_t)((value >> 8) & 0xFFu),
        (uint8_t)((value >> 16) & 0xFFu),
        (uint8_t)((value >> 24) & 0xFFu),
    };
    uint8_t status_bytes[4] = {status, error, 0u, 0u};

    dl_uart_putc(SLIP_END);
    for (int i = 0; i < 8; i++)
        dl_slip_putc(hdr[i]);
    for (uint16_t i = 0; i < payload_len; i++)
        dl_slip_putc(payload[i]);
    for (int i = 0; i < 4; i++)
        dl_slip_putc(status_bytes[i]);
    dl_uart_putc(SLIP_END);
}

#define DL_OK(op, val) dl_send_resp((op), (val), NULL, 0u, 0u, 0u)
#define DL_ERR(op, err) dl_send_resp((op), 0u, NULL, 0u, 1u, (err))

/* ---- Checksum (XOR over data bytes, seed 0xEF) ---- */

static uint8_t dl_checksum(const uint8_t *data, uint32_t len)
{
    uint8_t s = CHKSUM_SEED;
    for (uint32_t i = 0; i < len; i++)
        s ^= data[i];
    return s;
}

/* ---- Portable little-endian 32-bit read from a byte buffer ---- */

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- Protocol main loop ---- */

/*
 * Run the esptool SLIP download protocol.
 *
 * Processes commands until MEM_END is received with a non-zero entry point
 * (execute == 0 per esptool's packing convention).  Returns that entry point
 * address so the caller can jump to the uploaded stub.
 */
static uint32_t dl_run_protocol(void)
{
    for (;;)
    {
        int pkt_len = dl_slip_recv(s_dl_recv_buf, (int)sizeof(s_dl_recv_buf));

        /* Discard packets that are too short or overflowed our buffer */
        if (pkt_len < 8 || pkt_len > (int)sizeof(s_dl_recv_buf))
            continue;

        uint8_t dir = s_dl_recv_buf[0];
        uint8_t op = s_dl_recv_buf[1];
        uint16_t size = (uint16_t)s_dl_recv_buf[2] | ((uint16_t)s_dl_recv_buf[3] << 8);
        uint32_t chk = le32(&s_dl_recv_buf[4]);
        uint8_t *data = &s_dl_recv_buf[8];

        if (dir != (uint8_t)DIR_REQ)
            continue; /* ignore non-requests    */
        if ((int)(8u + size) > pkt_len)
            continue; /* truncated packet       */

        switch (op)
        {

        /* ---- SYNC (0x08) ---------------------------------------------- */
        case CMD_SYNC:
        {
            /*
             * esptool sends one SYNC command and reads back 8 responses.
             * The Value field must be non-zero so esptool knows it is
             * talking to a ROM-like loader (not an already-running stub).
             */
            for (int i = 0; i < 8; i++)
            {
                DL_OK(CMD_SYNC, 0x20120707u);
            }
            break;
        }

        /* ---- READ_REG (0x0A) ------------------------------------------ */
        case CMD_READ_REG:
        {
            if (size < 4u)
            {
                DL_ERR(op, ROM_ERR_INVALID_FORMAT);
                break;
            }
            uint32_t addr = le32(data);
            uint32_t val = *(volatile uint32_t *)(uintptr_t)addr;
            DL_OK(CMD_READ_REG, val);
            break;
        }

        /* ---- WRITE_REG (0x09) ----------------------------------------- */
        case CMD_WRITE_REG:
        {
            /* address(4), value(4), mask(4), delay_us(4) */
            if (size < 16u)
            {
                DL_ERR(op, ROM_ERR_INVALID_FORMAT);
                break;
            }
            uint32_t addr = le32(data);
            uint32_t value = le32(data + 4);
            uint32_t mask = le32(data + 8);
            /* delay_us at data+12 – not implemented in bootloader context */
            volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)addr;
            *reg = (*reg & ~mask) | (value & mask);
            DL_OK(CMD_WRITE_REG, 0u);
            break;
        }

        /* ---- MEM_BEGIN (0x05) ----------------------------------------- */
        case CMD_MEM_BEGIN:
        {
            /* total_size(4), num_blocks(4), block_size(4), offset(4) */
            if (size < 16u)
            {
                DL_ERR(op, ROM_ERR_INVALID_FORMAT);
                break;
            }
            s_mem_blksz = le32(data + 8);
            s_mem_base = le32(data + 12);
            DL_OK(CMD_MEM_BEGIN, 0u);
            break;
        }

        /* ---- MEM_DATA (0x07) ------------------------------------------ */
        case CMD_MEM_DATA:
        {
            /*
             * Data payload layout (per spec):
             *   [0-3]   length of "data to write"
             *   [4-7]   sequence number (0-based)
             *   [8-15]  two zero words (unused)
             *   [16..]  the actual bytes to write
             *
             * Checksum covers only the "data to write" bytes.
             */
            if (size < 16u)
            {
                DL_ERR(op, ROM_ERR_INVALID_FORMAT);
                break;
            }
            uint32_t dlen = le32(data);
            uint32_t seq = le32(data + 4);
            uint8_t *payload = data + 16;

            if (dl_checksum(payload, dlen) != (uint8_t)(chk & 0xFFu))
            {
                DL_ERR(op, ROM_ERR_CHECKSUM);
                break;
            }

            uint8_t *dest = (uint8_t *)(uintptr_t)(s_mem_base + seq * s_mem_blksz);
            memcpy(dest, payload, dlen);
            DL_OK(CMD_MEM_DATA, 0u);
            break;
        }

        /* ---- MEM_END (0x06) ------------------------------------------- */
        case CMD_MEM_END:
        {
            /*
             * esptool packs: struct.pack("<II", int(entry == 0), entry)
             *   execute == 0  →  jump to entry_point (run stub)
             *   execute == 1  →  do not execute (stay in loader / reboot)
             */
            if (size < 8u)
            {
                DL_ERR(op, ROM_ERR_INVALID_FORMAT);
                break;
            }
            uint32_t execute = le32(data);
            uint32_t entry = le32(data + 4);

            DL_OK(CMD_MEM_END, 0u);
            uart_tx_wait_idle(0); /* flush TX FIFO before handing off */

            if (execute == 0u && entry != 0u)
            {
                return entry; /* caller jumps to stub entry point */
            }
            break;
        }

        default:
            DL_ERR(op, ROM_ERR_INVALID_FORMAT);
            break;
        }
    }
}

/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{

    if ((*CUSTOM_FLAG_ADDR) == CUSTOM_DOWNLOAD_MAGIC_WORD)
    {
        ESP_LOGW(TAG, "magic set; entering dl");
        (*CUSTOM_FLAG_ADDR) = 0; // Clear the magic word

        /* Disable RTC watchdog so it cannot interrupt the download session */
        REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1);
        WRITE_PERI_REG(RTC_CNTL_WDTCONFIG0_REG, 0);

        /*
         * Run the esptool-compatible SLIP download protocol.
         * Blocks until MEM_END is received with a non-zero entry point,
         * then returns that address so we can jump into the uploaded stub.
         */
        uint32_t stub_entry = dl_run_protocol();

        if (stub_entry != 0u)
        {
            /* Jump to stub; the stub signals readiness by sending "OHAI" */
            void (*stub_fn)(void) = (void (*)(void))(uintptr_t)stub_entry;
            stub_fn();
        }

        /* stub_entry == 0 or stub returned unexpectedly – reset */
        bootloader_reset();
    }

    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK)
    {
        bootloader_reset();
    }

    // Init board GPIOs, must be done to prevent back powering peripherals
    bootloader_user_gpio_init();

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    // If this boot is a wake up from the deep sleep then go to the short way,
    // try to load the application which worked before deep sleep.
    // It skips a lot of checks due to it was done before (while first boot).
    bootloader_utility_load_boot_image_from_deep_sleep();
    // If it is not successful try to load an application as usual.
#endif

    // 2. Select the number of boot partition
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX)
    {
        bootloader_reset();
    }

    // 2.1 Print a custom message!
    esp_rom_printf("[%s] %s\n", TAG, "Test");

    // 3. Load the app image for booting
    bootloader_utility_load_boot_image(&bs, boot_index);
}

// Select the number of boot partition
static int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs))
    {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return bootloader_utility_get_selected_boot_partition(bs);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif