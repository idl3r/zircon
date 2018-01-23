// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <threads.h>

#include <bits/limits.h>
#include <ddk/debug.h>
#include <hw/reg.h>

#include <zircon/assert.h>
#include <zircon/types.h>

#include <soc/aml-common/aml-gpio.h>
#include <soc/aml-a113/a113-hw.h>

#define PAGE_START(a) ((~(PAGE_SIZE - 1)) & (a))

static aml_gpio_block_t gpio_blocks[] = {
    // GPIO X Block
    {
        .start_pin = (A113_GPIOX_START + 0),
        .pin_block = A113_GPIOX_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_4,
        .ctrl_offset = GPIO_REG2_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOX_START + 8),
        .pin_block = A113_GPIOX_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_5,
        .ctrl_offset = GPIO_REG2_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOX_START + 16),
        .pin_block = A113_GPIOX_START,
        .pin_count = 7,
        .mux_offset = PERIPHS_PIN_MUX_6,
        .ctrl_offset = GPIO_REG2_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },

    // GPIO A Block
    {
        .start_pin = (A113_GPIOA_START + 0),
        .pin_block = A113_GPIOA_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_B,
        .ctrl_offset = GPIO_REG0_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOA_START + 8),
        .pin_block = A113_GPIOA_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_C,
        .ctrl_offset = GPIO_REG0_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOA_START + 16),
        .pin_block = A113_GPIOA_START,
        .pin_count = 5,
        .mux_offset = PERIPHS_PIN_MUX_D,
        .ctrl_offset = GPIO_REG0_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },

    // GPIO Boot Block
    {
        .start_pin = (A113_GPIOB_START + 0),
        .pin_block = A113_GPIOB_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_0,
        .ctrl_offset = GPIO_REG4_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOB_START + 8),
        .pin_block = A113_GPIOB_START,
        .pin_count = 7,
        .mux_offset = PERIPHS_PIN_MUX_1,
        .ctrl_offset = GPIO_REG4_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },

    // GPIO Y Block
    {
        .start_pin = (A113_GPIOY_START + 0),
        .pin_block = A113_GPIOY_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_8,
        .ctrl_offset = GPIO_REG1_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOY_START + 8),
        .pin_block = A113_GPIOY_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_9,
        .ctrl_offset = GPIO_REG1_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },

    // GPIO Z Block
    {
        .start_pin = (A113_GPIOZ_START + 0),
        .pin_block = A113_GPIOZ_START,
        .pin_count = 8,
        .mux_offset = PERIPHS_PIN_MUX_2,
        .ctrl_offset = GPIO_REG3_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOZ_START + 8),
        .pin_block = A113_GPIOZ_START,
        .pin_count = 3,
        .mux_offset = PERIPHS_PIN_MUX_3,
        .ctrl_offset = GPIO_REG3_EN_N,
//        .ctrl_block_base_phys = GPIO_BASE_PAGE,
        .lock = MTX_INIT,
    },

    // GPIO AO Block
    // NOTE: The GPIO AO block has a seperate control block than the other
    //       GPIO blocks.
    {
        .start_pin = (A113_GPIOAO_START + 0),
        .pin_block = A113_GPIOAO_START,
        .pin_count = 8,
        .mux_offset = AO_RTI_PIN_MUX_REG0,
        .ctrl_offset = AO_GPIO_O_EN_N,
//        .ctrl_block_base_phys = GPIOAO_BASE_PAGE,
        .lock = MTX_INIT,
    },
    {
        .start_pin = (A113_GPIOAO_START + 8),
        .pin_block = A113_GPIOAO_START,
        .pin_count = 6,
        .mux_offset = AO_RTI_PIN_MUX_REG1,
        .ctrl_offset = AO_GPIO_O_EN_N,
//        .ctrl_block_base_phys = GPIOAO_BASE_PAGE,
        .lock = MTX_INIT,
    },
};

static zx_status_t aml_pin_to_block(aml_gpio_t* gpio, const uint32_t pinid, aml_gpio_block_t** result) {
    ZX_DEBUG_ASSERT(result);

    for (size_t i = 0; i < gpio->gpio_block_count; i++) {
        aml_gpio_block_t* gpio_block = &gpio->gpio_blocks[i];
        const uint32_t end_pin = gpio_block->start_pin + gpio_block->pin_count;
        if (pinid >= gpio_block->start_pin && pinid < end_pin) {
            *result = gpio_block;
            return ZX_OK;
        }
    }

    return ZX_ERR_NOT_FOUND;
}

