// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Standard Includes
#include <endian.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <threads.h>

// DDK Includes
#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/iotxn.h>
#include <ddk/debug.h>
#include <ddk/protocol/sdmmc.h>

// Zircon Includes
#include <sync/completion.h>
#include <pretty/hexdump.h>
#include <zircon/threads.h>
#include <zircon/device/block.h>

#include "sdmmc.h"

#define SDMMC_IOTXN_RECEIVED    ZX_EVENT_SIGNALED
#define SDMMC_SHUTDOWN          ZX_USER_SIGNAL_0
#define SDMMC_SHUTDOWN_DONE     ZX_USER_SIGNAL_1

#define SDMMC_LOCK(dev)   mtx_lock(&(dev)->lock);
#define SDMMC_UNLOCK(dev) mtx_unlock(&(dev)->lock);

static bool sdmmc_use_dma(sdmmc_device_t* dev) {
    return (dev->host_info.caps & (SDMMC_HOST_CAP_ADMA2 | SDMMC_HOST_CAP_64BIT));
}

static void sdmmc_req_cplt(sdmmc_request_t* req, void* cookie) {
    completion_signal((completion_t*)cookie);
}

zx_status_t sdmmc_do_request(sdmmc_device_t* dev, sdmmc_request_t* req) {
    completion_t completion = COMPLETION_INIT;
    req->complete_cb = sdmmc_req_cplt;
    req->cookie = &completion;

    sdmmc_request(&dev->host, req);

    completion_wait(&completion, ZX_TIME_INFINITE);

    return req->status;
}

static zx_status_t sdmmc_go_idle(sdmmc_device_t* dev, sdmmc_request_t* req) {
    req->cmd = SDMMC_GO_IDLE_STATE;
    req->arg = 0;
    return sdmmc_do_request(dev, req);
}

static zx_status_t sdmmc_send_status(sdmmc_device_t* dev, sdmmc_request_t* req, uint16_t rca) {
    req->cmd = SDMMC_SEND_STATUS;
    req->arg = rca << 16;
    return sdmmc_do_request(dev, req);
}

static zx_status_t sdmmc_stop_transmission(sdmmc_device_t* dev, sdmmc_request_t* req) {
    req->cmd = SDMMC_STOP_TRANSMISSION;
    req->arg = 0;
    return sdmmc_do_request(dev, req);
}

static zx_off_t sdmmc_get_size(void* ctx) {
    sdmmc_device_t* dev = ctx;
    return dev->block_info.block_count * dev->block_info.block_size;
}

static void sdmmc_sync_iotxn_cplt(iotxn_t* txn, void* cookie) {
    completion_signal((completion_t*)cookie);
}

static zx_status_t sdmmc_device_sync(sdmmc_device_t* dev) {
    iotxn_t* txn;
    zx_status_t status = iotxn_alloc(&txn, 0, 0);
    if (status != ZX_OK) {
        return status;
    }
    completion_t completion = COMPLETION_INIT;
    txn->opcode = IOTXN_OP_READ;
    txn->flags = IOTXN_SYNC_BEFORE;
    txn->offset = 0;
    txn->length = 0;
    txn->complete_cb = sdmmc_sync_iotxn_cplt;
    txn->cookie = &completion;
    iotxn_queue(dev->zxdev, txn);
    completion_wait(&completion, ZX_TIME_INFINITE);
    status = txn->status;
    iotxn_release(txn);
    return status;
}

