/**
 * \file
 * \brief PCI service client-side logic
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2011, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdio.h>
#include <stdlib.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/nameservice_client.h>
#include <barrelfish/dispatch.h>
#include <barrelfish/inthandler.h>
#include <pci/pci.h>
#include <pci/pci_client_debug.h>
#include <if/pci_defs.h>
#include <if/pci_rpcclient_defs.h>
#include <acpi_client/acpi_client.h>

#define INVALID_VECTOR ((uint32_t)-1)

static struct pci_rpc_client *pci_client = NULL;

errval_t pci_reregister_irq_for_device(uint32_t class, uint32_t subclass, uint32_t prog_if,
                                       uint32_t vendor, uint32_t device,
                                       uint32_t bus, uint32_t dev, uint32_t fun,
                                       interrupt_handler_fn handler,
                                       void *handler_arg,
                                       interrupt_handler_fn reloc_handler,
                                       void *reloc_handler_arg)
{
    uint32_t vector = INVALID_VECTOR;
    errval_t err, msgerr;

    if (handler != NULL && reloc_handler != NULL) {
        // register movable interrupt
        err = inthandler_setup_movable(handler, handler_arg, reloc_handler,
                reloc_handler_arg, &vector);
        if (err_is_fail(err)) {
            return err;
        }

        assert(vector != INVALID_VECTOR);
    } else if (handler != NULL) {
        // register non-movable interrupt
        err = inthandler_setup(handler, handler_arg, &vector);
        if (err_is_fail(err)) {
            return err;
        }

        assert(vector != INVALID_VECTOR);
    }

    err = pci_client->vtbl.
        reregister_interrupt(pci_client, class, subclass, prog_if, vendor,
                device, bus, dev, fun, disp_get_current_core_id(),
                vector, &msgerr);
    if (err_is_fail(err)) {
        return err;
    } else if (err_is_fail(msgerr)) {
        return msgerr;
    }
    return SYS_ERR_OK;
}

errval_t pci_register_driver_movable_irq(pci_driver_init_fn init_func, uint32_t class,
                                         uint32_t subclass, uint32_t prog_if,
                                         uint32_t vendor, uint32_t device,
                                         uint32_t bus, uint32_t dev, uint32_t fun,
                                         interrupt_handler_fn handler,
                                         void *handler_arg,
                                         interrupt_handler_fn reloc_handler,
                                         void *reloc_handler_arg)
{
    pci_caps_per_bar_t *caps_per_bar = NULL;
    uint8_t nbars;
    errval_t err, msgerr;

    err = pci_client->vtbl.
        init_pci_device(pci_client, class, subclass, prog_if, vendor,
                        device, bus, dev, fun, &msgerr,
                        &nbars, &caps_per_bar);
    if (err_is_fail(err)) {
        return err;
    } else if (err_is_fail(msgerr)) {
        free(caps_per_bar);
        return msgerr;
    }

    struct capref irq_src_cap;

    // Get vector 0 of the device.
    // For backward compatibility with function interface.
    err = pci_client->vtbl.get_irq_cap(pci_client, 0, &msgerr, &irq_src_cap);
    if (err_is_fail(err) || err_is_fail(msgerr)) {
        if (err_is_ok(err)) {
            err = msgerr;
        }
        DEBUG_ERR(err, "requesting cap for IRQ 0 of device");
        goto out;
    }

    uint32_t gsi = INVALID_VECTOR;
    err = invoke_irqsrc_get_vector(irq_src_cap, &gsi);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Could not lookup GSI vector");
        return err;
    }
    PCI_CLIENT_DEBUG("Got irqsrc cap, gsi: %"PRIu32"\n", gsi);

    // Get irq_dest_cap from monitor
    struct capref irq_dest_cap;
    err = alloc_dest_irq_cap(&irq_dest_cap);
    if(err_is_fail(err)){
        DEBUG_ERR(err, "Could not allocate dest irq cap");
        goto out;
    }
    uint32_t irq_dest_vec = INVALID_VECTOR;
    err = invoke_irqdest_get_vector(irq_dest_cap, &irq_dest_vec);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Could not lookup irq vector");
        return err;
    }
    PCI_CLIENT_DEBUG("Got dest cap, vector: %"PRIu32"\n", irq_dest_vec);


    // Setup routing
    // TODO: Instead of getting the vectors of each cap and set up routing,
    // pass both to a routing service and let the service handle the setup.
    struct acpi_rpc_client* cl = get_acpi_rpc_client();
    errval_t ret_error;
    err = cl->vtbl.enable_and_route_interrupt(cl, gsi, disp_get_core_id(), irq_dest_vec, &ret_error);
    assert(err_is_ok(err));
    if (err_is_fail(ret_error)) {
        DEBUG_ERR(ret_error, "failed to route interrupt %d -> %d\n", gsi, irq_dest_vec);
        return err_push(ret_error, PCI_ERR_ROUTING_IRQ);
    }

    // Connect endpoint to handler
    if(handler != NULL){
        err = inthandler_setup_movable_cap(irq_dest_cap, handler, handler_arg,
                reloc_handler, reloc_handler_arg);
        if (err_is_fail(err)) {
            return err;
        }
    }


    assert(nbars > 0); // otherwise we should have received an error!

    // FIXME: leak
    struct device_mem *bars = calloc(nbars, sizeof(struct device_mem));
    assert(bars != NULL);

    // request caps for all bars of device
    for (int nb = 0; nb < nbars; nb++) {
        struct device_mem *bar = &bars[nb];

        int ncaps = (*caps_per_bar)[nb];
        if (ncaps != 0) {
            bar->nr_caps = ncaps;
            bar->frame_cap = malloc(ncaps * sizeof(struct capref)); // FIXME: leak
            assert(bar->frame_cap != NULL);
        }

        for (int nc = 0; nc < ncaps; nc++) {
            struct capref cap;
            uint8_t type;

            err = pci_client->vtbl.get_bar_cap(pci_client, nb, nc, &msgerr, &cap,
                                           &type, &bar->bar_nr);
            if (err_is_fail(err) || err_is_fail(msgerr)) {
                if (err_is_ok(err)) {
                    err = msgerr;
                }
                DEBUG_ERR(err, "requesting cap %d for BAR %d of device", nc, nb);
                goto out;
            }

            if (type == 0) { // Frame cap BAR
                bar->frame_cap[nc] = cap;
                if (nc == 0) {
                    struct frame_identity id = { .base = 0, .bytes = 0 };
                    invoke_frame_identify(cap, &id);
                    bar->paddr = id.base;
                    bar->bits = log2ceil(id.bytes);
                    bar->bytes = id.bytes * ncaps;
                }
            } else { // IO BAR
                bar->io_cap = cap;
                err = cap_copy(cap_io, cap);
                if(err_is_fail(err) && err_no(err) != SYS_ERR_SLOT_IN_USE) {
                    DEBUG_ERR(err, "cap_copy for IO cap");
                    goto out;
                }
            }
        }
    }

    // initialize the device. We have all the caps now
    init_func(bars, nbars);

    err = SYS_ERR_OK;

 out:
    free(caps_per_bar);
    return err;
}

errval_t pci_register_driver_irq(pci_driver_init_fn init_func, uint32_t class,
                                 uint32_t subclass, uint32_t prog_if,
                                 uint32_t vendor, uint32_t device,
                                 uint32_t bus, uint32_t dev, uint32_t fun,
                                 interrupt_handler_fn handler,
                                 void *handler_arg)
{
     return pci_register_driver_movable_irq(init_func, class, subclass,
             prog_if, vendor, device, bus, dev, fun, handler, handler_arg,
             NULL, NULL);
}


errval_t pci_register_driver_noirq(pci_driver_init_fn init_func, uint32_t class,
                                   uint32_t subclass, uint32_t prog_if,
                                   uint32_t vendor, uint32_t device,
                                   uint32_t bus, uint32_t dev, uint32_t fun)
{
    return pci_register_driver_irq(init_func, class, subclass, prog_if, vendor,
                                   device, bus, dev, fun, NULL, NULL);
}

errval_t pci_register_legacy_driver_irq(legacy_driver_init_fn init_func,
                                        uint16_t iomin, uint16_t iomax, int irq,
                                        interrupt_handler_fn handler,
                                        void *handler_arg)
{
    errval_t err, msgerr;
    struct capref iocap;

    uint32_t vector = INVALID_VECTOR;
    err = inthandler_setup(handler, handler_arg, &vector);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "inthandler_setup()\n");
        return err;
    }

    err = pci_client->vtbl.init_legacy_device(pci_client, iomin, iomax, irq,
                                              disp_get_core_id(), vector,
                                              &msgerr, &iocap);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "pci_client->init_legacy_device()\n");
        return err;
    } else if (err_is_fail(msgerr)) {
        DEBUG_ERR(msgerr, "pci_client->init_legacy_device()\n");
        return msgerr;
    }

    /* copy IO cap to default location */
    err = cap_copy(cap_io, iocap);
    if (err_is_fail(err) && err_no(err) != SYS_ERR_SLOT_IN_USE) {
        DEBUG_ERR(err, "failed to copy legacy io cap to default slot\n");
    }

    err = cap_destroy(iocap);
    assert(err_is_ok(err));

    /* run init func */
    init_func();

    return msgerr;
}

