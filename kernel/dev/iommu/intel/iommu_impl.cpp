// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "iommu_impl.h"

#include <err.h>
#include <kernel/auto_lock.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_aspace.h>
#include <kernel/vm/vm_object_paged.h>
#include <mxalloc/new.h>
#include <mxtl/algorithm.h>
#include <mxtl/limits.h>
#include <mxtl/ref_ptr.h>
#include <mxtl/unique_ptr.h>
#include <platform.h>
#include <trace.h>

#include "context_table_state.h"
#include "device_context.h"
#include "hw.h"

#define LOCAL_TRACE 1

namespace intel_iommu {

IommuImpl::IommuImpl(volatile void* register_base,
                     mxtl::unique_ptr<const uint8_t[]> desc, uint32_t desc_len)
    : desc_(mxtl::move(desc)), desc_len_(desc_len), mmio_(register_base) {
    memset(&irq_block_, 0, sizeof(irq_block_));
}

status_t IommuImpl::Create(mxtl::unique_ptr<const uint8_t[]> desc_bytes, uint32_t desc_len,
                           mxtl::RefPtr<Iommu>* out) {
    status_t status = ValidateIommuDesc(desc_bytes, desc_len);
    if (status != MX_OK) {
        return status;
    }

    auto desc = reinterpret_cast<const mx_iommu_desc_intel_t*>(desc_bytes.get());
    const uint64_t register_base = desc->register_base;

    auto kernel_aspace = VmAspace::kernel_aspace();
    void *vaddr;
    status = kernel_aspace->AllocPhysical(
            "iommu",
            PAGE_SIZE,
            &vaddr,
            PAGE_SIZE_SHIFT,
            register_base,
            0,
            ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE);
    if (status != MX_OK) {
        return status;
    }

    AllocChecker ac;
    auto instance = mxtl::AdoptRef<IommuImpl>(new (&ac) IommuImpl(vaddr, mxtl::move(desc_bytes),
                                                                  desc_len));
    if (!ac.check()) {
        kernel_aspace->FreeRegion(reinterpret_cast<vaddr_t>(vaddr));
        return MX_ERR_NO_MEMORY;
    }

    status = instance->Initialize();
    if (status != MX_OK) {
        return status;
    }

    *out = mxtl::move(instance);
    return MX_OK;
}

IommuImpl::~IommuImpl() {
    AutoLock guard(&lock_);

    // We cannot unpin memory until translation is disabled
    status_t status = SetTranslationEnableLocked(false, INFINITE_TIME);
    ASSERT(status == MX_OK);

    DisableFaultsLocked();
    auto& pcie_platform = PcieBusDriver::GetDriver()->platform();
    pcie_platform.FreeMsiBlock(&irq_block_);

    VmAspace::kernel_aspace()->FreeRegion(mmio_.base());
}

status_t IommuImpl::ValidateIommuDesc(const mxtl::unique_ptr<const uint8_t[]>& desc_bytes,
                                      uint32_t desc_len) {
    auto desc = reinterpret_cast<const mx_iommu_desc_intel_t*>(desc_bytes.get());

    // Validate the size
    if (desc_len < sizeof(*desc)) {
        LTRACEF("desc too short: %u < %zu\n", desc_len, sizeof(*desc));
        return MX_ERR_INVALID_ARGS;
    }
    static_assert(sizeof(desc->scope_bytes) < sizeof(size_t),
                  "if this changes, need to check for overflow");
    const size_t actual_size = sizeof(*desc) + desc->scope_bytes + desc->reserved_memory_bytes;
    if (desc_len != actual_size) {
        LTRACEF("desc size mismatch: %u != %zu\n", desc_len, actual_size);
        return MX_ERR_INVALID_ARGS;
    }

    // Validate scopes
    if (desc->scope_bytes == 0 && !desc->whole_segment) {
        LTRACEF("desc has no scopes\n");
        return MX_ERR_INVALID_ARGS;
    }
    const size_t num_scopes = desc->scope_bytes / sizeof(mx_iommu_desc_intel_scope_t);
    if (num_scopes * sizeof(mx_iommu_desc_intel_scope_t) != desc->scope_bytes) {
        LTRACEF("desc has invalid scope_bytes field\n");
        return MX_ERR_INVALID_ARGS;
    }

    auto scopes = reinterpret_cast<mx_iommu_desc_intel_scope_t*>(
            reinterpret_cast<uintptr_t>(desc) + sizeof(*desc));
    for (size_t i = 0; i < num_scopes; ++i) {
        if (scopes[i].num_hops == 0) {
            LTRACEF("desc scope %zu has no hops\n", i);
            return MX_ERR_INVALID_ARGS;
        }
        if (scopes[i].num_hops > countof(scopes[0].dev_func)) {
            LTRACEF("desc scope %zu has too many hops\n", i);
            return MX_ERR_INVALID_ARGS;
        }
    }

    // Validate reserved memory regions
    size_t cursor_bytes = sizeof(*desc) + desc->scope_bytes;
    while (cursor_bytes + sizeof(mx_iommu_desc_intel_reserved_memory_t) < desc_len) {
        auto mem = reinterpret_cast<mx_iommu_desc_intel_reserved_memory_t*>(
                reinterpret_cast<uintptr_t>(desc) + cursor_bytes);
        // TODO: overflow checking
        const size_t next_entry = cursor_bytes + sizeof(mx_iommu_desc_intel_reserved_memory_t) + mem->scope_bytes;
        if (next_entry > desc_len) {
            LTRACEF("desc reserved memory entry has invalid scope_bytes\n");
            return MX_ERR_INVALID_ARGS;
        }

        // TODO: Make sure that the reserved memory regions are not in our
        // allocatable RAM pools

        // Validate scopes
        if (mem->scope_bytes == 0) {
            LTRACEF("desc reserved memory entry has no scopes\n");
            return MX_ERR_INVALID_ARGS;
        }
        const size_t num_scopes = mem->scope_bytes / sizeof(mx_iommu_desc_intel_scope_t);
        if (num_scopes * sizeof(mx_iommu_desc_intel_scope_t) != desc->scope_bytes) {
            LTRACEF("desc reserved memory entry has invalid scope_bytes field\n");
            return MX_ERR_INVALID_ARGS;
        }

        auto scopes = reinterpret_cast<mx_iommu_desc_intel_scope_t*>(
                reinterpret_cast<uintptr_t>(mem) + sizeof(*mem));
        for (size_t i = 0; i < num_scopes; ++i) {
            if (scopes[i].num_hops == 0) {
                LTRACEF("desc reserved memory entry scope %zu has no hops\n", i);
                return MX_ERR_INVALID_ARGS;
            }
            if (scopes[i].num_hops > countof(scopes[0].dev_func)) {
                LTRACEF("desc reserved memory entry scope %zu has too many hops\n", i);
                return MX_ERR_INVALID_ARGS;
            }
        }

        cursor_bytes = next_entry;
    }
    if (cursor_bytes != desc_len) {
        LTRACEF("desc has invalid reserved_memory_bytes field\n");
        return MX_ERR_INVALID_ARGS;
    }

    LTRACEF("validated desc\n");
    return MX_OK;
}

bool IommuImpl::IsValidBusTxnId(uint64_t bus_txn_id) const {
    // TODO(teisenbe): Decode the txn id and check against configuration.
    if (bus_txn_id > UINT16_MAX) {
        return false;
    }
    return true;
}

status_t IommuImpl::Map(uint64_t bus_txn_id, paddr_t paddr, size_t size, uint32_t perms,
                        dev_vaddr_t* vaddr) {
    DEBUG_ASSERT(vaddr);
    if (!IS_PAGE_ALIGNED(paddr) || !IS_PAGE_ALIGNED(size)) {
        return MX_ERR_INVALID_ARGS;
    }
    if (perms & ~(IOMMU_FLAG_PERM_READ | IOMMU_FLAG_PERM_WRITE | IOMMU_FLAG_PERM_EXECUTE)) {
        return MX_ERR_INVALID_ARGS;
    }
    if (perms == 0) {
        return MX_ERR_INVALID_ARGS;
    }
    if (!IsValidBusTxnId(bus_txn_id)) {
        return MX_ERR_NOT_FOUND;
    }

    uint8_t bus;
    uint8_t dev_func;
    decode_bus_txn_id(bus_txn_id, &bus, &dev_func);

    AutoLock guard(&lock_);
    DeviceContext* dev;
    status_t status = GetOrCreateDeviceContextLocked(bus, dev_func, &dev);
    if (status != MX_OK) {
        return status;
    }
    status = dev->SecondLevelMap(paddr, size, perms, vaddr);
    if (status != MX_OK) {
        return status;
    }

    ASSERT(!caps_.required_write_buf_flushing());
    // TODO(teisenbe): Integrate finer-grained cache flushing inside of the page
    // table management
    __asm__ volatile("wbinvd" : : : "memory");

    return MX_OK;
}

status_t IommuImpl::Unmap(uint64_t bus_txn_id, dev_vaddr_t vaddr, size_t size) {
    if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(size)) {
        return MX_ERR_INVALID_ARGS;
    }
    if (!IsValidBusTxnId(bus_txn_id)) {
        return MX_ERR_NOT_FOUND;
    }

