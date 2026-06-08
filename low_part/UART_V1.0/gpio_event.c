/*
 * gpio_event.c — MCU-side GPIO Event + Delta Time protocol encoder
 *
 * Wire protocol:
 *   [varint delta_time] [varint toggle_mask] ...
 *   delta_time=0 → sync: [0x00] [state_LE_4B]
 *
 * Varint encoding (LE, 7 bits per byte, MSB=continuation):
 *   while value > 0x7F: emit (value & 0x7F) | 0x80; value >>= 7
 *   emit value & 0x7F
 *
 * TX path: all output goes through the ring buffer (uart_tx_write_byte),
 *          DMA sends data in bulk — no blocking uart_send_byte calls.
 *
 * All timing uses uint32_t tick subtraction for natural wraparound handling.
 * user_timer_ticks() returns microseconds (stimer_get_tick() / 24).
 */

#include "gpio_event.h"

extern void uart_tx_write_byte(unsigned char byte);
extern void uart_tx_write_buf(const unsigned char *data, unsigned int len);
extern void uart_tx_try_send(void);

#define SYNC_INTERVAL_US  (GPIO_EVENT_SYNC_INTERVAL / GPIO_EVENT_TICK_NS)

static uint32_t  g_gpio_state;
static uint32_t  g_last_ticks;
static uint32_t  g_last_sync_tick;
static int       g_initialized;

static void varint_encode(uint32_t value, uint8_t *buf, uint8_t *out_len)
{
    uint8_t *p = buf;
    while (value > 0x7F) {
        *p++ = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    *p++ = (uint8_t)(value & 0x7F);
    *out_len = (uint8_t)(p - buf);
}

static void varint_send(uint32_t value)
{
    uint8_t buf[5];
    uint8_t len;
    varint_encode(value, buf, &len);
    uart_tx_write_buf(buf, len);
}

static void send_event(uint32_t delta_ticks, uint32_t toggle_mask)
{
    varint_send(delta_ticks);
    varint_send(toggle_mask);
}

static void record_transition(uint32_t new_state)
{
    uint32_t toggle_mask;
    uint32_t delta_ticks;
    uint32_t now;

    if (!g_initialized) return;

    user_critical_enter();
    now = user_timer_ticks();
    delta_ticks = (uint32_t)(now - g_last_ticks);
    // if (delta_ticks == 0) delta_ticks = 1;
    toggle_mask = g_gpio_state ^ new_state;

    if (toggle_mask == 0) {
        user_critical_exit();
        return;
    }

    g_gpio_state = new_state;
    g_last_ticks = now;
    user_critical_exit();

    send_event(delta_ticks, toggle_mask);
}

void gpio_event_init(void)
{
    g_gpio_state    = 0;
    g_last_ticks    = user_timer_ticks();
    g_last_sync_tick = g_last_ticks;
    g_initialized   = 1;

    gpio_event_sync();
}

void gpio_event_high(int channel)
{
    if ((uint32_t)channel > GPIO_EVENT_GPIO_MAX) return;
    record_transition(g_gpio_state | (1u << (uint32_t)channel));
}

void gpio_event_low(int channel)
{
    if ((uint32_t)channel > GPIO_EVENT_GPIO_MAX) return;
    record_transition(g_gpio_state & ~(1u << (uint32_t)channel));
}

void gpio_event_toggle(int channel)
{
    if ((uint32_t)channel > GPIO_EVENT_GPIO_MAX) return;
    record_transition(g_gpio_state ^ (1u << (uint32_t)channel));
}

void gpio_event_write(uint32_t mask, uint32_t value)
{
    mask &= ((1u << (GPIO_EVENT_GPIO_MAX + 1)) - 1);
    value &= mask;
    record_transition((g_gpio_state & ~mask) | value);
}

void gpio_event_send_uart_byte(uint8_t byte)
{
    uint32_t mask = (uint32_t)0xFF << GPIO_EVENT_UART_OFFSET;
    uint32_t new_state = (g_gpio_state & ~mask) | (((uint32_t)byte) << GPIO_EVENT_UART_OFFSET);
    record_transition(new_state);
}

void gpio_event_send_data(int channel, int len, const uint8_t *data)
{
    uint32_t delta_ticks;
    uint32_t now;
    uint32_t toggle;

    if (!g_initialized) return;
    if (channel < 0 || channel > 7) return;
    if (len <= 0 || !data) return;

    while (len-- > 0) {
        toggle = GPIO_EVENT_UART_MARKER
               | (((uint32_t)channel & 0x07) << 8)
               | (uint32_t)(*data++);

        user_critical_enter();
        now = user_timer_ticks();
        delta_ticks = (uint32_t)(now - g_last_ticks);
        // if (delta_ticks == 0) delta_ticks = 1;
        g_last_ticks = now;
        user_critical_exit();

        send_event(delta_ticks, toggle);
    }
}

void gpio_event_sync(void)
{
    uint32_t now;
    uint8_t sync_buf[5];

    if (!g_initialized) return;

    user_critical_enter();
    now = user_timer_ticks();

    g_last_sync_tick = now;

    sync_buf[0] = 0x00;
    sync_buf[1] = (uint8_t)(g_gpio_state);
    sync_buf[2] = (uint8_t)(g_gpio_state >> 8);
    sync_buf[3] = (uint8_t)(g_gpio_state >> 16);
    sync_buf[4] = (uint8_t)(g_gpio_state >> 24);
    user_critical_exit();

    uart_tx_write_buf(sync_buf, 5);
}
void gpio_event_poll(void)
{
    uint32_t now;

    if (!g_initialized) return;

    now = user_timer_ticks();
    if ((uint32_t)(now - g_last_sync_tick) >= SYNC_INTERVAL_US) {
        gpio_event_sync();
    }

    uart_tx_try_send();
}

void gpio_event_flush(void)
{
    uart_tx_try_send();
}
