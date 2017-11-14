// Copyright 2017 The Fuchsia Authors
// Copyright (c) 2016 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <zircon/compiler.h>
#include <err.h>
#include <inttypes.h>
#include <string.h>
#include <arch/x86/feature.h>
#include <kernel/auto_lock.h>
#include <kernel/mp.h>
#include <lib/console.h>

#define PRINT_MSR(msr)     print_msr(msr, #msr, 0)
#define PRINT_CLR_MSR(msr) print_msr(msr, #msr, 1)

static void print_msr(uint32_t msr_id, const char* name, bool clear) {
    uint64_t v = read_msr(msr_id);
    printf("    %s=0x%016" PRIx64 "\n", name, v);
    if (clear) write_msr(msr_id, v);
}

static void thermal_disable(void) {
    // disable PL1/2
    uint64_t v = read_msr(X86_MSR_PKG_POWER_LIMIT);
    v &= ~((3 << 15) | (3llu << 47));
    write_msr(X86_MSR_PKG_POWER_LIMIT, v);
}

static void thermal_default_pl1(void) {
    uint64_t d = read_msr(X86_MSR_PKG_POWER_INFO) & 0x7f;
    uint64_t v = read_msr(X86_MSR_PKG_POWER_LIMIT);
    v &= ~0x7f;
    v |= d;
    write_msr(X86_MSR_PKG_POWER_LIMIT, v);
}

static void thermal_set_pl1(void) {
    uint64_t target_w = 3; // watts

    // sets PL1
    uint64_t u = (1 << (read_msr(X86_MSR_RAPL_POWER_UNIT) & 0xf));
    uint64_t v = read_msr(X86_MSR_PKG_POWER_LIMIT);
    v &= ~0x7f;
    v |= (target_w * u) & 0x7f;
    write_msr(X86_MSR_PKG_POWER_LIMIT, v);
}

static void thermal_dump(void) {
    PRINT_MSR(X86_MSR_IA32_MISC_ENABLE);
    uint64_t v = read_msr(X86_MSR_IA32_MISC_ENABLE);
    printf("  EIST %s\n", (v & (1 << 16)) ? "enabled" : "disabled");
    printf("  OPP %s\n", (v & (1llu << 38)) ? "supported" : "unsupported");

    PRINT_MSR(X86_MSR_IA32_PERF_CTL);
    v = read_msr(X86_MSR_IA32_PERF_CTL);
    printf("  IDA/Turbo %s\n", (v & (1llu << 32)) ? "disabled" : "enabled");

    PRINT_MSR(X86_MSR_IA32_PM_ENABLE);
    printf("  HWP %s\n", x86_feature_test(X86_FEATURE_HWP) ? "supported" : "unsupported");
    printf("  HWP %s\n", (read_msr(X86_MSR_IA32_PM_ENABLE) & 1) ? "enabled" : "disabled");

    PRINT_MSR(X86_MSR_IA32_PKG_HDC_CTL);
    printf("  HDC %s\n", (read_msr(X86_MSR_IA32_PKG_HDC_CTL) & 1) ? "enabled" : "disabled");

    PRINT_MSR(X86_MSR_IA32_CLOCK_MODULATION);

    PRINT_MSR(X86_MSR_IA32_THERM_STATUS);
    PRINT_MSR(X86_MSR_IA32_THERM_INTERRUPT);
    PRINT_MSR(X86_MSR_IA32_PACKAGE_THERM_STATUS);
    PRINT_MSR(X86_MSR_IA32_PACKAGE_THERM_INTERRUPT);
    PRINT_MSR(X86_MSR_THERM2_CTL);
    PRINT_MSR(X86_MSR_RAPL_POWER_UNIT);

    PRINT_MSR(X86_MSR_PKG_POWER_LIMIT);
    v = read_msr(X86_MSR_PKG_POWER_LIMIT);
    printf("  PKG PL1 enable %s clamp %s\n", (v & (1 << 15)) ? "enabled" : "disabled",
                                             (v & (1 << 16)) ? "enabled" : "disabled");
    printf("  PKG PL2 enable %s clamp %s\n", (v & (1llu << 47)) ? "enabled" : "disabled",
                                             (v & (1llu << 48)) ? "enabled" : "disabled");

    PRINT_MSR(X86_MSR_PKG_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_PKG_PERF_STATUS);
    PRINT_MSR(X86_MSR_PKG_POWER_INFO);
#if 0
    PRINT_MSR(X86_MSR_DRAM_POWER_LIMIT);
    PRINT_MSR(X86_MSR_DRAM_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_DRAM_PERF_STATUS);
    PRINT_MSR(X86_MSR_DRAM_POWER_INFO);
    PRINT_MSR(X86_MSR_PP0_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_PP0_POWER_LIMIT);
    PRINT_MSR(X86_MSR_PP0_POLICY);
    PRINT_MSR(X86_MSR_PP1_ENERGY_STATUS);
    PRINT_MSR(X86_MSR_PP1_POWER_LIMIT);
    PRINT_MSR(X86_MSR_PP1_POLICY);
#endif
}

static int cmd_thermal(int argc, const cmd_args* argv, uint32_t flags) {
    if (argc < 2) {
usage:
        printf("usage:\n");
        printf("%s dump\n", argv[0].str);
#if 0
        printf("%s set\n", argv[0].str);
        printf("%s default\n", argv[0].str);
#endif
        printf("%s disable\n", argv[0].str);
        return ZX_ERR_INTERNAL;
    }

    if (!strcmp(argv[1].str, "dump")) {
        thermal_dump();
#if 0
    } else if (!strcmp(argv[1].str, "set-pl1")) {
        thermal_set_pl1();
    } else if (!strcmp(argv[1].str, "default-pl1")) {
        thermal_default_pl1();
#endif
    } else if (!strcmp(argv[1].str, "disable")) {
        thermal_disable();
    } else {
        printf("unknown command\n");
        goto usage;
    }
    return ZX_OK;
}

STATIC_COMMAND_START
STATIC_COMMAND("thermal", "thermal features\n", &cmd_thermal)
STATIC_COMMAND_END(thermal);