    uint8_t bus;
    uint8_t dev_func;
    decode_bus_txn_id(bus_txn_id, &bus, &dev_func);

    AutoLock guard(&lock_);
    DeviceContext* dev;
    status_t status = GetOrCreateDeviceContextLocked(bus, dev_func, &dev);
    if (status != MX_OK) {
        return status;
    }
    status = dev->SecondLevelUnmap(vaddr, size);
    if (status != MX_OK) {
        return status;
    }

    // TODO: Is this the right flush?
    // TODO: Do finer grained flushing
    status = InvalidateContextCacheGlobalLocked();
    if (status != MX_OK) {
        return status;
    }
    status = InvalidateIotlbGlobalLocked();
    if (status != MX_OK) {
        return status;
    }

    return MX_OK;
}

status_t IommuImpl::ClearMappingsForBusTxnId(uint64_t bus_txn_id) {
    PANIC_UNIMPLEMENTED;
    return MX_ERR_NOT_SUPPORTED;
}

status_t IommuImpl::Initialize() {
    AutoLock guard(&lock_);

    // Ensure we support this device version
    auto version = reg::Version::Get().ReadFrom(&mmio_);
    if (version.major() != 1 && version.minor() != 0) {
        LTRACEF("Unsupported IOMMU version: %u.%u\n", version.major(), version.minor());
        return MX_ERR_NOT_SUPPORTED;
    }

    // Cache useful capability info
    caps_ = reg::Capability::Get().ReadFrom(&mmio_);
    extended_caps_ = reg::ExtendedCapability::Get().ReadFrom(&mmio_);

    max_guest_addr_mask_ = (1ULL << (caps_.max_guest_addr_width() + 1)) - 1;
    fault_recording_reg_offset_ = static_cast<uint32_t>(
            caps_.fault_recording_register_offset() * 16);
    num_fault_recording_reg_ = static_cast<uint32_t>(caps_.num_fault_recording_reg() + 1);
    iotlb_reg_offset_ = static_cast<uint32_t>(extended_caps_.iotlb_register_offset() * 16);
    if (iotlb_reg_offset_ > PAGE_SIZE - 16) {
        LTRACEF("Unsupported IOMMU: IOTLB offset runs past the register page\n");
        return MX_ERR_NOT_SUPPORTED;
    }
    supports_extended_context_ = extended_caps_.supports_extended_context();
    if (extended_caps_.supports_pasid()) {
        valid_pasid_mask_ = static_cast<uint32_t>((1ULL << (extended_caps_.pasid_size() + 1)) - 1);
    }

    const uint64_t num_domains = caps_.num_domains();
    if (num_domains > 0x6) {
        LTRACEF("Unknown num_domains value\n");
        return MX_ERR_NOT_SUPPORTED;
    }
    num_supported_domains_ = static_cast<uint32_t>(4 + 2 * num_domains);

    // Sanity check initial configuration
    auto global_ctl = reg::GlobalControl::Get().ReadFrom(&mmio_);
    if (global_ctl.translation_enable()) {
        LTRACEF("DMA remapping already enabled?!\n");
        return MX_ERR_BAD_STATE;
    }
    if (global_ctl.interrupt_remap_enable()) {
        LTRACEF("IRQ remapping already enabled?!\n");
        return MX_ERR_BAD_STATE;
    }

    // Allocate and setup the root table
    status_t status = IommuPage::AllocatePage(&root_table_page_);
    if (status != MX_OK) {
        LTRACEF("alloc root table failed\n");
        return status;
    }
    status = SetRootTablePointerLocked(root_table_page_.paddr());
    if (status != MX_OK) {
        LTRACEF("set root table failed\n");
        return status;
    }

    // Enable interrupts before we enable translation
    status = IommuImpl::ConfigureFaultEventInterruptLocked();
    if (status != MX_OK) {
        LTRACEF("configuring fault event irq failed\n");
        return status;
    }

    status = EnableBiosReservedMappingsLocked();
    if (status != MX_OK) {
        LTRACEF("enable bios reserved mappings failed\n");
        return status;
    }

    status = SetTranslationEnableLocked(true, current_time() + LK_SEC(1));
    if (status != MX_OK) {
        LTRACEF("set translation enable failed\n");
        return status;
    }

    return MX_OK;
}

