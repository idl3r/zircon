// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/iotxn.h>
#include <zircon/compiler.h>

__BEGIN_CDECLS;

static_assert(sizeof(sdmmc_protocol_data_t) <= sizeof(iotxn_proto_data_t), "sdmmc protocol data too large\n");

typedef enum sdmmc_voltage {
    SDMMC_VOLTAGE_330,
    SDMMC_VOLTAGE_180,
    SDMMC_VOLTAGE_MAX,
} sdmmc_voltage_t;

typedef enum sdmmc_bus_width {
    SDMMC_BUS_WIDTH_1,
    SDMMC_BUS_WIDTH_4,
    SDMMC_BUS_WIDTH_8,
    SDMMC_BUS_WIDTH_MAX,
} sdmmc_bus_width_t;

typedef enum sdmmc_timing {
    SDMMC_TIMING_LEGACY,
    SDMMC_TIMING_HS,
    SDMMC_TIMING_HSDDR,
    SDMMC_TIMING_HS200,
    SDMMC_TIMING_HS400,
    SDMMC_TIMING_MAX,
} sdmmc_timing_t;

typedef sdmmc_request {
    uint32_t cmd;
    uint32_t arg;

    // data command parameters
    uint16_t blockcount;
    uint16_t blocksize;

    // current block to transfer for PIO
    uint16_t blockid;

    // response data from command
    uint32_t response[4];

    // completion callback. it is illegal to call request() from this callback
    void (*completion)(sdmmc_request_t* req, void* cookie);
    void* cookie;

    iotxn_t* txn;   // associated iotxn for commands with data
};

typedef struct sdmmc_protocol_ops {
    // set signal voltage
    zx_status_t (*set_signal_voltage)(void* ctx, sdmmc_voltage_t voltage);
    // set bus width
    zx_status_t (*set_bus_width)(void* ctx, sdmmc_bus_width_t bus_width);
    // set bus frequency
    zx_status_t (*set_bus_freq)(void* ctx, uint32_t bus_freq);
    // set mmc timing
    zx_status_t (*set_timing)(void* ctx, sdmmc_timing_t timing);
    // issue a hw reset
    void (*hw_reset)(void* ctx);
    // perform tuning
    zx_status_t (*perform_tuning)(void* ctx);
    // issue a request
    zx_status_t (*request)(void* ctx, sdmmc_request_t* req);
} sdmmc_protocol_ops_t;

typedef struct sdmmc_protocol {
    sdmmc_protocol_ops_t* ops;
    void* ctx;
} sdmmc_protocol_t;

static inline zx_status_t sdmmc_set_signal_voltage(sdmmc_protocol_t* sdmmc,
                                                   sdmmc_voltage_t voltage) {
    return sdmmc->ops->set_signal_voltage(sdmmc->ctx, voltage);
}

static inline zx_status_t sdmmc_set_bus_width(sdmmc_protocol_t* sdmmc,
                                              sdmmc_bus_width_t bus_width) {
    return sdmmc->ops->set_bus_width(sdmmc->ctx, bus_width);
}

static inline zx_status_t sdmmc_set_bus_freq(sdmmc_protocol_t* sdmmc, uint32_t bus_freq) {
    return sdmmc->ops->set_bus_freq(sdmmc->ctx, bus_freq);
}

static inline zx_status_t sdmmc_set_timing(sdmmc_protocol_t* sdmmc, sdmmc_timing_t timing) {
    return sdmmc->ops->set_timing(sdmmc->ctx, timing);
}

static inline void sdmmc_hw_reset(sdmmc_protocol_t* sdmmc) {
    sdmmc->ops->hw_reset(sdmmc->ctx);
}

static inline zx_status_t sdmmc_perform_tuning(sdmmc_protocol_t* sdmmc) {
    return sdmmc->ops->perform_tuning(sdmmc->ctx);
}

__END_CDECLS;
