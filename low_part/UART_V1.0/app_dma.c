/********************************************************************************************************
 * @file    app_dma.c
 *
 * @brief   UART_VCD firmware — platform init + main loop (Telink RISC-V MCU).
 *          Event encoding, TX ring buffer, and ISR are in gpio_event.c.
 *
 * @author  Telink + DSView
 * @date    2019-2026
 *******************************************************************************************************/
#include "gpio_event.h"

unsigned char rec_buff[256] __attribute__((aligned(4))) = {0};
volatile unsigned int rev_data_len = 0;

/* ─── Platform init ─── */

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

    uart_hw_fsm_reset(0);
    uart_set_pin(0, UART0_TX_PIN, UART0_RX_PIN);
    uart_cal_div_and_bwpc(3000000, sys_clk.pclk * 1000 * 1000, &div, &bwpc);
    uart_set_rx_timeout(0, bwpc, 12, UART_BW_MUL2);
    uart_init(0, div, bwpc, UART_PARITY_NONE, UART_STOP_BIT_ONE);
    uart_set_tx_dma_config(0, DMA3);
    uart_set_rx_dma_config(0, DMA2);

    uart_set_irq_mask(0, UART_TXDONE_MASK);
    plic_interrupt_enable(IRQ_UART0);
    core_interrupt_enable();

    uart_set_irq_mask(0, UART_RXDONE_MASK);
    uart_receive_dma(0, (unsigned char *)rec_buff, sizeof(rec_buff));

    gpio_event_init();
    gpio_event_send_label(0, (const uint8_t *)"Ra0:", 4);
    gpio_event_send_label(1, (const uint8_t *)"RX1:", 4);
    gpio_event_send_label(2, (const uint8_t *)"RX2:", 4);
    gpio_event_send_label(3, (const uint8_t *)"RX3:", 4);
    gpio_event_send_label(4, (const uint8_t *)"RX4:", 4);
    gpio_event_send_label(5, (const uint8_t *)"RX5:", 4);
    gpio_event_send_label(6, (const uint8_t *)"RX6:", 4);
    gpio_event_send_label(7, (const uint8_t *)"Ra7:", 4);
}

/* ─── Main loop ─── */

void main_loop(void)
{
    unsigned long t = stimer_get_tick();

    gpio_event_send_sync();

    gpio_set_high_level(LED1);
    for (int i = 0; i < 24; i++) {
        gpio_event_toggle(i);
    }
    gpio_set_low_level(LED1);

    gpio_set_high_level(LED2);
    // static uint8_t data[] = {0, 'a', 'l', 'u', 'e'};
    // for (int i = 0; i < 8; i++) {
    //     int mode = (i < 4) ? GPIO_EVENT_RENDER_MODE_HEX : GPIO_EVENT_RENDER_MODE_ASCII;
    //     gpio_event_send_text(i, mode, data, 5);
    // }
    // data[0]++;
    gpio_set_low_level(LED2);

    uart_tx_poll();

    while (!clock_time_exceed(t, 500)) {
    }
}