status_t IommuImpl::EnableBiosReservedMappingsLocked() {
    auto desc = reinterpret_cast<const mx_iommu_desc_intel_t*>(desc_.get());

    size_t cursor_bytes = 0;
    while (cursor_bytes + sizeof(mx_iommu_desc_intel_reserved_memory_t) < desc->reserved_memory_bytes) {
        // The descriptor has already been validated, so no need to check again.
        auto mem = reinterpret_cast<mx_iommu_desc_intel_reserved_memory_t*>(
                reinterpret_cast<uintptr_t>(desc) + sizeof(*desc) + desc->scope_bytes +
                cursor_bytes);

        const size_t num_scopes = mem->scope_bytes / sizeof(mx_iommu_desc_intel_scope_t);
        auto scopes = reinterpret_cast<mx_iommu_desc_intel_scope_t*>(
                reinterpret_cast<uintptr_t>(mem) + sizeof(*mem));
        for (size_t i = 0; i < num_scopes; ++i) {
            if (scopes[i].num_hops != 1) {
                // TODO(teisenbe): Implement
                return MX_ERR_NOT_SUPPORTED;
            }

            DeviceContext* dev;
            status_t status = GetOrCreateDeviceContextLocked(scopes[i].start_bus,
                                                             scopes[i].dev_func[0], &dev);
            if (status != MX_OK) {
                return status;
            }

            LTRACEF("Enabling region [%lx, %lx) for %02x:%02x.%02x\n", mem->base_addr, mem->base_addr + mem->len, scopes[i].start_bus, (uint8_t)(scopes[i].dev_func[0] >> 3), scopes[i].dev_func[0] & 0x7u);
            dev_vaddr_t vaddr;
            const uint32_t perms = IOMMU_FLAG_PERM_READ | IOMMU_FLAG_PERM_WRITE;
            status = dev->SecondLevelMap(mem->base_addr, mem->len, perms, &vaddr);
            if (status != MX_OK) {
                return status;
            }
            ASSERT(mem->base_addr == vaddr);

            ASSERT(!caps_.required_write_buf_flushing());
            // TODO(teisenbe): Integrate finer-grained cache flushing inside of the page
            // table management
            __asm__ volatile("wbinvd" : : : "memory");
        }

        cursor_bytes += sizeof(*mem) + mem->scope_bytes;
    }

    return MX_OK;
}

