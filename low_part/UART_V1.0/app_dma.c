/********************************************************************************************************
 * @file    app_dma.c
 *
 * @brief   This is the source file for Telink RISC-V MCU
 *
 * @author  Driver Group
 * @date    2019
 *
 * @par     Copyright (c) 2019, Telink Semiconductor (Shanghai) Co., Ltd. ("TELINK")
 *
 *          Licensed under the Apache License, Version 2.0 (the "License");
 *          you may not use this file except in compliance with the License.
 *          You may obtain a copy of the License at
 *
 *              http://www.apache.org/licenses/LICENSE-2.0
 *
 *          Unless required by applicable law or agreed to in writing, software
 *          distributed under the License is distributed on an "AS IS" BASIS,
 *          WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *          See the License for the specific language governing permissions and
 *          limitations under the License.
 *
 *******************************************************************************************************/
#include "common.h"
#include <complex.h>

#define UART0_MODULE 0
#define UART1_MODULE 1

#define UART_MODULE_SEL UART0_MODULE

#define UART_MASTER_DEVICE   1
#define UART_SLAVE_DEVICE    2
#define UART_DEVICE          UART_SLAVE_DEVICE

#define UART_DMA_CHANNEL_RX  DMA2
#define UART_DMA_CHANNEL_TX  DMA3

#define BUFF_DATA_LEN        256

#define DMA_REV_LEN BUFF_DATA_LEN

/*
 * Ping-pong TX buffer — zero-copy DMA.
 * Producer writes to tx_buf[tx_wr_idx]. When buffer fills or flush is
 * requested, DMA starts on it and writer switches to the other half.
 */
#define TX_BUF_SIZE (4096)  /* must be power of 2 and multiple of 4 */

static unsigned char tx_buf[2][TX_BUF_SIZE] __attribute__((aligned(4)));
static volatile int  tx_wr_idx   = 0;
static volatile int  tx_wr_pos   = 0;
static volatile int  tx_dma_busy = 0;

unsigned char rec_buff[BUFF_DATA_LEN] __attribute__((aligned(4))) = {0};
volatile unsigned int rev_data_len = 0;

static unsigned int tx_last_us_tick = 0;

/* ─── internal: flush current write buffer, swap to other ─── */

static int tx_flush_one(void)
{
    int len = tx_wr_pos;

    if (len == 0) return 0;

    uart_send_dma(UART_MODULE_SEL, tx_buf[tx_wr_idx], len);
    tx_dma_busy = 1;
    tx_wr_idx  ^= 1;
    tx_wr_pos   = 0;
    return 1;
}

/* ─── public write ─── */

void uart_tx_write_byte(unsigned char byte)
{
    if (tx_wr_pos >= TX_BUF_SIZE) {
        if (tx_dma_busy) return;  /* both buffers busy — drop */
        tx_flush_one();
    }
    tx_buf[tx_wr_idx][tx_wr_pos++] = byte;
}

void uart_tx_write_buf(const unsigned char *data, unsigned int len)
{
    unsigned int room, words, i;
    uint32_t *dst;
    const uint32_t *src;

    while (len > 0) {
        room = TX_BUF_SIZE - tx_wr_pos;
        if (room >= len) {
            dst   = (uint32_t *)&tx_buf[tx_wr_idx][tx_wr_pos];
            src   = (const uint32_t *)data;
            words = len >> 2;
            for (i = 0; i < words; i++) dst[i] = src[i];
            for (i = words << 2; i < len; i++)
                tx_buf[tx_wr_idx][tx_wr_pos + i] = data[i];
            tx_wr_pos += len;
            return;
        }
        if (room > 0) {
            dst   = (uint32_t *)&tx_buf[tx_wr_idx][tx_wr_pos];
            src   = (const uint32_t *)data;
            words = room >> 2;
            for (i = 0; i < words; i++) dst[i] = src[i];
            for (i = words << 2; i < room; i++)
                tx_buf[tx_wr_idx][tx_wr_pos + i] = data[i];
            tx_wr_pos += room;
            data += room;
            len  -= room;
        }
        if (tx_dma_busy) return;
        tx_flush_one();
    }
}

unsigned int uart_tx_ring_used(void)
{
    return tx_wr_pos + (tx_dma_busy ? TX_BUF_SIZE : 0);
}