static zx_status_t sdmmc_ioctl(void* ctx, uint32_t op, const void* cmd,
                               size_t cmdlen, void* reply, size_t max, size_t* out_actual) {
    switch (op) {
    case IOCTL_BLOCK_GET_INFO: {
        sdmmc_device_t* dev = ctx;
        block_info_t* info = reply;
        if (max < sizeof(*info)) {
            return ZX_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(info, &dev->block_info, sizeof(*info));
        *out_actual = sizeof(*info);
        return ZX_OK;
    }
    case IOCTL_BLOCK_RR_PART: {
        sdmmc_device_t* dev = ctx;
        return device_rebind(dev->zxdev);
    }
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
    return 0;
}

static void sdmmc_unbind(void* ctx) {
    sdmmc_device_t* dev = ctx;
    device_remove(dev->zxdev);
}

static void sdmmc_release(void* ctx) {
    sdmmc_device_t* dev = ctx;
    if (dev->worker_thread_running) {
        // signal the worker thread and wait for it to terminate
        zx_object_signal(dev->worker_event, 0, SDMMC_SHUTDOWN);
        zx_object_wait_one(dev->worker_event, SDMMC_SHUTDOWN_DONE, ZX_TIME_INFINITE, NULL);

        SDMMC_LOCK(dev);

        // error out all pending requests
        iotxn_t* txn = NULL;
        list_for_every_entry(&dev->txn_list, txn, iotxn_t, node) {
            SDMMC_UNLOCK(dev);

            iotxn_complete(txn, ZX_ERR_BAD_STATE, 0);

            SDMMC_LOCK(dev);
        }

        SDMMC_UNLOCK(dev);

        thrd_join(dev->worker_thread, NULL);
    }

    if (dev->worker_event != ZX_HANDLE_INVALID) {
        zx_handle_close(dev->worker_event);
    }

    free(dev);
}

static void sdmmc_iotxn_queue(void* ctx, iotxn_t* txn) {
    zxlogf(SPEW, "sdmmc: iotxn_queue txn %p offset 0x%" PRIx64
                   " length 0x%" PRIx64 "\n", txn, txn->offset, txn->length);

    if (txn->offset % SDHC_BLOCK_SIZE) {
        zxlogf(ERROR, "sdmmc: iotxn offset not aligned to block boundary, "
                "offset =%" PRIu64 ", block size = %d\n",
                txn->offset, SDHC_BLOCK_SIZE);
        iotxn_complete(txn, ZX_ERR_INVALID_ARGS, 0);
        return;
    }

    if (txn->length % SDHC_BLOCK_SIZE) {
        zxlogf(ERROR, "sdmmc: iotxn length not aligned to block boundary, "
                "offset =%" PRIu64 ", block size = %d\n",
                txn->length, SDHC_BLOCK_SIZE);
        iotxn_complete(txn, ZX_ERR_INVALID_ARGS, 0);
        return;
    }

    if ((txn->offset >= sdmmc_get_size(ctx)) ||
        (sdmmc_get_size(ctx) - txn->offset < txn->length)) {
        zxlogf(ERROR, "sdmmc: iotxn beyond boundary off device "
                "device size =%" PRIu64 "\n", sdmmc_get_size(ctx));
        iotxn_complete(txn, ZX_ERR_OUT_OF_RANGE, 0);
        return;
    }

    // immediately complete empty requests, unless it's for sync
    if ((txn->length == 0) && !(txn->flags & IOTXN_SYNC_BEFORE)) {
        iotxn_complete(txn, ZX_OK, 0);
        return;
    }

    sdmmc_device_t* dev = ctx;

    SDMMC_LOCK(dev);

    list_add_tail(&dev->txn_list, &txn->node);

    SDMMC_UNLOCK(dev);

    // Wake up the worker thread
    zx_object_signal(dev->worker_event, 0, SDMMC_IOTXN_RECEIVED);
}

// Device protocol.
static zx_protocol_device_t sdmmc_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .ioctl = sdmmc_ioctl,
    .unbind = sdmmc_unbind,
    .release = sdmmc_release,
};

static void sdmmc_query(void* ctx, block_info_t* info_out, size_t* block_op_size_out) {
    sdmmc_device_t* dev = ctx;
    memcpy(info_out, &dev->block_info, sizeof(info_out));
    *block_op_size_out = sizeof(sdmmc_request_t) - sizeof(block_op_t);
}

static void sdmmc_queue(void* ctx, block_op_t* txn) {
    zx_status_t st = ZX_OK;
    sdmmc_device_t* dev = ctx;
    sdmmc_request_t* req = containerof(txn, sdmmc_request_t, bop);

    if ((txn->command == BLOCK_OP_READ) || (txn->command == BLOCK_OP_WRITE)) {
        // Figure out which SD command we need to issue
        bool multiblk = txn->rw.length > 1;
        if (txn->command == BLOCK_OP_READ) {
            req->cmd = multiblk ? SDMMC_READ_MULTIPLE_BLOCK : SDMMC_READ_BLOCK;
        } else {
            req->cmd = multiblk ? SDMMC_WRITE_MULTIPLE_BLOCK : SDMMC_WRITE_BLOCK;
        }

        req->blockcount = txn->rw.length;
        req->blocksize = dev->block_info.block_size;

        if (sdmmc_use_dma(dev)) {
            req->use_dma = true;

            zx_status_t st = ZX_OK;
            size_t bytes = txn->rw.length * req->blocksize;
            zx_handle_t vmo = txn->rw.vmo;
            if ((st = zx_vmo_op_range(vmo, ZX_VMO_OP_COMMIT, txn->rw.offset_vmo, bytes,
                                      NULL, 0)) != ZX_OK) {
                zxlogf(TRACE, "sdmmc: could not commit pages\n");
                goto err;
            }
        } else {
        }

    } else if (txn->command == BLOCK_OP_FLUSH) {
    } else {
        st = ZX_ERR_NOT_SUPPORTED;
        return;
    }
    return;
err:
    txn->completion_cb(txn, st);
}

// Block protocol
static block_protocol_ops_t block_proto = {
    .query = sdmmc_query,
    .queue = sdmmc_queue,
};

static zx_status_t sdmmc_wait_for_tran(sdmmc_device_t* dev) {
    sdmmc_request_t req;
    uint32_t current_state;
    const size_t max_attempts = 10;
    size_t attempt = 0;
    for (; attempt <= max_attempts; attempt++) {
        zx_status_t st = sdmmc_send_status(dev, &req, dev->rca);
        if (st != ZX_OK) {
            zxlogf(SPEW, "sdmmc: SDMMC_SEND_STATUS error, retcode = %d\n", st);
            return st;
        }

        current_state = MMC_STATUS_CURRENT_STATE(req.response);
        if (current_state == MMC_STATUS_CURRENT_STATE_RECV) {
            st = sdmmc_stop_transmission(dev, &req);
            continue;
        } else if (current_state == MMC_STATUS_CURRENT_STATE_TRAN) {
            break;
        }

        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    }

    if (attempt == max_attempts) {
        // Too many retries, fail.
        return ZX_ERR_TIMED_OUT;
    } else {
        return ZX_OK;
    }
}

static void sdmmc_do_txn(sdmmc_device_t* dev, iotxn_t* txn) {
    zxlogf(SPEW, "sdmmc: do_txn txn %p offset 0x%" PRIx64
                 " length 0x%" PRIx64 "\n", txn, txn->offset, txn->length);

    // if this txn is for sync, the only thing to do is complete it
    if ((txn->length == 0) && (txn->flags & IOTXN_SYNC_BEFORE)) {
        iotxn_complete(txn, ZX_OK, 0);
        return;
    }

    uint32_t cmd = 0;

    // Figure out which SD command we need to issue.
    switch(txn->opcode) {
        case IOTXN_OP_READ:
            if (txn->length > SDHC_BLOCK_SIZE) {
                cmd = SDMMC_READ_MULTIPLE_BLOCK;
            } else {
                cmd = SDMMC_READ_BLOCK;
            }
            break;
        case IOTXN_OP_WRITE:
            if (txn->length > SDHC_BLOCK_SIZE) {
                cmd = SDMMC_WRITE_MULTIPLE_BLOCK;
            } else {
                cmd = SDMMC_WRITE_BLOCK;
            }
            break;
        default:
            // Invalid opcode?
            zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d\n", txn, ZX_ERR_INVALID_ARGS);
            iotxn_complete(txn, ZX_ERR_INVALID_ARGS, 0);
            return;
    }

    sdmmc_request_t req = {
        .cmd = cmd,
        (sdmmc_request_t*).arg = txn->offset / SDHC_BLOCK_SIZE,
        .blockcount = txn->length / SDHC_BLOCK_SIZE,
        .blocksize = SDHC_BLOCK_SIZE,
    };
    zx_status_t st = ZX_OK;
    if (sdmmc_use_dma(dev)) {
        st = iotxn_physmap(txn);
        if (st != ZX_OK) {
            zxlogf(SPEW, "sdmmc: do_txn iotxn_physmap error %d\n", st);
            iotxn_complete(txn, st, 0);
            return;
        }

        if (txn->opcode == IOTXN_OP_READ) {
            iotxn_cache_flush_invalidate(txn, 0, txn->length);
        } else {
            iotxn_cache_flush(txn, 0, txn->length);
        }

        req.use_dma = true;
        req.phys.phys = txn->phys;
        req.phys.phys_count = txn->phys_count;
        req.phys.length = txn->length;
        req.phys.vmo_offset = txn->vmo_offset;
    } else {
        req.use_dma = false;
        st = iotxn_mmap(txn, &req.virt);
        if (st != ZX_OK) {
            zxlogf(SPEW, "sdmmc: do_txn iotxn_mmap error %d\n", st);
            iotxn_complete(txn, st, 0);
            return;
        }
    }

    st = sdmmc_do_request(dev, &req);
    if (st != ZX_OK) {
        zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d (cmd 0x%x)\n", txn, st, cmd);
        iotxn_complete(txn, st, 0);
        return;
    }

    zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d\n", txn, ZX_OK);
    iotxn_complete(txn, ZX_OK, txn->length);
}

static int sdmmc_worker_thread(void* arg) {
    zx_status_t st = ZX_OK;
    sdmmc_device_t* dev = (sdmmc_device_t*)arg;

    st = sdmmc_host_info(&dev->host, &dev->host_info);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdmmc: failed to get host info\n");
        return st;
    }

    zxlogf(TRACE, "sdmmc: host caps dma %d 8-bit bus %d max_transfer_size %" PRIu64 "\n",
           sdmmc_use_dma(dev) ? 1 : 0,
           (dev->host_info.caps & SDMMC_HOST_CAP_BUS_WIDTH_8) ? 1 : 0,
           dev->host_info.max_transfer_size);

    dev->block_info.max_transfer_size = dev->host_info.max_transfer_size;

    // Reset the card.
    sdmmc_hw_reset(&dev->host);

    // No matter what state the card is in, issuing the GO_IDLE_STATE command will
    // put the card into the idle state.
    sdmmc_request_t req;
    if ((st = sdmmc_go_idle(dev, &req)) != ZX_OK) {
        zxlogf(ERROR, "sdmmc: SDMMC_GO_IDLE_STATE failed, retcode = %d\n", st);
        device_remove(dev->zxdev);
        return st;
    }

    // Probe for SD, then MMC
    if ((st = sdmmc_probe_sd(dev)) != ZX_OK) {
        if ((st = sdmmc_probe_mmc(dev)) != ZX_OK) {
            zxlogf(ERROR, "sdmmc: failed to probe\n");
            device_remove(dev->zxdev);
            return st;
        }
    }

    // Device must be in TRAN state at this point
    st = sdmmc_wait_for_tran(dev);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdmmc: waiting for TRAN state failed, retcode = %d\n", st);
        device_remove(dev->zxdev);
        return st;
    }

    device_make_visible(dev->zxdev);

    for (;;) {
        // don't loop until txn_list is empty to check for SDMMC_SHUTDOWN
        // between each txn.
        mtx_lock(&dev->lock);
        iotxn_t* txn = list_remove_head_type(&dev->txn_list, iotxn_t, node);
        mtx_unlock(&dev->lock);
        if (txn) {
            sdmmc_do_txn(dev, txn);
        } else {
            zx_object_signal(dev->worker_event, SDMMC_IOTXN_RECEIVED, 0);
        }

        uint32_t pending;
        zx_status_t st = zx_object_wait_one(dev->worker_event,
                SDMMC_IOTXN_RECEIVED | SDMMC_SHUTDOWN, ZX_TIME_INFINITE, &pending);
        if (st != ZX_OK) {
            zxlogf(ERROR, "sdmmc: worker thread wait failed, retcode = %d\n", st);
            break;
        }
        if (pending & SDMMC_SHUTDOWN) {
            zx_object_signal(dev->worker_event, pending, SDMMC_SHUTDOWN_DONE);
            break;
        }
    }

    zxlogf(TRACE, "sdmmc: worker thread terminated\n");

    return 0;
}

