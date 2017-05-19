// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "device_context.h"

#include <kernel/vm.h>
#include <mxalloc/new.h>
#include <mxtl/unique_ptr.h>
#include <trace.h>

#include "hw.h"
#include "iommu_impl.h"

#define LOCAL_TRACE 0

namespace intel_iommu {

DeviceContext::DeviceContext(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                         volatile ds::ExtendedContextEntry* context_entry)
        : parent_(parent), extended_context_entry_(context_entry), initialized_(false),
          bus_(bus), dev_func_(dev_func), extended_(true) {
}

DeviceContext::DeviceContext(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                         volatile ds::ContextEntry* context_entry)
        : parent_(parent), context_entry_(context_entry), initialized_(false),
          bus_(bus), dev_func_(dev_func), extended_(false) {
}

DeviceContext::~DeviceContext() {
    if (extended_) {
        ds::ExtendedContextEntry entry;
        entry.ReadFrom(extended_context_entry_);
        entry.set_present(0);
        entry.WriteTo(extended_context_entry_);
    } else {
        ds::ContextEntry entry;
        entry.ReadFrom(context_entry_);
        entry.set_present(0);
        entry.WriteTo(context_entry_);
    }

    // TODO(teisenbe): Perform a context cache flush

    if (initialized_) {
        status_t status = second_level_pt_.Destroy();
        ASSERT(status == MX_OK);
    }
}

status_t DeviceContext::Create(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                             volatile ds::ContextEntry* context_entry,
                             mxtl::unique_ptr<DeviceContext>* device) {
    uint8_t aspace_width = 0;
    auto caps = parent->caps();
    if (caps->supports_48_bit_agaw()) {
        aspace_width = 48;
    } else if (caps->supports_39_bit_agaw()) {
        aspace_width = 39;
    }

    ds::ContextEntry entry;
    entry.ReadFrom(context_entry);

    // It's a bug if we're trying to re-initialize an existing entry
    ASSERT(!entry.present());

    AllocChecker ac;
    mxtl::unique_ptr<DeviceContext> dev(new (&ac) DeviceContext(bus, dev_func,
                                                                parent, context_entry));
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    status_t status = dev->second_level_pt_.Init(0, 1ull << aspace_width,
                                                 ARCH_ASPACE_FLAG_GUEST_PASPACE);
    if (status != MX_OK) {
        return status;
    }
    dev->initialized_ = true;

    entry.set_present(1);
    entry.set_fault_processing_disable(0);
    entry.set_translation_type(ds::ContextEntry::kDeviceTlbDisabled);
    // TODO: don't hardcode this, and make an enum
    entry.set_address_width(0b010);
    // TODO: Use a real allocator here
    static uint64_t next_domain_id = 0;
    entry.set_domain_id(next_domain_id++);
    entry.set_second_level_pt_ptr(dev->second_level_pt_.pt_phys() >> 12);

    entry.WriteTo(context_entry);

    *device = mxtl::move(dev);
    return MX_OK;
}

status_t DeviceContext::Create(uint8_t bus, uint8_t dev_func, IommuImpl* parent,
                             volatile ds::ExtendedContextEntry* context_entry,
                             mxtl::unique_ptr<DeviceContext>* device) {

    uint8_t aspace_width = 0;
    auto caps = parent->caps();
    if (caps->supports_48_bit_agaw()) {
        aspace_width = 48;
    } else if (caps->supports_39_bit_agaw()) {
        aspace_width = 39;
    }

    ds::ExtendedContextEntry entry;
    entry.ReadFrom(context_entry);

    // It's a bug if we're trying to re-initialize an existing entry
    ASSERT(!entry.present());

    AllocChecker ac;
    mxtl::unique_ptr<DeviceContext> dev(new (&ac) DeviceContext(bus, dev_func,
                                                                parent, context_entry));
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    status_t status = dev->second_level_pt_.Init(0, 1ull << aspace_width,
                                                 ARCH_ASPACE_FLAG_GUEST_PASPACE);
    if (status != MX_OK) {
        return status;
    }
    dev->initialized_ = true;

    entry.set_present(1);
    entry.set_fault_processing_disable(0);
    entry.set_translation_type(ds::ExtendedContextEntry::kHostModeWithDeviceTlbDisabled);
    entry.set_deferred_invld_enable(0);
    entry.set_page_request_enable(0);
    entry.set_nested_translation_enable(0);
    entry.set_pasid_enable(0);
    entry.set_global_page_enable(0);
    // TODO: don't hardcode this, and make an enum
    entry.set_address_width(0b010);
    entry.set_no_exec_enable(1);
    entry.set_write_protect_enable(1);
    // TODO: reconsider
    entry.set_cache_disable(0);
    entry.set_extended_mem_type_enable(0);
    // TODO: Use a real allocator here
    static uint64_t next_domain_id = 0;
    entry.set_domain_id(next_domain_id++);
    entry.set_smep_enable(1);
    entry.set_extended_accessed_flag_enable(0);
    entry.set_execute_requests_enable(0);
    entry.set_second_level_execute_bit_enable(0);
    entry.set_second_level_pt_ptr(dev->second_level_pt_.pt_phys() >> 12);

    entry.WriteTo(context_entry);

    *device = mxtl::move(dev);
    return MX_OK;
}

status_t DeviceContext::SecondLevelMap(paddr_t paddr, size_t size, uint32_t perms,
                                       paddr_t* virt_paddr) {
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr));
    size_t mapped;
    uint flags = 0;
    // TODO: Don't use ARCH_MMU_FLAGs here, should be arch agnostic
    if (perms & IOMMU_FLAG_PERM_READ) {
        flags |= ARCH_MMU_FLAG_PERM_READ;
    }
    if (perms & IOMMU_FLAG_PERM_WRITE) {
        flags |= ARCH_MMU_FLAG_PERM_WRITE;
    }
    if (perms & IOMMU_FLAG_PERM_EXECUTE) {
        flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
    }
    status_t status = second_level_pt_.Map(paddr, paddr, size / PAGE_SIZE, flags,
                                           &mapped);
    if (status != MX_OK) {
        return status;
    }
    ASSERT(mapped == size / PAGE_SIZE);
    *virt_paddr = paddr;
    LTRACEF("Map(%02x:%02x.%1x): [%p, %p) -> %p %#x\n", bus_, (unsigned int)dev_func_ >> 3, (unsigned int)dev_func_ & 0x7, (void*)paddr, (void*)(paddr + size), (void*)paddr, flags);
    return MX_OK;
}

status_t DeviceContext::SecondLevelUnmap(paddr_t virt_paddr, size_t size) {
    DEBUG_ASSERT(IS_PAGE_ALIGNED(virt_paddr));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));
    size_t unmapped;
    LTRACEF("Unmap(%02x:%02x.%1x): [%p, %p)\n", bus_, (unsigned int)dev_func_ >> 3, (unsigned int)dev_func_ & 0x7, (void*)virt_paddr, (void*)(virt_paddr + size));
    return second_level_pt_.Unmap(virt_paddr, size / PAGE_SIZE, &unmapped);
}

} // namespace intel_iommu
