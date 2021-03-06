/**
 * \file
 * \brief ARM pl011 UART kernel-level driver. 
 */

/*
 * Copyright (c) 2007, 2008, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <dev/pl011_uart_dev.h>
#include <arch/arm/pl011.h>
#include <kernel.h>
#include <paging_kernel_arch.h>
#include <arch/arm/arm.h>
#include <platform.h>
#include <serial.h>

// We can provide a maximum of 6 UARTs
#define MAX_PORTS 6

// How big is each PL011 in address space?
#define UART_DEVICE_BYTES	0x4c

static pl011_uart_t uarts[ MAX_PORTS ];

// Mask all interrupts in the IMSC register
#define INTERRUPTS_MASK		0

#define MSG(format, ...) printk( LOG_NOTE, "pl011: "format, ## __VA_ARGS__ )

/** 
 * \brief Configure the serial interface, from a caller that knows
 * that this is a bunch of PL011s, and furthermore where they are in
 * the physical address space. 
 */
errval_t serial_early_init(unsigned n)
{
    assert(!paging_mmu_enabled());
    assert(n < serial_num_physical_ports);

    pl011_uart_initialize(&uarts[n], (mackerel_addr_t)uart_base[n]);

    // Make sure that the UART is enabled and transmitting - not all platforms
    // do this for us.
    pl011_uart_CR_txe_wrf(&uarts[n], 1);
    pl011_uart_CR_uarten_wrf(&uarts[n], 1);

    return SYS_ERR_OK;
}

/*
 * \brief Initialize a serial port.  The MMU is turned on.
 */
void pl011_init(unsigned port, lvaddr_t base, bool hwinit)
{
    assert(paging_mmu_enabled());
    assert(port < serial_num_physical_ports);

    pl011_uart_t *u = &uarts[port];

    // [Re]initialize the Mackerel state for the UART
    pl011_uart_initialize(u, (mackerel_addr_t) base);

    if (hwinit) {
	// Mask all interrupts: set all bits to zero
	pl011_uart_IMSC_rawwr(u, INTERRUPTS_MASK);
	
	// Configure port to 38400 baud, 8 data, no parity, 1 stop (8-N-1)
	//
	// (This is a mild scam as system is running in QEMU)
	//
	// Note baud rate changes not committed in h/w until lcr_h
	// written.
	pl011_uart_IBRD_divint_wrf(u, 0xc); // Assuming UARTCLK is 7.3728MHz
	pl011_uart_FBRD_divfrac_wrf(u, 0);
	
	// Set 8 bits, no parity
	pl011_uart_LCR_H_t lcr = (pl011_uart_LCR_H_t)0;
	pl011_uart_LCR_H_fen_insert(lcr, 1);
	pl011_uart_LCR_H_wlen_insert(lcr, pl011_uart_bits8);
	pl011_uart_LCR_H_wr(u, lcr);
    }
}

/*
 * \brief Put a character to the port
 */
void serial_putchar(unsigned port, char c) 
{
    assert(port < MAX_PORTS );
    pl011_uart_t *u = &uarts[port];
    assert(u->base != 0);

    while (pl011_uart_FR_txff_rdf(u) == 1)
        ;
    pl011_uart_DR_rawwr(u, c);
    // pl011_uart_DR_t dr;
    // dr = (pl011_uart_DR_t)0;
    // pl011_uart_DR_data_insert(dr, (uint8_t)c);
};


/*
 * \brief Read a character from a port
 */
char serial_getchar(unsigned port)
{
    assert(port < MAX_PORTS );
    pl011_uart_t *u = &uarts[port];
    assert(u->base != 0);

    while (pl011_uart_FR_rxfe_rdf(u) == 1)
        ;
    return (char) pl011_uart_DR_data_rdf(u);
};