static zx_status_t aml_gpio_set_direction(aml_gpio_block_t* block,
                                           const uint32_t index,
                                           const gpio_config_flags_t flags) {

    const uint32_t pinid = index - block->pin_block;

    mtx_lock(&block->lock);

    volatile uint32_t* reg = (volatile uint32_t*)(block->ctrl_block_base_virt);
    reg += block->ctrl_offset;
    uint32_t regval = readl(reg);
    const uint32_t pinmask = 1 << pinid;

    if (flags & GPIO_DIR_OUT) {
        regval &= ~pinmask;
    } else {
        regval |= pinmask;
    }

    writel(regval, reg);

    mtx_unlock(&block->lock);

    return ZX_OK;
}

static zx_status_t aml_gpio_config(void* ctx, uint32_t index, gpio_config_flags_t flags) {
    aml_gpio_t* gpio = ctx;
    zx_status_t status;

    aml_gpio_block_t* block;
    if ((status = aml_pin_to_block(gpio, index, &block)) != ZX_OK) {
        zxlogf(ERROR, "aml_gpio_config: pin not found %u\n", index);
        return status;
    }

    if ((status = aml_gpio_set_direction(block, index, flags)) != ZX_OK) {
        zxlogf(ERROR, "aml_gpio_config: failed to set pin(%u) direction, rc = %d\n",
               index, status);
        return status;
    }

    return ZX_OK;
}

// Configure a pin for an alternate function specified by fn
static zx_status_t aml_gpio_set_alt_function(void* ctx, const uint32_t pin, const uint32_t fn) {
    aml_gpio_t* gpio = ctx;

    if (fn > A113_PINMUX_ALT_FN_MAX) {
        zxlogf(ERROR, "aml_config_pinmux: pin mux alt config out of range"
                " %u\n", fn);
        return ZX_ERR_OUT_OF_RANGE;
    }

    zx_status_t status;

    aml_gpio_block_t* block;
    if (((status = aml_pin_to_block(gpio, pin, &block)) != ZX_OK) != ZX_OK) {
        zxlogf(ERROR, "aml_config_pinmux: pin not found %u\n", pin);
        return status;
    }

    // Points to the control register.
    volatile uint32_t* reg = (volatile uint32_t*)(block->ctrl_block_base_virt);
    reg += block->mux_offset;

    // Sanity Check: pin_to_block must return a block that contains `pin`
    //               therefore `pin` must be greater than or equal to the first
    //               pin of the block.
    ZX_DEBUG_ASSERT(pin >= block->start_pin);

    // Each Pin Mux is controlled by a 4 bit wide field in `reg`
    // Compute the offset for this pin.
    const uint32_t pin_shift = (pin - block->start_pin) * 4;
    const uint32_t mux_mask = ~(0x0F << pin_shift);
    const uint32_t fn_val = fn << pin_shift;

    mtx_lock(&block->lock);

    uint32_t regval = readl(reg);
    regval &= mux_mask;     // Remove the previous value for the mux
    regval |= fn_val;       // Assign the new value to the mux
    writel(regval, reg);

    mtx_unlock(&block->lock);

    return ZX_OK;
}

static zx_status_t aml_gpio_read(void* ctx, uint32_t index, uint8_t* out_value) {
    aml_gpio_t* gpio = ctx;
    zx_status_t status;

    aml_gpio_block_t* block;
    if ((status = aml_pin_to_block(gpio, index, &block)) != ZX_OK) {
        zxlogf(ERROR, "aml_config_pinmux: pin not found %u\n", index);
        return status;
    }

    const uint32_t pinindex = index - block->pin_block;
    const uint32_t readmask = 1 << pinindex;

    volatile uint32_t* reg = (volatile uint32_t*)(block->ctrl_block_base_virt);
    reg += block->ctrl_offset;

    if (block->pin_block == A113_GPIOAO_START) {
        reg += GPIOAO_INPUT_OFFSET;
    } else {
        reg += GPIO_INPUT_OFFSET;
    }

    mtx_lock(&block->lock);

    const uint32_t regval = readl(reg);

    mtx_unlock(&block->lock);

    if (regval & readmask) {
        *out_value = 1;
    } else {
        *out_value = 0;
    }

    return ZX_OK;
}

