/*
 * gpio_event.c — UART_VCD Event Protocol v2 encoder + TX ring buffer + UART ISR
 *
 * Wire protocol:  [uint24_le delta_ticks][1B header][payload...]
 * TX path:        ping-pong DMA ring buffer (4KB × 2)
 * Clock:          24MHz systimer ticks
 *
 * All hot-path functions are static inline for zero call overhead.
 * Public API functions are non-static, declared in gpio_event.h.
 * LTO (-flto) will inline them into app_dma.c callers.
 */
#include "gpio_event.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UART0_MODULE 0
#define UART_MODULE_SEL UART0_MODULE

#define UART_DMA_CHANNEL_RX  DMA2
#define UART_DMA_CHANNEL_TX  DMA3

#define TX_BUF_SIZE (4096)  /* power of 2, multiple of 4 */

/* ═══════════════════════════════════════════════════════════════════════════
 * SHARED STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

static unsigned char tx_buf[2][TX_BUF_SIZE] __attribute__((aligned(4)));
static volatile int  tx_wr_idx   = 0;
static volatile int  tx_wr_pos   = 0;
static volatile int  tx_dma_busy = 0;
static volatile int  tx_buffer_lock = 0;

static uint32_t g_gpio_state  = 0;
static uint32_t g_last_tick   = 0;
static int      g_initialized = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * PROTOCOL CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GPIO_LOW    0x00
#define GPIO_HIGH   0x20
#define GPIO_TOGGLE 0x40

/* ═══════════════════════════════════════════════════════════════════════════
 * ===  SECTION 1 — TX RING BUFFER (bottom of call chain, defined first) ====
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline int tx_flush_one(void)
{
    int len = tx_wr_pos;
    if (len == 0) return 0;
    uart_send_dma(UART_MODULE_SEL, tx_buf[tx_wr_idx], len);
    tx_dma_busy = 1;
    tx_wr_idx  ^= 1;
    tx_wr_pos   = 0;
    return 1;
}

static inline void uart_tx_write_byte(unsigned char byte)
{
    if (tx_wr_pos >= TX_BUF_SIZE) {
        if (tx_dma_busy) return;
        tx_flush_one();
    }
    tx_buf[tx_wr_idx][tx_wr_pos++] = byte;
}

static inline void uart_tx_write_fast4(const uint8_t data[4])
{
    if (tx_wr_pos + 4 > TX_BUF_SIZE) {
        if (tx_dma_busy) return;
        tx_flush_one();
        if (tx_wr_pos + 4 > TX_BUF_SIZE) return;
    }
    *(uint32_t *)(tx_buf[tx_wr_idx] + tx_wr_pos) = *(const uint32_t *)data;
    tx_wr_pos += 4;
}

static inline void __attribute__((always_inline))
uart_tx_write_fast4_irq(uint32_t dword)
{
    if (tx_buffer_lock || tx_wr_pos + 4 > TX_BUF_SIZE)
        return;
    *(uint32_t *)(tx_buf[tx_wr_idx] + tx_wr_pos) = dword;
    tx_wr_pos += 4;
}

static inline uint8_t *uart_tx_reserve(unsigned int n)
{
    uint8_t *p;

    if (n > TX_BUF_SIZE) return NULL;
    if (tx_wr_pos + n > TX_BUF_SIZE) {
        if (tx_dma_busy) return NULL;
        tx_flush_one();
    }
    p = tx_buf[tx_wr_idx] + tx_wr_pos;
    return p;
}

static inline void uart_tx_commit(unsigned int n)
{
    tx_wr_pos += n;
}

void uart_tx_poll(void)
{
    tx_buffer_lock = 1;
    if (!tx_dma_busy && tx_wr_pos != 0)
        tx_flush_one();
    tx_buffer_lock = 0;
}

static void uart_tx_flush(void)
{
    while (tx_wr_pos > 0 || tx_dma_busy) {
        if (!tx_dma_busy && tx_wr_pos > 0)
            tx_flush_one();
    }
}

/* ─── Trivial user wrappers ─── */