errval_t pci_setup_inthandler(interrupt_handler_fn handler, void *handler_arg,
                                      uint8_t *ret_vector)
{
    errval_t err;
    uint32_t vector = INVALID_VECTOR;
    *ret_vector = 0;
    err = inthandler_setup(handler, handler_arg, &vector);
    if (err_is_ok(err)) {
        *ret_vector = vector + 32; // FIXME: HACK
    }
    return err;
}

errval_t pci_read_conf_header(uint32_t dword, uint32_t *val)
{
    errval_t err, msgerr;
    err = pci_client->vtbl.read_conf_header(pci_client, dword, &msgerr, val);
    return err_is_fail(err) ? err : msgerr;
}

errval_t pci_write_conf_header(uint32_t dword, uint32_t val)
{
    errval_t err, msgerr;
    err = pci_client->vtbl.write_conf_header(pci_client, dword, val, &msgerr);
    return err_is_fail(err) ? err : msgerr;
}

errval_t pci_msix_enable_addr(struct pci_address *addr, uint16_t *count)
{
    errval_t err, msgerr;
    if (addr == NULL) {
        err = pci_client->vtbl.msix_enable(pci_client, &msgerr, count);
    } else {
        err = pci_client->vtbl.msix_enable_addr(pci_client, addr->bus, addr->device,
                                                addr->function, &msgerr, count);
    }
    return err_is_fail(err) ? err : msgerr;
}

