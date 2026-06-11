/*
 * gpio_event.c — MCU-side UART_VCD Event Protocol v2 encoder
 *
 * Wire protocol:
 *   [uint24_le delta_ticks] [data_block...]
 *
 * delta_ticks: 3-byte LE, raw 24MHz systimer tick count since last event
 * data_block:  1-byte header + payload, 4-byte aligned
 *
 * Header byte:
 *   bit7:     mode (0=GPIO, 1=string)
 *   bits6-5:  sub-mode (GPIO: low/high/toggle, String: hex/ascii)
 *   bits4-0:  param (GPIO: channel 0-23, String: label_len 0-31)
 *
 * TX path: all output goes through the ring buffer (uart_tx_write_byte),
 *          DMA sends data in bulk.
 *
 * All timing uses raw 24MHz systimer ticks (stimer_get_tick).
 */

#include "gpio_event.h"

extern void uart_tx_write_byte(unsigned char byte);
extern void uart_tx_write_buf(const unsigned char *data, unsigned int len);
extern void uart_tx_write_fast4(const uint8_t data[4]);
extern uint8_t *uart_tx_reserve(unsigned int n);
extern void uart_tx_try_send(void);


/* ─── Internal State ─── */

static uint32_t  g_gpio_state = 0;
static uint32_t  g_last_tick = 0;      /* raw 24MHz systimer tick */
static int       g_initialized = 0;

/* ─── GPIO sub-mode ─── */
#define GPIO_LOW    0x00   /* bits6-5: 00 */
#define GPIO_HIGH   0x20   /* bits6-5: 01 */
#define GPIO_TOGGLE 0x40   /* bits6-5: 10 */

/* ─── String sub-mode ─── */
#define STRING_HEX   0x20   /* bits6-5: 01 (mode=1, sub=00/01) */
#define STRING_ASCII 0x60   /* bits6-5: 11 (mode=1, sub=10/11) */

/* ─── Core inline: sample tick, compute delta, write 4B delta+header ─── */

static inline void __attribute__((always_inline))
gpio_event_emit(uint32_t new_state, uint8_t sub_mode, int channel)
{
    uint32_t now, delta, dword;

    if (!g_initialized) return;

    user_critical_enter();
    now   = stimer_get_tick();
    delta = now - g_last_tick;
    g_last_tick = now;
    g_gpio_state = new_state;
    user_critical_exit();

    dword = delta | ((uint32_t)(sub_mode | (uint8_t)channel) << 24);
    uart_tx_write_fast4((const uint8_t *)&dword);
}


void gpio_event_init(void)
{
    g_gpio_state   = 0;
    g_last_tick    = stimer_get_tick();
    g_initialized  = 1;
}

void gpio_event_reset_timer(void)
{
    if (!g_initialized) return;
    user_critical_enter();
    g_last_tick = stimer_get_tick();
    user_critical_exit();
}

void gpio_event_high(int channel)
{
    if ((uint32_t)channel > GPIO_EVENT_GPIO_MAX) return;
    gpio_event_emit(g_gpio_state | (1u << (uint32_t)channel), GPIO_HIGH, channel);
}

void gpio_event_low(int channel)
{
    if ((uint32_t)channel > GPIO_EVENT_GPIO_MAX) return;
    gpio_event_emit(g_gpio_state & ~(1u << (uint32_t)channel), GPIO_LOW, channel);
}

void gpio_event_toggle(int channel)
{
    uint32_t mask;
    if ((uint32_t)channel > GPIO_EVENT_GPIO_MAX) return;
    mask = 1u << (uint32_t)channel;
    if (g_gpio_state & mask)
        gpio_event_emit(g_gpio_state & ~mask, GPIO_LOW, channel);
    else
        gpio_event_emit(g_gpio_state | mask, GPIO_HIGH, channel);
}

void gpio_event_write(uint32_t mask, uint32_t value)
{
    mask &= ((1u << (GPIO_EVENT_GPIO_MAX + 1)) - 1);
    value &= mask;
    gpio_event_emit((g_gpio_state & ~mask) | value, GPIO_TOGGLE, 0);
}

/* ─── String event: header + payload, 4-byte aligned ─── */

void gpio_event_send_string(int channel, int render_mode,
                            const uint8_t *label, int label_len,
                            const uint8_t *data,  int data_len)
{
    uint32_t now, delta, dword;
    uint8_t  header;
    uint8_t  total_len;
    int      payload_len, block_len;
    uint8_t *p;

    if (!g_initialized) return;
    if (channel < 0 || channel > 7) return;
    if (label_len < 0 || label_len > 31) return;
    if (data_len < 0) return;

    user_critical_enter();
    now   = stimer_get_tick();
    delta = now - g_last_tick;
    g_last_tick = now;
    user_critical_exit();

    header = 0x80 | ((uint8_t)(render_mode & 3) << 5) | (uint8_t)(label_len & 0x1F);
    dword = delta | ((uint32_t)header << 24);

    total_len   = (uint8_t)(label_len + data_len);
    payload_len = 2 + total_len;
    block_len   = (payload_len + 3) & ~3;

    p = uart_tx_reserve(4 + block_len);
    if (p) {
        *(uint32_t *)p = dword;
        p += 4;
        *p++ = (uint8_t)channel;
        *p++ = total_len;
        if (label && label_len > 0) {
            memcpy(p, label, (unsigned int)label_len);
            p += label_len;
        }
        if (data && data_len > 0) {
            memcpy(p, data, (unsigned int)data_len);
            p += data_len;
        }
        {
            int pad = block_len - payload_len;
            if (pad > 0) memset(p, 0, (unsigned int)pad);
        }
        return;
    }

    {
        uint8_t db[4];
        db[0] = (uint8_t)(delta);
        db[1] = (uint8_t)(delta >> 8);
        db[2] = (uint8_t)(delta >> 16);
        db[3] = header;
        uart_tx_write_fast4(db);
    }

    {
        static uint8_t str_buf[300] __attribute__((aligned(4)));
        p = str_buf;
        *p++ = (uint8_t)channel;
        *p++ = total_len;
        if (label && label_len > 0) {
            memcpy(p, label, (unsigned int)label_len);
            p += label_len;
        }
        if (data && data_len > 0) {
            memcpy(p, data, (unsigned int)data_len);
            p += data_len;
        }
        memset(p, 0, (unsigned int)(block_len - payload_len));
        uart_tx_write_buf(str_buf, block_len);
    }
}

/* ─── DMA flush ─── */

void gpio_event_tx(void)
{
    uart_tx_try_send();
}
