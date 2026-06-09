/*
 * gpio_event.h — MCU-side UART_VCD Event Protocol v2 encoder
 *
 * 24MHz system clock, raw tick delta, fixed-size header encoding.
 * Compatible with DSView uart-vcd EVENT protocol v2 driver.
 *
 * TX path uses a 16KB ring buffer + DMA for non-blocking transmission.
 *
 * Usage:
 *   1. Implement the hardware abstraction callbacks below.
 *   2. Call gpio_event_init() once at startup.
 *   3. Call gpio_event_high(ch)/low(ch)/toggle(ch)/write(mask) to
 *      record GPIO state changes. Events are auto-queued to ring buffer.
 *   4. Call gpio_event_tx() periodically to flush DMA.
 */

#ifndef GPIO_EVENT_H
#define GPIO_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Configuration ─── */

#define GPIO_EVENT_SYS_CLOCK_HZ    24000000
#define GPIO_EVENT_MAX_CHANNELS    32
#define GPIO_EVENT_GPIO_MAX        23
#define GPIO_EVENT_UART_OFFSET     24
#define GPIO_EVENT_UART_BAUD       1000000
#define GPIO_EVENT_UART_NUM        0

#define GPIO_EVENT_UART_MARKER     0x80

#include "common.h"

extern uint32_t user_timer_ticks(void);   /* 1us ticks (for init only) */
extern void user_critical_enter(void);
extern void user_critical_exit(void);

/* ─── Public API ─── */

void gpio_event_init(void);

/* Reset the tick base — call before burst to avoid large first delta */
void gpio_event_reset_timer(void);

/* GPIO channels 0-23: set level or toggle */
void gpio_event_high(int channel);
void gpio_event_low(int channel);
void gpio_event_toggle(int channel);
void gpio_event_write(uint32_t mask, uint32_t value);

/* String channels RX0-RX7: send text data
 * render_mode: 0=hex, 1=ascii
 * label_bytes + data_bytes packed in payload */
void gpio_event_send_string(int channel, int render_mode,
                            const uint8_t *label, int label_len,
                            const uint8_t *data,  int data_len);

/* Flush DMA — call in main loop or from timer */
void gpio_event_tx(void);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_EVENT_H */
