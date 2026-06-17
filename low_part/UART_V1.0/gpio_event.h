/*
 * gpio_event.h — MCU-side UART_VCD Event Protocol v3 encoder
 *
 * 24MHz system clock, raw tick delta, fixed-size header encoding.
 * Compatible with DSView uart-vcd EVENT protocol v3 driver.
 *
 * TX path uses two 4KB ping-pong DMA buffers for non-blocking transmission.
 *
 * Layering:
 *   gpio_event.c — all logic (event encoder, TX ring buffer, UART ISR)
 *   app_dma.c    — platform init + main loop
 *
 * LTO (-flto) will inline gpio_event_emit and hot-path helpers
 * into the callers in app_dma.c.
 *
 * Usage:
 *   1. Call gpio_event_init() after UART/DMA initialization.
 *   2. Use gpio_event_irq_*() from GPIO interrupt handlers.
 */

#ifndef GPIO_EVENT_H
#define GPIO_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Configuration ─── */
enum {
    GPIO_EVENT_RENDER_MODE_HEX = 0,
    GPIO_EVENT_RENDER_MODE_ASCII = 1,
};
#define GPIO_EVENT_SYS_CLOCK_HZ    24000000
#define GPIO_EVENT_MAX_CHANNELS    32
#define GPIO_EVENT_GPIO_MAX        23
#define GPIO_EVENT_UART_OFFSET     24
#define GPIO_EVENT_UART_BAUD       1000000
#define GPIO_EVENT_UART_NUM        0
#define GPIO_EVENT_UART_MARKER     0x80

#include "common.h"

/* ─── Entry points ─── */

extern void user_init(void);
extern void main_loop(void);

/* ─── GPIO event API ─── */

void gpio_event_init(void);
void gpio_event_reset_timer(void);
void gpio_event_high(int channel);
void gpio_event_low(int channel);
void gpio_event_toggle(int channel);

/*
 * Interrupt-only fast path. These functions do no channel validation and must
 * only be called with channel 0..23 from an ISR. Use one event-producing
 * context: while these APIs are active, do not call regular GPIO/string or
 * user_uart_* APIs. The main loop may keep calling uart_tx_poll().
 */
void gpio_event_irq_high(unsigned int channel);
void gpio_event_irq_low(unsigned int channel);
void gpio_event_irq_toggle(unsigned int channel);

void gpio_event_send_label(int channel, const uint8_t *label, int label_len);
void gpio_event_send_sync(void);
void gpio_event_send_text(int channel, int render_mode,
                          const uint8_t *data, int data_len);
void gpio_event_tx(void);

/* ─── TX ring buffer API ─── */

void uart_tx_poll(void);
void user_uart_send_byte(uint8_t byte);
void user_uart_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_EVENT_H */
