/**
 * \file
 * \brief Kernel serial driver for the OMAP44xx UARTs.  
 */

/*
 * Copyright (c) 2012-2015, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <omap_uart.h>
#include <dev/omap/omap_uart_dev.h>
#include <kernel.h>
#include <arm.h>
#include <paging_kernel_arch.h>
#include <platform.h>
#include <serial.h>

//
// Serial console and debugger interfaces
//
#define NUM_PORTS 8
static omap_uart_t ports[NUM_PORTS];

static void omap_uart_hw_init(omap_uart_t *uart);

#define MSG(port, format, ...) \
    printk( LOG_NOTE, "OMAP serial[%d]: "format, port, ## __VA_ARGS__ )

/*
 * Initialize UARTs before the MMU is on.
 */
errval_t serial_early_init(unsigned port)
{
    assert(!paging_mmu_enabled());
    assert(port < serial_num_physical_ports);
    omap_uart_initialize(&ports[port], (mackerel_addr_t)uart_base[port]);
    omap_uart_hw_init(&ports[port]);
    return SYS_ERR_OK;
}

/*
 * Re-initialize UARTs after the MMU is on.
 */
void omap_uart_init(unsigned port, lvaddr_t base, bool initialize_hw)
{
    assert(paging_mmu_enabled());
    assert(port < serial_num_physical_ports);
    omap_uart_initialize(&ports[port], (mackerel_addr_t)base);
    if (initialize_hw) omap_uart_hw_init(&ports[port]);
}

/*
 * Initialzie OMAP UART hardware
 * UART TRM 23.3
 */
static void omap_uart_hw_init(omap_uart_t *uart)
{
    // do soft reset
    omap_uart_SYSC_softreset_wrf(uart, 0x1);
    while (!omap_uart_SYSS_resetdone_rdf(uart)); // poll for reset completion

    // disable UART access to DLL and DLH
    omap_uart_MDR1_mode_select_wrf(uart, 0x7);
    // register config mode B (according to 23.3.5.1.1.3, step 10-13)
    omap_uart_LCR_wr(uart, 0xbf);
    // 115200 baud
    omap_uart_DLL_clock_lsb_wrf(uart, 0x1a);
    omap_uart_DLH_clock_msb_wrf(uart, 0x0);
    // no enhanced mode
    omap_uart_EFR_enhanced_en_wrf(uart, 0);
    // Set line to 8N1
    omap_uart_LCR_t lcr = omap_uart_LCR_default;
    lcr = omap_uart_LCR_parity_en_insert(lcr, 0);       // No parity
    lcr = omap_uart_LCR_nb_stop_insert(lcr, 0);         // 1 stop bit
    lcr = omap_uart_LCR_char_length_insert(lcr, omap_uart_wl_8bits); // 8 data bits
    omap_uart_LCR_wr(uart, lcr); // this also sets register config mode 'operational'
    // load UART mode (16x?)
    omap_uart_MDR1_mode_select_wrf(uart, 0x0);
}

/**
 * \brief Prints a single character to a serial port. 
 */
void serial_putchar(unsigned port, char c)
{
    assert(port <= NUM_PORTS);
    omap_uart_t *uart = &ports[port];

    // Wait until FIFO can hold more characters
    while(!omap_uart_LSR_tx_fifo_e_rdf(uart));

    // Write character
    omap_uart_THR_thr_wrf(uart, c);
}

/** 
 * \brief Reads a single character from the default serial port.
 * This function spins waiting for a character to arrive.
 */
char serial_getchar(unsigned port)
{
    assert(port <= NUM_PORTS);
    omap_uart_t *uart = &ports[port];

    // Wait until there is at least one character in the FIFO
    while(!omap_uart_LSR_rx_fifo_e_rdf(uart));

    // Return the character
    return omap_uart_RHR_rhr_rdf(uart);
}