// Sets the root table pointer and invalidates the context-cache and IOTLB.
status_t IommuImpl::SetRootTablePointerLocked(paddr_t pa) {
    DEBUG_ASSERT(IS_PAGE_ALIGNED(pa));

    auto root_table_addr = reg::RootTableAddress::Get().FromValue(0);
    // If we support extended contexts, use it.
    root_table_addr.set_root_table_type(supports_extended_context_);
    root_table_addr.set_root_table_address(pa >> PAGE_SIZE_SHIFT);
    root_table_addr.WriteTo(&mmio_);

    auto global_ctl = reg::GlobalControl::Get().ReadFrom(&mmio_);
    DEBUG_ASSERT(!global_ctl.translation_enable());
    global_ctl.set_root_table_ptr(1);
    global_ctl.WriteTo(&mmio_);
    status_t status = WaitForValueLocked(&global_ctl, &decltype(global_ctl)::root_table_ptr,
                                         1, current_time() + LK_SEC(1));
    if (status != MX_OK) {
        LTRACEF("Timed out waiting for root_table_ptr bit to take\n");
        return status;
    }

    status = InvalidateContextCacheGlobalLocked();
    if (status != MX_OK) {
        return status;
    }

    status = InvalidateIotlbGlobalLocked();
    if (status != MX_OK) {
        return status;
    }

    return MX_OK;
}