static zx_status_t sdmmc_bind(void* ctx, zx_device_t* parent) {
    // Allocate the device.
    sdmmc_device_t* dev = calloc(1, sizeof(*dev));
    if (!dev) {
        zxlogf(ERROR, "sdmmc: no memory to allocate sdmmc device!\n");
        return ZX_ERR_NO_MEMORY;
    }

    zx_status_t st = device_get_protocol(parent, ZX_PROTOCOL_SDMMC, &dev->host);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdmmc: failed to get sdmmc protocol\n");
        st = ZX_ERR_NOT_SUPPORTED;
        goto fail;
    }

    mtx_init(&dev->lock, mtx_plain);
    list_initialize(&dev->txn_list);

    st = zx_event_create(0, &dev->worker_event);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdmmc: failed to create event, retcode = %d\n", st);
        goto fail;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "sdmmc",
        .ctx = dev,
        .ops = &sdmmc_device_proto,
        .proto_id = ZX_PROTOCOL_BLOCK_CORE,
        .flags = DEVICE_ADD_INVISIBLE,
    };

    st = device_add(parent, &args, &dev->zxdev);
    if (st != ZX_OK) {
        goto fail;
    }

    // bootstrap in a thread
    int rc = thrd_create_with_name(&dev->worker_thread, sdmmc_worker_thread, dev, "sdmmc-worker");
    if (rc != thrd_success) {
        st = thrd_status_to_zx_status(rc);
        goto fail_remove;
    }
    dev->worker_thread_running = true;

    return ZX_OK;

fail_remove:
    device_remove(dev->zxdev);
fail:
    free(dev);
    return st;
}

static zx_driver_ops_t sdmmc_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = sdmmc_bind,
};

// The formatter does not play nice with these macros.
// clang-format off
ZIRCON_DRIVER_BEGIN(sdmmc, sdmmc_driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_SDMMC),
ZIRCON_DRIVER_END(sdmmc)
// clang-format on