errval_t pci_msix_enable(uint16_t *count)
{
    return pci_msix_enable_addr(NULL, count);
}

errval_t pci_msix_vector_init_addr(struct pci_address *addr, uint16_t idx,
                                   uint8_t destination, uint8_t vector)
{
    errval_t err, msgerr;
    if (addr == NULL) {
        err = pci_client->vtbl.msix_vector_init(pci_client, idx, destination,
                                                    vector, &msgerr);
    } else {
        err = pci_client->vtbl.msix_vector_init_addr(pci_client, addr->bus,
                                                     addr->device, addr->function,
                                                     idx, destination,
                                                     vector, &msgerr);
    }

    return err_is_fail(err) ? err : msgerr;
}

errval_t pci_msix_vector_init(uint16_t idx, uint8_t destination,
                              uint8_t vector)
{
    return pci_msix_vector_init_addr(NULL, idx, destination, vector);
}

static void bind_cont(void *st, errval_t err, struct pci_binding *b)
{
    errval_t *reterr = st;
    if (err_is_ok(err)) {
        struct pci_rpc_client *r = malloc(sizeof(*r));
        assert(r != NULL);
        err = pci_rpc_client_init(r, b);
        if (err_is_ok(err)) {
            pci_client = r;
        } else {
            free(r);
        }
    }
    *reterr = err;
}

errval_t pci_client_connect(void)
{
    iref_t iref;
    errval_t err, err2 = SYS_ERR_OK;

    err = connect_to_acpi();
    if(err_is_fail(err)){
        return err;
    }

    /* Connect to the pci server */
    err = nameservice_blocking_lookup("pci", &iref);
    if (err_is_fail(err)) {
        return err;
    }

    assert(iref != 0);

    /* Setup flounder connection with pci server */
    err = pci_bind(iref, bind_cont, &err2, get_default_waitset(),
                   IDC_BIND_FLAG_RPC_CAP_TRANSFER);
    if (err_is_fail(err)) {
        return err;
    }

    /* XXX: Wait for connection establishment */
    while (pci_client == NULL && err2 == SYS_ERR_OK) {
        messages_wait_and_handle_next();
    }

    return err2;
}
