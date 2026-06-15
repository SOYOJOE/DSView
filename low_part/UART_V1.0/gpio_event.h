/*
 * gpio_event.h — MCU-side UART_VCD Event Protocol v2 encoder
 *
 * 24MHz system clock, raw tick delta, fixed-size header encoding.
 * Compatible with DSView uart-vcd EVENT protocol v2 driver.
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
 *   1. Implement user_critical_enter/exit, user_timer_ticks.
 *   2. Call main_loop() — encapsulates all hardware interaction.
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

/* ─── Required platform callbacks ─── */

extern uint32_t user_timer_ticks(void);
extern uint32_t user_critical_enter(void);
extern void user_critical_exit(uint32_t state);

/* ─── Entry points ─── */

extern void user_init(void);
extern void main_loop(void);

/* ─── GPIO event API ─── */

void gpio_event_init(void);
void gpio_event_reset_timer(void);
void gpio_event_high(int channel);
void gpio_event_low(int channel);
void gpio_event_toggle(int channel);
void gpio_event_send_string(int channel, int render_mode,
                            const uint8_t *label, int label_len,
                            const uint8_t *data,  int data_len);
void gpio_event_tx(void);

/* ─── TX ring buffer API ─── */

void uart_tx_poll(void);
void user_uart_send_byte(uint8_t byte);
void user_uart_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_EVENT_H */
