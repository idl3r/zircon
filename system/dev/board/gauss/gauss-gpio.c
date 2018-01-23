// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/protocol/platform-defs.h>
#include <soc/aml-a113/a113-hw.h>

#include <limits.h>

#include "gauss.h"

#define PAGE_MASK (PAGE_SIZE - 1)

// Physical Base Address for Pinmux/GPIO Control
#define GPIO_BASE_PHYS 0xff634400
#define GPIO_BASE_PAGE ((~PAGE_MASK) & GPIO_BASE_PHYS)

// The GPIO "Always On" Domain has its own control block mapped here.
#define GPIOAO_BASE_PHYS 0xff800000
#define GPIOAO_BASE_PAGE ((~PAGE_MASK) & GPIOAO_BASE_PHYS)

static const pbus_mmio_t gpio_mmios[] = {
    {
        .base = ROUNDDOWN(GPIO_BASE_PAGE, PAGE_SIZE),
        .offset = GPIO_BASE_PAGE & (PAGE_SIZE - 1),
        .length = DWC3_MMIO_LENGTH,
    },
    {
        .base = GPIOAO_BASE_PAGE,
        .length = DWC3_MMIO_LENGTH,
    },
};

static const pbus_irq_t gpio_irqs[] = {
    { .irq = 64, },
    { .irq = 65, },
    { .irq = 66, },
    { .irq = 67, },
    { .irq = 68, },
    { .irq = 69, },
    { .irq = 70, },
    { .irq = 71, },
};

static const pbus_dev_t gpio_dev = {
    .name = "gpio",
    .vid = PDEV_VID_AMLOGIC,
    .pid = PDEV_PID_GENERIC,
    .did = PDEV_DID_AMLOGIC_GPIO,
    .mmios = gpio_mmios,
    .mmio_count = countof(gpio_mmios),
    .irqs = gpio_irqs,
    .irq_count = countof(gpio_irqs),
};

zx_status_t gauss_gpio_init(gauss_bus_t* bus) {
    zx_status_t status = pbus_add_proto_helper(&bus->pbus, ZX_PROTOCOL_GPIO, &gpio_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "pbus_add_proto_helper(ZX_PROTOCOL_GPIO)  failed: %d\n", status);
        return status;
    }

    return device_get_protocol(bus->parent, ZX_PROTOCOL_GPIO, &bus->gpio);
}
