// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

// TODO(teisenbe): Move x86 mmu stuff into a lib, and use a different
// parameterization for the IOMMU.  Slightly different support for things like
// large pages and dynamic multilevel translation.
#include <arch/aspace.h>

#include <mxtl/intrusive_double_list.h>
#include <mxtl/macros.h>
#include <mxtl/unique_ptr.h>

#include "hw.h"

namespace intel_iommu {

class IommuImpl;

class DeviceContext : public mxtl::DoublyLinkedListable<mxtl::unique_ptr<DeviceContext>> {
public:
    ~DeviceContext();

    // Create a new DeviceContext representing the given BDF.  It is a fatal error
    // to try to create a context for a BDF that already has one.
    static status_t Create(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                           volatile ds::ExtendedContextEntry* context_entry,
                           mxtl::unique_ptr<DeviceContext>* device);
    static status_t Create(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                           volatile ds::ContextEntry* context_entry,
                           mxtl::unique_ptr<DeviceContext>* device);

    // Check if this DeviceContext is for the given BDF
    bool is_bdf(uint8_t bus, uint8_t dev_func) const {
        return bus == bus_ && dev_func == dev_func_;
    }

    // Use the second-level translation table to map the host address |paddr| to the
    // guest's address |*virt_paddr|.  |size| is in bytes.
    status_t SecondLevelMap(paddr_t paddr, size_t size, uint32_t perms, paddr_t* virt_paddr);
    status_t SecondLevelUnmap(paddr_t virt_paddr, size_t size);

private:
    DeviceContext(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                  volatile ds::ExtendedContextEntry* context_entry);
    DeviceContext(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                  volatile ds::ContextEntry* context_entry);

    DISALLOW_COPY_ASSIGN_AND_MOVE(DeviceContext);

    IommuImpl* const parent_;
    union {
        volatile ds::ExtendedContextEntry* const extended_context_entry_;
        volatile ds::ContextEntry* const context_entry_;
    };

    // Page tables used for translating requests-without-PASID and for nested
    // translation of requests-with-PASID.
    ArchVmAspace second_level_pt_;

    bool initialized_;

    const uint8_t bus_;
    const uint8_t dev_func_;
    const bool extended_;
};

} // namespace intel_iommu
