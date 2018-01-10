// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zircon/compiler.h>

#include <ddk/device.h>
#include <ddk/protocol/block.h>
#include <ddk/protocol/sdmmc.h>
#include <hw/sdmmc.h>

#include <threads.h>

__BEGIN_CDECLS;

typedef enum sdmmc_type {
    SDMMC_TYPE_SD,
    SDMMC_TYPE_MMC,
} sdmmc_type_t;

typedef struct sdmmc_device {
    zx_device_t* zxdev;

    sdmmc_protocol_t host;
    sdmmc_host_info_t host_info;

    sdmmc_type_t type;

    sdmmc_bus_width_t bus_width;
    sdmmc_voltage_t signal_voltage;
    sdmmc_timing_t timing;

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

    block_info_t block_info;
} sdmmc_device_t;

// Issue a request to the host controller and wait for completion.
zx_status_t sdmmc_do_request(sdmmc_device_t* dev, sdmmc_request_t* req);

zx_status_t sdmmc_probe_sd(sdmmc_device_t* dev);
zx_status_t sdmmc_probe_mmc(sdmmc_device_t* dev);

__END_CDECLS;