void uart_tx_try_send(void)
{
    if (tx_dma_busy) return;
    if (tx_wr_pos == 0) return;
    tx_flush_one();
}

void uart_tx_poll(void)
{
    unsigned int now_tick, now_us;

    if (tx_dma_busy) return;
    if (tx_wr_pos == 0) return;

    now_tick = stimer_get_tick();
    now_us   = now_tick / SYSTEM_TIMER_TICK_1US;

    if (now_us != tx_last_us_tick || uart_tx_ring_used() >= (TX_BUF_SIZE / 2)) {
        tx_last_us_tick = now_us;
        tx_flush_one();
    }
}

void uart_tx_flush(void)
{
    while (tx_wr_pos > 0 || tx_dma_busy) {
        if (!tx_dma_busy && tx_wr_pos > 0)
            tx_flush_one();
        /* busy-wait for DMA completion */
    }
}

#include "gpio_event.h"
void user_init(void)
{
    unsigned short div;
    unsigned char  bwpc;
    gpio_function_en(LED1);
    gpio_output_en(LED1);
    gpio_input_dis(LED1);
    gpio_function_en(LED2);
    gpio_output_en(LED2);
    gpio_input_dis(LED2);
    gpio_function_en(LED3);
    gpio_output_en(LED3);
    gpio_input_dis(LED3);
    gpio_function_en(LED4);
    gpio_output_en(LED4);
    gpio_input_dis(LED4);
    uart_hw_fsm_reset(UART_MODULE_SEL);
    uart_set_pin(UART_MODULE_SEL, UART0_TX_PIN, UART0_RX_PIN);
    uart_cal_div_and_bwpc(3000000, sys_clk.pclk * 1000 * 1000, &div, &bwpc);
    uart_set_rx_timeout(UART_MODULE_SEL, bwpc, 12, UART_BW_MUL2);
    uart_init(UART_MODULE_SEL, div, bwpc, UART_PARITY_NONE, UART_STOP_BIT_ONE);
    uart_set_tx_dma_config(UART_MODULE_SEL, UART_DMA_CHANNEL_TX);
    uart_set_rx_dma_config(UART_MODULE_SEL, UART_DMA_CHANNEL_RX);

    uart_set_irq_mask(UART_MODULE_SEL, UART_TXDONE_MASK);
    plic_interrupt_enable(IRQ_UART0);
    core_interrupt_enable();

    uart_set_irq_mask(UART_MODULE_SEL, UART_RXDONE_MASK);
    uart_receive_dma(UART_MODULE_SEL, (unsigned char *)rec_buff, DMA_REV_LEN);

    tx_last_us_tick = stimer_get_tick() ;

    gpio_event_init();
}

void user_uart_send_byte(uint8_t byte)
{
    uart_tx_write_byte(byte);
}

void user_uart_flush(void)
{
    uart_tx_flush();
}

uint32_t user_timer_ticks(void)
{
    return stimer_get_tick();
}

void user_critical_enter(void)
{

}
void user_critical_exit(void)
{

}
void main_loop(void)
{
    unsigned long t = stimer_get_tick();
  
    gpio_set_high_level(LED1);
    for(int i = 0 ;i < 1; i++){
        gpio_event_toggle(i);
    }
    gpio_set_low_level(LED1);

    gpio_set_high_level(LED2);
    // for(int i = 0 ;i < 8; i++){
    //     uint8_t data[] = "value";
    //     int mode = (i < 4) ? 0 : 1;
    //     gpio_event_send_string(i, mode, (uint8_t *)"lable:", 6, data, 5);
    // }
    gpio_set_low_level(LED2);
    // delay_ms(10);
    uart_tx_poll();

    while (!clock_time_exceed(t, 15)) {
    }
}

_attribute_ram_code_sec_ void uart0_irq_handler(void)
{
    if (uart_get_irq_status(UART_MODULE_SEL, UART_TXDONE_IRQ_STATUS))
    {
        gpio_set_high_level(LED3);
    
        uart_clr_irq_status(UART_MODULE_SEL, UART_TXDONE_IRQ_STATUS);
        tx_dma_busy = 0;
        uart_tx_try_send();
        gpio_set_low_level(LED3);
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
