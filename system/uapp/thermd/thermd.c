// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/device/sysinfo.h>
#include <zircon/device/thermal.h>
#include <zircon/syscalls/system.h>
#include <zircon/syscalls.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

static zx_handle_t root_resource;

static uint32_t pl1_mw; // current PL1 value

#define PL1_MIN 2500
#define PL1_MAX 7000

static zx_status_t get_root_resource(zx_handle_t* root_resource) {
    int fd = open("/dev/misc/sysinfo", O_RDWR);
    if (fd < 0) {
        return ZX_ERR_NOT_FOUND;
    }

    ssize_t n = ioctl_sysinfo_get_root_resource(fd, root_resource);
    close(fd);
    if (n != sizeof(*root_resource)) {
        if (n < 0) {
            return (zx_status_t)n;
        } else {
            return ZX_ERR_NOT_FOUND;
        }
    }
    return ZX_OK;
}

static zx_status_t set_pl1(uint32_t target) {
#if 0
    zx_system_powerctl_arg_t arg = {
        .x86_power_limit = {
            .power_limit = target,
            .time_window = 0,
            .clamp = 1,
            .enable = 1,
        },
    };
    zx_status_t st = zx_system_powerctl(root_resource, ZX_SYSTEM_POWERCTL_X86_SET_PKG_PL1, &arg);
    if (st != ZX_OK) {
        fprintf(stderr, "ERROR: Failed to set PL1 to %d: %d\n", target, st);
        return -1;
    }
    pl1_mw = target;
#endif
    return ZX_OK;
}

static uint32_t to_celsius(uint32_t val) {
    // input is 10th of a kelvin
    return (val * 10 - 27315) / 100;
}

static uint32_t to_kelvin(uint32_t celsius) {
    // return in 10th of a kelvin
    return (celsius * 100 + 27315) / 10;
}

int main(int argc, char** argv) {
    zx_status_t st = get_root_resource(&root_resource);
    if (st != ZX_OK) {
        fprintf(stderr, "ERROR: Failed to get root resource: %d\n", st);
        return -1;
    }

    // first sensor is ambient sensor
    int fd = open("/dev/class/thermal/000", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open sensor: %d\n", fd);
        return -1;
    }

    uint32_t temp;
    ssize_t rc = read(fd, &temp, sizeof(temp));
    if (rc != sizeof(temp)) {
        return rc;
    }
    printf("Temp is %u C \n", to_celsius(temp));

    thermal_info_t info;
    rc = ioctl_thermal_get_info(fd, &info);
    if (rc != sizeof(info)) {
        fprintf(stderr, "ERROR: Failed to get thermal info: %zd\n", rc);
        return rc;
    }

    printf("Passive temp is %u C\n", to_celsius(info.passive_temp));
    printf("Critical temp is %u C\n", to_celsius(info.critical_temp));

    zx_handle_t h = ZX_HANDLE_INVALID;
    rc = ioctl_thermal_get_state_change_event(fd, &h);
    if (rc != sizeof(h)) {
        fprintf(stderr, "ERROR: Failed to get event: %zd\n", rc);
        return rc;
    }

    if (info.max_trip_count == 0) {
        printf("Trip points not supported, exiting\n");
        return 0;
    }

    // Set a trip point
    trip_point_t tp = {
        .id = 0,
        .temp = info.passive_temp,
    };
    rc = ioctl_thermal_set_trip(fd, &tp);
    if (rc) {
        fprintf(stderr, "ERROR: Failed to set trip point: %zd\n", rc);
        return rc;
    }
    printf("Trip point set to %u C\n", to_celsius(tp.temp));

    // set PL1 to 7 watts (EDP)
    set_pl1(PL1_MAX);

    for (;;) {
        zx_signals_t observed = 0;
        st = zx_object_wait_one(h, ZX_USER_SIGNAL_0,
                                zx_deadline_after(ZX_SEC(1)),
                                &observed);
        if ((st != ZX_OK) && (st != ZX_ERR_TIMED_OUT)) {
            fprintf(stderr, "ERROR: Failed to wait on event: %d\n", st);
            return st;
        }
        if (observed & ZX_USER_SIGNAL_0) {
            rc = ioctl_thermal_get_info(fd, &info);
            if (rc != sizeof(info)) {
                fprintf(stderr, "ERROR: Failed to get thermal info: %zd\n", rc);
                return rc;
            }
            if (info.state) {
                printf("Trip point event, throttling\n");

                set_pl1(PL1_MIN); // decrease power limit

                rc = read(fd, &temp, sizeof(temp));
                if (rc != sizeof(temp)) {
                    fprintf(stderr, "ERROR: Failed to read temperature: %zd\n", rc);
                    return rc;
                }
                printf("Temp is %u C \n", to_celsius(temp));
            } else {
                printf("spurious thermal event\n");
            }
        }
        if (st == ZX_ERR_TIMED_OUT) {
            rc = read(fd, &temp, sizeof(temp));
            if (rc != sizeof(temp)) {
                fprintf(stderr, "ERROR: Failed to read temperature: %zd\n", rc);
                return rc;
            }
            printf("Temp is %u C State 0x%x PL1 %u Trip %u C\n", to_celsius(temp),
                   info.state, pl1_mw, to_celsius(info.active_trip[0]));

            // increase power limit if the temperature dropped enough
            if ((temp < info.active_trip[0] - 50) && (pl1_mw != PL1_MAX)) {
                // make sure the state is clear
                rc = ioctl_thermal_get_info(fd, &info);
                if (rc != sizeof(info)) {
                    fprintf(stderr, "ERROR: Failed to get thermal info: %zd\n", rc);
                    return rc;
                }
                if (!info.state) {
                    set_pl1(PL1_MAX);
                    printf("Reset throttling\n");
                }
            }

            if ((temp > info.active_trip[0]) && (pl1_mw != PL1_MIN)) {
                printf("Trip point, throttling\n");

                set_pl1(PL1_MIN); // decrease power limit
            }
        }
    }

    close(fd);

    return 0;
}
