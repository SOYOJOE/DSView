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

#define UART_TX_RING_SIZE    (16 * 1024)
#define UART_TX_RING_MASK    (UART_TX_RING_SIZE - 1)

#define UART_TX_DMA_BUF_SIZE 4096

static unsigned char uart_tx_ring[UART_TX_RING_SIZE] __attribute__((aligned(4)));
static unsigned char uart_tx_dma_buf[UART_TX_DMA_BUF_SIZE] __attribute__((aligned(4)));
static volatile unsigned int tx_ring_head = 0;
static volatile unsigned int tx_ring_tail = 0;
static volatile unsigned char tx_dma_busy = 0;
static volatile unsigned int  tx_dma_len  = 0;

unsigned char tx_byte_buff[16] __attribute__((aligned(4))) = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
unsigned char rec_buff[BUFF_DATA_LEN] __attribute__((aligned(4))) = {0};

volatile unsigned int rev_data_len  = 0;

static unsigned int tx_last_us_tick = 0;

void uart_tx_write_byte(unsigned char byte)
{
    unsigned int next_head = (tx_ring_head + 1) & UART_TX_RING_MASK;
    if (next_head != tx_ring_tail) {
        uart_tx_ring[tx_ring_head] = byte;
        tx_ring_head = next_head;
    }
}

void uart_tx_write_buf(const unsigned char *data, unsigned int len)
{
    for (unsigned int i = 0; i < len; i++) {
        unsigned int next_head = (tx_ring_head + 1) & UART_TX_RING_MASK;
        if (next_head != tx_ring_tail) {
            uart_tx_ring[tx_ring_head] = data[i];
            tx_ring_head = next_head;
        }
    }
}

unsigned int uart_tx_ring_used(void)
{
    return (tx_ring_head - tx_ring_tail) & UART_TX_RING_MASK;
}

static void uart_tx_start_dma(void)
{
    unsigned int head  = tx_ring_head;
    unsigned int tail  = tx_ring_tail;
    unsigned int len;

    if (head == tail) return;

    if (head > tail) {
        len = head - tail;
    } else {
        len = UART_TX_RING_SIZE - tail;
    }

    if (len > UART_TX_DMA_BUF_SIZE) {
        len = UART_TX_DMA_BUF_SIZE;
    }
    if (tail + len <= UART_TX_RING_SIZE) {
        memcpy(uart_tx_dma_buf, &uart_tx_ring[tail], len);
    } else {
        unsigned int first = UART_TX_RING_SIZE - tail;
        memcpy(uart_tx_dma_buf, &uart_tx_ring[tail], first);
        memcpy(uart_tx_dma_buf + first, uart_tx_ring, len - first);
    }
    tx_dma_busy = 1;
    tx_dma_len  = len;
    uart_send_dma(UART_MODULE_SEL, uart_tx_dma_buf, len);
}

void uart_tx_try_send(void)
{
    if (tx_dma_busy) return;
    if (tx_ring_head == tx_ring_tail) return;
    uart_tx_start_dma();
}

void uart_tx_poll(void)
{
    unsigned int now_tick;
    unsigned int now_us;

    if (tx_dma_busy) return;
    if (tx_ring_head == tx_ring_tail) return;

    now_tick = stimer_get_tick();
    now_us   = now_tick / SYSTEM_TIMER_TICK_1US;

    if (now_us != tx_last_us_tick || uart_tx_ring_used() >= (UART_TX_RING_SIZE / 2)) {
        tx_last_us_tick = now_us;
        uart_tx_start_dma();
    }
}

void uart_tx_flush(void)
{
    while (tx_ring_head != tx_ring_tail) {
        uart_tx_start_dma();
        while (tx_dma_busy);
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
    uart_cal_div_and_bwpc(1000000, sys_clk.pclk * 1000 * 1000, &div, &bwpc);
    uart_set_rx_timeout(UART_MODULE_SEL, bwpc, 12, UART_BW_MUL2);
    uart_init(UART_MODULE_SEL, div, bwpc, UART_PARITY_NONE, UART_STOP_BIT_ONE);
    uart_set_tx_dma_config(UART_MODULE_SEL, UART_DMA_CHANNEL_TX);
    uart_set_rx_dma_config(UART_MODULE_SEL, UART_DMA_CHANNEL_RX);

    uart_set_irq_mask(UART_MODULE_SEL, UART_TXDONE_MASK);
    plic_interrupt_enable(IRQ_UART0);
    core_interrupt_enable();

    uart_set_irq_mask(UART_MODULE_SEL, UART_RXDONE_MASK);
    uart_receive_dma(UART_MODULE_SEL, (unsigned char *)rec_buff, DMA_REV_LEN);

    tx_last_us_tick = stimer_get_tick() / SYSTEM_TIMER_TICK_1US;

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
    return stimer_get_tick() / SYSTEM_TIMER_TICK_1US;
}

void user_critical_enter(void)
{

}
void user_critical_exit(void)
{

}
void main_loop(void)
{
    gpio_event_high(8);
    gpio_set_high_level(LED2);
    gpio_set_high_level(LED1);
    gpio_set_low_level(LED2);
    for(int i = 0 ;i < 24; i++){
        gpio_event_toggle(i);
    }
    gpio_set_low_level(LED1);
    gpio_event_low(8);
    uint8_t data[1] = {'a'};
    // gpio_set_high_level(LED2);
    gpio_event_high(9);
    for(int i = 0; i < 8; i++){
        gpio_event_send_data(i,1,data);
        data[0]++;
    }
    // gpio_set_low_level(LED2);
    gpio_event_low(9);

    delay_us(1000);
    uart_tx_poll();
}

_attribute_ram_code_sec_ void uart0_irq_handler(void)
{
    if (uart_get_irq_status(UART_MODULE_SEL, UART_TXDONE_IRQ_STATUS))
    {
        gpio_set_high_level(LED3);
        uart_clr_irq_status(UART_MODULE_SEL, UART_TXDONE_IRQ_STATUS);
        tx_ring_tail = (tx_ring_tail + tx_dma_len) & UART_TX_RING_MASK;
        tx_dma_busy  = 0;
        tx_dma_len   = 0;
        uart_tx_start_dma();
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