void user_uart_send_byte(uint8_t byte)
{
    uart_tx_write_byte(byte);
}

void user_uart_flush(void)
{
    uart_tx_flush();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ===  SECTION 2 — GPIO EVENT PROTOCOL ENCODER =============================
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void __attribute__((always_inline))
gpio_event_emit(uint32_t new_state, uint8_t sub_mode, int channel)
{
    uint32_t now, delta, dword;

    if (!g_initialized) return;

    now   = stimer_get_tick();
    delta = now - g_last_tick;
    g_last_tick = now;
    g_gpio_state = new_state;

    dword = delta | ((uint32_t)(sub_mode | (uint8_t)channel) << 24);
    uart_tx_write_fast4((const uint8_t *)&dword);
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

static inline void __attribute__((always_inline))
gpio_event_emit_irq(uint32_t new_state, uint8_t sub_mode, unsigned int channel)
{
    uint32_t now = stimer_get_tick();
    uint32_t dword = (now - g_last_tick) |
        ((uint32_t)(sub_mode | (uint8_t)channel) << 24);

    g_last_tick = now;
    g_gpio_state = new_state;
    uart_tx_write_fast4_irq(dword);
}

_attribute_ram_code_sec_ void gpio_event_irq_high(unsigned int channel)
{
    gpio_event_emit_irq(g_gpio_state | (1u << channel), GPIO_HIGH, channel);
}

_attribute_ram_code_sec_ void gpio_event_irq_low(unsigned int channel)
{
    gpio_event_emit_irq(g_gpio_state & ~(1u << channel), GPIO_LOW, channel);
}

_attribute_ram_code_sec_ void gpio_event_irq_toggle(unsigned int channel)
{
    uint32_t mask = 1u << channel;
    if (g_gpio_state & mask)
        gpio_event_emit_irq(g_gpio_state & ~mask, GPIO_LOW, channel);
    else
        gpio_event_emit_irq(g_gpio_state | mask, GPIO_HIGH, channel);
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
    g_last_tick = stimer_get_tick();
}

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

    now   = stimer_get_tick();
    delta = now - g_last_tick;
    g_last_tick = now;

    header = 0x80 | ((uint8_t)(render_mode & 3) << 5) | (uint8_t)(label_len & 0x1F);
    dword = delta | ((uint32_t)header << 24);

    total_len   = (uint8_t)(label_len + data_len);
    payload_len = 2 + total_len;
    block_len   = (payload_len + 3) & ~3;

    p = uart_tx_reserve((unsigned int)(4 + block_len));
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
        uart_tx_commit((unsigned int)(4 + block_len));
    }
}

void gpio_event_tx(void)
{
    uart_tx_poll();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ===  SECTION 3 — UART ISR ================================================
 * ═══════════════════════════════════════════════════════════════════════════ */

_attribute_ram_code_sec_ void uart0_irq_handler(void)
{
    if (uart_get_irq_status(UART_MODULE_SEL, UART_TXDONE_IRQ_STATUS))
    {
        uart_clr_irq_status(UART_MODULE_SEL, UART_TXDONE_IRQ_STATUS);
        tx_dma_busy = 0;
    }

    if (uart_get_irq_status(UART_MODULE_SEL, UART_RXDONE_IRQ_STATUS))
    {
        if ((uart_get_irq_status(UART_MODULE_SEL, UART_RX_ERR))) {
            uart_clr_irq_status(UART_MODULE_SEL, UART_RXBUF_IRQ_STATUS);
        }
        uart_clr_irq_status(UART_MODULE_SEL, UART_RXDONE_IRQ_STATUS);
    }
}
PLIC_ISR_REGISTER(uart0_irq_handler, IRQ_UART0)