static zx_status_t aml_gpio_write(void* ctx, uint32_t index, uint8_t value) {
    aml_gpio_t* gpio = ctx;
    zx_status_t status;

    aml_gpio_block_t* block;
    if ((status = aml_pin_to_block(gpio, index, &block)) != ZX_OK) {
        zxlogf(ERROR, "aml_gpio_write: pin not found %u\n", index);
        return status;
    }

    uint32_t pinindex = index - block->pin_block;

    volatile uint32_t* reg = (volatile uint32_t*)(block->ctrl_block_base_virt);
    reg += block->ctrl_offset;

    if (block->pin_block == A113_GPIOAO_START) {
        // Output pins are shifted by 16 bits for GPIOAO block
        pinindex += 16;
    } else {
        // Output register is offset for regular GPIOs
        reg += GPIO_OUTPUT_OFFSET;
    }

    mtx_lock(&block->lock);

    uint32_t regval = readl(reg);

    if (value) {
        regval |= 1 << pinindex;
    } else {
        regval &= ~(1 << pinindex);
    }

    writel(regval, reg);

    mtx_unlock(&block->lock);

    return ZX_OK;
}


void aml_gpio_release(aml_gpio_t* gpio) {
    io_buffer_release(&gpio->periphs_ao_reg);
    io_buffer_release(&gpio->periphs_reg);
}

static gpio_protocol_ops_t gpio_ops = {
    .config = aml_gpio_config,
    .set_alt_function = aml_gpio_set_alt_function,
    .read = aml_gpio_read,
    .write = aml_gpio_write,
};

zx_status_t aml_gpio_init(aml_gpio_t* gpio, zx_paddr_t gpio_base, zx_paddr_t a0_base,
                          aml_gpio_block_t* gpio_blocks, size_t gpio_block_count) {
    ZX_DEBUG_ASSERT(gpio);

    zx_paddr_t gpio_base_page = PAGE_START(gpio_base);
    zx_paddr_t a0_base_page = PAGE_START(a0_base);

    zx_handle_t resource = get_root_resource();
    zx_status_t status = ZX_ERR_INTERNAL;

    gpio->gpio_blocks = gpio_blocks;
    gpio->gpio_block_count = gpio_block_count;

    // Initialize the Standard GPIO Block
    status = io_buffer_init_physical(&gpio->periphs_reg, gpio_base_page,
                                     PAGE_SIZE, resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "aml_gpio_init: Failed to map io buffer at 0x%p"
                ", status = %d\n", (void *)gpio_base_page, status);
        return status;
    }

    // Initialize the "Always On" GPIO AO Block.
    status = io_buffer_init_physical(&gpio->periphs_ao_reg, a0_base_page,
                                     PAGE_SIZE, resource,
                                     ZX_CACHE_POLICY_UNCACHED_DEVICE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "aml_gpio_init: Failed to map io buffer at 0x%p"
                ", status = %d\n", (void *)a0_base_page, status);

        // Failed to initialize completely. Release the IO Buffer we allocated
        // above.
        io_buffer_release(&gpio->periphs_reg);
        return status;
    }

    // Initialize each of the GPIO Pin blocks.
    for (size_t i = 0; i < gpio_block_count; i++) {
        aml_gpio_block_t* gpio_block = &gpio_blocks[i];

        // Set the appropriate virtual address of the GPIO control block based
        // on the physical address of the block.
        if (gpio_block->ctrl_block_base_phys == a0_base_page) {
            gpio_block->ctrl_block_base_virt =
                ((zx_vaddr_t)io_buffer_virt(&gpio->periphs_ao_reg)) +
                (a0_base - a0_base_page);
        } else if (gpio_block->ctrl_block_base_phys == gpio_base_page) {
            gpio_block->ctrl_block_base_virt =
                ((zx_vaddr_t)io_buffer_virt(&gpio->periphs_reg)) +
                (gpio_base - gpio_base_page);
        } else {
            zxlogf(ERROR, "aml_gpio_init: unexpected gpio control block"
                    " base address at 0x%016lx\n",
                    gpio_block->ctrl_block_base_phys);
            status = ZX_ERR_NOT_SUPPORTED;
            goto cleanup_and_fail;
        }
    }

    // Copy the protocol into the aml gpio struct.
    gpio->proto.ops = &gpio_ops;
    gpio->proto.ctx = gpio;

    return ZX_OK;

cleanup_and_fail:
    io_buffer_release(&gpio->periphs_ao_reg);
    io_buffer_release(&gpio->periphs_reg);
    return status;
}