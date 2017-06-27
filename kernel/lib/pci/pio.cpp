// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <assert.h>
#include <kernel/mutex.h>
#include <magenta/types.h>
#include <lib/pci/pio.h>
#include <kernel/auto_lock.h>

// TODO: This library exists as a shim for the awkward period between bringing
// PCI legacy support online, and moving PCI to userspace. Initially, it exists
// as a kernel library that userspace accesses via syscalls so that a userspace
// process never causes a race condition with the bus driver's accesses. Later,
// all accesses will go through the library itself in userspace and the syscalls
// will no longer exist.

namespace PCI {

static mutex_t pio_lock = MUTEX_INITIAL_VALUE(pio_lock);
#ifdef ARCH_X86
#include <arch/x86.h>

static constexpr uint16_t kPciConfigAddr = 0xCF8;
static constexpr uint16_t kPciConfigData = 0xCFC;

static constexpr uint32_t PciBdfAddr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    return ((1 << 31) |           // bit 31: enable bit, bits 30-24 reserved
           ((bus & 0xFF) << 16) | // bits 23-16 bus
           ((dev & 0x1F) << 11) | // bits 15-11 device
           ((func & 0x7) << 8) |  // bifs 10-8 func
           (off & 0xFC));         // bits 7-2 register, bits 1 & 0 must be zero
}

mx_status_t PioCfgRead(uint32_t addr, uint32_t* val, size_t width) {
    mxtl::AutoLock lock(&pio_lock);

    size_t shift = (addr & 0x3) * 8;
    if (shift + width > 32) {
        return MX_ERR_INVALID_ARGS;
    }

    outpd(kPciConfigAddr, addr);
    uint32_t tmp_val = inpd(kPciConfigData);
    uint32_t width_mask = (1 << width) - 1;

    // Align the read to the correct offset, then mask based on byte width
    *val = (tmp_val >> shift) & width_mask;
    return MX_OK;
}

mx_status_t PioCfgRead(uint8_t bus, uint8_t dev, uint8_t func,
                             uint8_t offset, uint32_t* val, size_t width) {
    return PioCfgRead(PciBdfAddr(bus, dev, func, offset), val, width);
}

mx_status_t PioCfgWrite(uint32_t addr, uint32_t val, size_t width) {
    mxtl::AutoLock lock(&pio_lock);

    size_t shift = (addr & 0x3) * 8;
    if (shift + width > 32) {
        return MX_ERR_INVALID_ARGS;
    }

    uint32_t width_mask = (1 << width) - 1;
    uint32_t write_mask = width_mask << shift;
    outpd(kPciConfigAddr, addr);
    uint32_t tmp_val = inpd(kPciConfigData);

    val &= width_mask;
    tmp_val &= ~write_mask;
    tmp_val |= (val << shift);
    outpd(kPciConfigData, tmp_val);

    return MX_OK;
}

mx_status_t PioCfgWrite(uint8_t bus, uint8_t dev, uint8_t func,
                              uint8_t offset, uint32_t val, size_t width) {
    return PioCfgWrite(PciBdfAddr(bus, dev, func, offset), val, width);
}

#else // not x86
mx_status_t PioCfgRead(uint32_t addr, uint32_t* val, size_t width) {
    return MX_ERR_NOT_SUPPORTED;
}

mx_status_t PioCfgRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                             uint32_t* val, size_t width) {
    return MX_ERR_NOT_SUPPORTED;
}

mx_status_t PioCfgWrite(uint32_t addr, uint32_t val, size_t width) {
    return MX_ERR_NOT_SUPPORTED;;
}

mx_status_t PioCfgWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                             uint32_t val, size_t width) {
    return MX_ERR_NOT_SUPPORTED;
}

#endif // ARCH_X86
}; // namespace PCI