status_t IommuImpl::SetTranslationEnableLocked(bool enabled, lk_time_t deadline) {
    auto global_ctl = reg::GlobalControl::Get().ReadFrom(&mmio_);
    global_ctl.set_translation_enable(enabled);
    global_ctl.WriteTo(&mmio_);

    return WaitForValueLocked(&global_ctl, &decltype(global_ctl)::translation_enable,
                              enabled, deadline);
}

status_t IommuImpl::InvalidateContextCacheGlobalLocked() {
    DEBUG_ASSERT(lock_.IsHeld());

    auto context_cmd = reg::ContextCommand::Get().FromValue(0);
    context_cmd.set_invld_context_cache(1);
    // TODO: make this an enum
    context_cmd.set_invld_request_granularity(1);
    context_cmd.WriteTo(&mmio_);

    return WaitForValueLocked(&context_cmd, &decltype(context_cmd)::invld_context_cache, 0,
                              INFINITE_TIME);
}

status_t IommuImpl::InvalidateIotlbGlobalLocked() {
    DEBUG_ASSERT(lock_.IsHeld());

    auto iotlb_invld = reg::IotlbInvalidate::Get(iotlb_reg_offset_).ReadFrom(&mmio_);
    iotlb_invld.set_invld_iotlb(1);
    // TODO: make this an enum
    iotlb_invld.set_invld_request_granularity(1);
    iotlb_invld.WriteTo(&mmio_);

    return WaitForValueLocked(&iotlb_invld, &decltype(iotlb_invld)::invld_iotlb, 0,
                              INFINITE_TIME);
}

template <class RegType>
status_t IommuImpl::WaitForValueLocked(RegType* reg,
                                        typename RegType::ValueType (RegType::*getter)(),
                                        typename RegType::ValueType value,
                                        lk_time_t deadline) {
    DEBUG_ASSERT(lock_.IsHeld());

    const lk_time_t kMaxSleepDuration = LK_USEC(10);

    while (true) {
        reg->ReadFrom(&mmio_);
        if ((reg->*getter)() == value) {
            return MX_OK;
        }

        const lk_time_t now = current_time();
        if (now > deadline) {
            break;
        }

        lk_time_t sleep_deadline = mxtl::min(now + kMaxSleepDuration, deadline);
        thread_sleep(sleep_deadline);
    }
    return MX_ERR_TIMED_OUT;
}

enum handler_return IommuImpl::FaultHandler(void* ctx) {
    auto self = static_cast<IommuImpl*>(ctx);

    auto status = reg::FaultStatus::Get().ReadFrom(&self->mmio_);

    if (!status.primary_pending_fault()) {
        TRACEF("Non primary fault\n");
        return INT_NO_RESCHEDULE;
    }

    auto caps = reg::Capability::Get().ReadFrom(&self->mmio_);
    const uint32_t num_regs = static_cast<uint32_t>(caps.num_fault_recording_reg() + 1);
    const uint32_t reg_offset = static_cast<uint32_t>(caps.fault_recording_register_offset() * 16);

    uint32_t index = status.fault_record_index();
    while (1) {
        auto rec_high = reg::FaultRecordHigh::Get(reg_offset, index).ReadFrom(&self->mmio_);
        if (!rec_high.fault()) {
            break;
        }
        auto rec_low = reg::FaultRecordLow::Get(reg_offset, index).ReadFrom(&self->mmio_);
        uint64_t source = rec_high.source_id();
        TRACEF("IOMMU Fault: access %c, PASID (%c) %#04lx, reason %#02lx, source %02lx:%02lx.%lx, info: %lx\n",
               rec_high.request_type() ? 'R' : 'W',
               rec_high.pasid_present() ? 'V' : '-',
               rec_high.pasid_value(),
               rec_high.fault_reason(),
               source >> 8, (source >> 3) & 0x1f, source & 0x7,
               rec_low.fault_info() << 12);

        // Clear this fault (RW1CS)
        rec_high.WriteTo(&self->mmio_);

        ++index;
        if (index >= num_regs) {
            index -= num_regs;
        }
    }

    status.set_reg_value(0);
    // Clear the primary fault overflow condition (RW1CS)
    // TODO(teisenbe): How do we guarantee we get an interrupt on the next fault/if we left a fault unprocessed?
    status.set_primary_fault_overflow(1);
    status.WriteTo(&self->mmio_);

    return INT_NO_RESCHEDULE;
}

