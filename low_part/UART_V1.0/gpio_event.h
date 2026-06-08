/*
 * gpio_event.h — MCU-side GPIO Event + Delta Time protocol encoder
 *
 * 24MHz system clock, timer-based tick, varint-encoded UART output.
 * Compatible with DSView uart-vcd EVENT protocol driver.
 *
 * TX path uses a 16KB ring buffer + DMA for non-blocking transmission.
 * Same-microsecond events are automatically batched into one DMA transfer.
 *
 * Usage:
 *   1. Implement the hardware abstraction callbacks below.
 *   2. Call gpio_event_init() once at startup.
 *   3. Call gpio_event_high(ch)/low(ch)/toggle(ch)/write(mask) to
 *      record GPIO state changes. Events are auto-queued to ring buffer.
 *   4. Call gpio_event_poll() periodically for sync + DMA flush.
 */

#ifndef GPIO_EVENT_H
#define GPIO_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_EVENT_SYS_CLOCK_HZ    24000000
#define GPIO_EVENT_TICK_NS         1000
#define GPIO_EVENT_MAX_CHANNELS    32
#define GPIO_EVENT_GPIO_MAX        23
#define GPIO_EVENT_UART_OFFSET     24
#define GPIO_EVENT_SYNC_INTERVAL   100000000
#define GPIO_EVENT_UART_BAUD       1000000
#define GPIO_EVENT_UART_NUM        0
#define GPIO_EVENT_UART_DECODE_BAUD 115200
#define GPIO_EVENT_UART_MARKER      0x80000000

#define GPIO_EVENT_TIMER_PSC \
    ((GPIO_EVENT_SYS_CLOCK_HZ) / (1000000000UL / (GPIO_EVENT_TICK_NS)) - 1)

#define GPIO_EVENT_TICKS_TO_NS(t)   ((uint64_t)(t) * (GPIO_EVENT_TICK_NS))
#define GPIO_EVENT_NS_TO_TICKS(ns)  ((uint64_t)(ns) / (GPIO_EVENT_TICK_NS))

#include "common.h"

extern uint32_t user_timer_ticks(void);
extern void user_critical_enter(void);
extern void user_critical_exit(void);

void gpio_event_init(void);
void gpio_event_high(int channel);
void gpio_event_low(int channel);
void gpio_event_toggle(int channel);
void gpio_event_write(uint32_t mask, uint32_t value);
void gpio_event_send_uart_byte(uint8_t byte);
void gpio_event_send_data(int channel, int len, const uint8_t *data);
void gpio_event_sync(void);
void gpio_event_poll(void);
void gpio_event_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_EVENT_H */
