// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <bits.h>
#include <dev/pcie_bus_driver.h>
#include <dev/iommu.h>
#include <hwreg/mmio.h>
#include <kernel/mutex.h>
#include <magenta/syscalls/iommu.h>
#include <mxtl/intrusive_double_list.h>
#include <mxtl/macros.h>

#include "hw.h"
#include "iommu_page.h"

class VmMapping;
class VmObject;

namespace intel_iommu {

class ContextTableState;
class DeviceContext;

class IommuImpl final : public Iommu {
public:

    static status_t Create(mxtl::unique_ptr<const uint8_t[]> desc, uint32_t desc_len,
                           mxtl::RefPtr<Iommu>* out);

    bool IsValidBusTxnId(uint64_t bus_txn_id) const final;

    status_t Map(uint64_t bus_txn_id, paddr_t paddr, size_t size, uint32_t perms,
                 dev_vaddr_t* vaddr) final;
    status_t Unmap(uint64_t bus_txn_id, dev_vaddr_t vaddr, size_t size) final;

    status_t ClearMappingsForBusTxnId(uint64_t bus_txn_id) final;

    ~IommuImpl() final;

    reg::Capability* caps() TA_NO_THREAD_SAFETY_ANALYSIS { return &caps_; }
    reg::ExtendedCapability* extended_caps() TA_NO_THREAD_SAFETY_ANALYSIS {
        return &extended_caps_;
    }
private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(IommuImpl);
    IommuImpl(volatile void* register_base, mxtl::unique_ptr<const uint8_t[]> desc,
              uint32_t desc_len);

    static void decode_bus_txn_id(uint64_t bus_txn_id, uint8_t* bus, uint8_t* dev_func) {
        *bus = static_cast<uint8_t>(BITS_SHIFT(bus_txn_id, 15, 8));
        *dev_func = static_cast<uint8_t>(BITS_SHIFT(bus_txn_id, 7, 0));
    }

    static status_t ValidateIommuDesc(const mxtl::unique_ptr<const uint8_t[]>& desc,
                                      uint32_t desc_len);

    // Set up initial root structures and enable translation
    status_t Initialize();

    status_t InvalidateContextCacheGlobalLocked() TA_REQ(lock_);
    status_t InvalidateIotlbGlobalLocked() TA_REQ(lock_);
    status_t SetRootTablePointerLocked(paddr_t pa) TA_REQ(lock_);
    status_t SetTranslationEnableLocked(bool enabled, lk_time_t deadline) TA_REQ(lock_);
    status_t ConfigureFaultEventInterruptLocked() TA_REQ(lock_);
    status_t EnableBiosReservedMappingsLocked() TA_REQ(lock_);
    void DisableFaultsLocked() TA_REQ(lock_);
    static enum handler_return FaultHandler(void* ctx);
    status_t GetOrCreateContextTableLocked(uint8_t bus, uint8_t dev_func,
                                           ContextTableState** tbl) TA_REQ(lock_);
    status_t GetOrCreateDeviceContextLocked(uint8_t bus, uint8_t dev_func,
                                            DeviceContext** context) TA_REQ(lock_);

    // Utility for waiting until a register field changes to a value, timing out
    // if the deadline elapses.  If deadline is INFINITE_TIME, then will never time
    // out.  Can only return NO_ERROR and ERR_TIMED_OUT.
    template <class RegType>
    status_t WaitForValueLocked(RegType* reg,
                                typename RegType::ValueType (RegType::*getter)(),
                                typename RegType::ValueType value,
                                lk_time_t deadline) TA_REQ(lock_);

    volatile ds::RootTable* root_table() const TA_REQ(lock_) {
        return reinterpret_cast<volatile ds::RootTable*>(root_table_page_.vaddr());
    }

    Mutex lock_;

    // Descriptor of this hardware unit
    mxtl::unique_ptr<const uint8_t[]> desc_;
    uint32_t desc_len_;

    // Location of the memory-mapped hardware register bank.
    hwreg::RegisterIo mmio_ TA_GUARDED(lock_);

    // Interrupt allocation
    pcie_msi_block_t irq_block_ TA_GUARDED(lock_);

    // In-memory root table
    IommuPage root_table_page_ TA_GUARDED(lock_);
    // List of allocated context tables
    mxtl::DoublyLinkedList<mxtl::unique_ptr<ContextTableState>> context_tables_ TA_GUARDED(lock_);

    // A mask with bits set for each usable bit in an address with the largest allowed
    // address width.  E.g., if the largest allowed width is 48-bit,
    // max_guest_addr_mask will be 0xffff_ffff_ffff.
    uint64_t max_guest_addr_mask_ TA_GUARDED(lock_) = 0;
    uint32_t num_supported_domains_ TA_GUARDED(lock_) = 0;
    uint32_t valid_pasid_mask_ TA_GUARDED(lock_) = 0;
    uint32_t iotlb_reg_offset_ TA_GUARDED(lock_) = 0;
    uint32_t fault_recording_reg_offset_ TA_GUARDED(lock_) = 0;
    uint32_t num_fault_recording_reg_ TA_GUARDED(lock_) = 0;
    bool supports_extended_context_ TA_GUARDED(lock_) = 0;

    reg::Capability caps_ TA_GUARDED(lock_);
    reg::ExtendedCapability extended_caps_ TA_GUARDED(lock_);
};

} // namespace intel_iommu