status_t IommuImpl::ConfigureFaultEventInterruptLocked() {
    DEBUG_ASSERT(lock_.IsHeld());

    auto& pcie_platform = PcieBusDriver::GetDriver()->platform();
    if (!pcie_platform.supports_msi()) {
        return MX_ERR_NOT_SUPPORTED;
    }
    status_t status = pcie_platform.AllocMsiBlock(1, false/* can_target_64bit */,
                                                  false /* msi x */, &irq_block_);
    if (status != MX_OK) {
        return status;
    }

    auto event_data = reg::FaultEventData::Get().FromValue(irq_block_.tgt_data);
    auto event_addr = reg::FaultEventAddress::Get().FromValue(
            static_cast<uint32_t>(irq_block_.tgt_addr));
    auto event_upper_addr = reg::FaultEventUpperAddress::Get().FromValue(
            static_cast<uint32_t>(irq_block_.tgt_addr >> 32));

    event_data.WriteTo(&mmio_);
    event_addr.WriteTo(&mmio_);
    event_upper_addr.WriteTo(&mmio_);

    // Clear all primary fault records
    for (uint32_t i = 0; i < num_fault_recording_reg_; ++i) {
        const uint32_t offset = fault_recording_reg_offset_;
        auto record_high = reg::FaultRecordHigh::Get(offset, i).ReadFrom(&mmio_);
        record_high.WriteTo(&mmio_);
    }

    // Clear all pending faults
    auto fault_status_ctl = reg::FaultStatus::Get().ReadFrom(&mmio_);
    fault_status_ctl.WriteTo(&mmio_);

    pcie_platform.RegisterMsiHandler(&irq_block_, 0, FaultHandler, this);

    // Unmask interrupts
    auto fault_event_ctl = reg::FaultEventControl::Get().ReadFrom(&mmio_);
    fault_event_ctl.set_interrupt_mask(0);
    fault_event_ctl.WriteTo(&mmio_);

    return MX_OK;
}

void IommuImpl::DisableFaultsLocked() {
    auto fault_event_ctl = reg::FaultEventControl::Get().ReadFrom(&mmio_);
    fault_event_ctl.set_interrupt_mask(1);
    fault_event_ctl.WriteTo(&mmio_);
}

status_t IommuImpl::GetOrCreateContextTableLocked(uint8_t bus, uint8_t dev_func,
                                                  ContextTableState** tbl) {
    DEBUG_ASSERT(lock_.IsHeld());

    volatile ds::RootTable* root_table = this->root_table();
    DEBUG_ASSERT(root_table);

    volatile ds::RootEntrySubentry* target_entry = &root_table->entry[bus].lower;
    if (supports_extended_context_ && dev_func >= 0x80) {
        // If this is an extended root table and the device is in the upper half
        // of the bus address space, use the upper pointer.
        target_entry = &root_table->entry[bus].upper;
    }

    ds::RootEntrySubentry entry;
    entry.ReadFrom(target_entry);
    if (entry.present()) {
        // We know the entry exists, so search our list of tables for it.
        for (ContextTableState& context_table : context_tables_) {
            if (context_table.includes_bdf(bus, dev_func)) {
                *tbl = &context_table;
                return MX_OK;
            }
        }
    }

    // Couldn't find the ContextTable, so create it.
    mxtl::unique_ptr<ContextTableState> table;
    status_t status = ContextTableState::Create(bus, supports_extended_context_,
                                                dev_func >= 0x80 /* upper */,
                                                this,
                                                target_entry,
                                                &table);
    if (status != MX_OK) {
        return status;
    }

    *tbl = table.get();
    context_tables_.push_back(mxtl::move(table));

    return MX_OK;
}

status_t IommuImpl::GetOrCreateDeviceContextLocked(uint8_t bus, uint8_t dev_func,
                                                   DeviceContext** context) {

    DEBUG_ASSERT(lock_.IsHeld());

    ContextTableState* ctx_table_state;
    status_t status = GetOrCreateContextTableLocked(bus, dev_func, &ctx_table_state);
    if (status != MX_OK) {
        return status;
    }

    status = ctx_table_state->GetDeviceContext(bus, dev_func, context);
    if (status != MX_ERR_NOT_FOUND) {
        // Either status was MX_OK and we're done, or some error occurred.
        return status;
    }
    return ctx_table_state->CreateDeviceContext(bus, dev_func, context);
}

} // namespace intel_iommu
