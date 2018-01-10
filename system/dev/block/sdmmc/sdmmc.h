// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zircon/compiler.h>

#include <ddk/device.h>
#include <hw/sdmmc.h>

#include <threads.h>

__BEGIN_CDECLS;

typedef enum sdmmc_type {
    SDMMC_TYPE_SD,
    SDMMC_TYPE_MMC,
} sdmmc_type_t;

typedef struct sdmmc {
    zx_device_t* zxdev;
    zx_device_t* host_zxdev;

    sdmmc_protocol_t host;

    sdmmc_type_t type;

    sdmmc_bus_width_t bus_width;      // Data bus width
    sdmmc_voltage_t signal_voltage; // Bus signal voltage
    sdmmc_timing_t timing;         // Bus timing

    unsigned clock_rate;    // Bus clock rate
    uint64_t capacity;      // Card capacity

    uint16_t rca;           // Relative address

    uint32_t raw_cid[4];
    uint32_t raw_csd[4];
    uint8_t raw_ext_csd[512];

    mtx_t lock;

    list_node_t txn_list;   // list of iotxn

    thrd_t worker_thread;
    zx_handle_t worker_event;
    bool worker_thread_running;

    uint32_t max_transfer_size;
} sdmmc_t;

// Issue a command to the host controller
zx_status_t sdmmc_do_command(zx_device_t* dev, const uint32_t cmd, const uint32_t arg, iotxn_t* txn);

zx_status_t sdmmc_probe_sd(sdmmc_t* sdmmc, iotxn_t* setup_txn);
zx_status_t sdmmc_probe_mmc(sdmmc_t* sdmmc, iotxn_t* setup_txn);

__END_CDECLS;
