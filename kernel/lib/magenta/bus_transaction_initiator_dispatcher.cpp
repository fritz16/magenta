// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <magenta/bus_transaction_initiator_dispatcher.h>

#include <dev/iommu.h>
#include <err.h>
#include <kernel/auto_lock.h>
#include <kernel/vm/vm_object.h>
#include <magenta/rights.h>
#include <mxalloc/new.h>

status_t BusTransactionInitiatorDispatcher::Create(mxtl::RefPtr<Iommu> iommu, uint64_t bti_id,
                                                   mxtl::RefPtr<Dispatcher>* dispatcher,
                                                   mx_rights_t* rights) {

    if (!iommu->IsValidBusTxnId(bti_id)) {
        return MX_ERR_INVALID_ARGS;
    }

    AllocChecker ac;
    auto disp = new (&ac) BusTransactionInitiatorDispatcher(mxtl::move(iommu), bti_id);
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    *rights = MX_DEFAULT_BTI_RIGHTS;
    *dispatcher = mxtl::AdoptRef<Dispatcher>(disp);
    return MX_OK;
}

BusTransactionInitiatorDispatcher::BusTransactionInitiatorDispatcher(mxtl::RefPtr<Iommu> iommu,
                                                                     uint64_t bti_id)
        : iommu_(mxtl::move(iommu)), bti_id_(bti_id), state_tracker_(0u), zero_handles_(false) {}

BusTransactionInitiatorDispatcher::~BusTransactionInitiatorDispatcher() {
    DEBUG_ASSERT(pinned_memory_.is_empty());
}

status_t BusTransactionInitiatorDispatcher::Pin(mxtl::RefPtr<VmObject> vmo, uint64_t offset,
                                                uint64_t size, uint32_t perms,
                                                uint64_t* mapped_extents,
                                                size_t mapped_extents_len,
                                                size_t* actual_mapped_extents_len) {

    DEBUG_ASSERT(mapped_extents);
    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset));
    DEBUG_ASSERT(actual_mapped_extents_len);
    if (!IS_PAGE_ALIGNED(offset)) {
        return MX_ERR_INVALID_ARGS;
    }

    AutoLock guard(&lock_);

    if (zero_handles_) {
        return MX_ERR_BAD_STATE;
    }

    mxtl::unique_ptr<PinnedMemoryObject> pmo;
    status_t status = PinnedMemoryObject::Create(*this, mxtl::move(vmo),
                                                 offset, size, perms, &pmo);
    if (status != MX_OK) {
        return status;
    }

    const auto& pmo_addrs = pmo->mapped_extents();
    const size_t found_extents = pmo->mapped_extents_len();
    if (mapped_extents_len < found_extents)  {
        return MX_ERR_BUFFER_TOO_SMALL;
    }

    // Copy out addrs
    DEBUG_ASSERT(pmo->mapped_extents_len() <= ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE);
    for (size_t i = 0; i < found_extents; ++i) {
        mapped_extents[i] = pmo_addrs[i];
    }

    *actual_mapped_extents_len = found_extents;
    pinned_memory_.push_back(mxtl::move(pmo));
    return MX_OK;
}

status_t BusTransactionInitiatorDispatcher::Unpin(const uint64_t* mapped_extents,
                                                  size_t mapped_extents_len) {
    AutoLock guard(&lock_);

    if (zero_handles_) {
        return MX_ERR_BAD_STATE;
    }

    for (auto& pmo : pinned_memory_) {
        if (pmo.mapped_extents_len() != mapped_extents_len) {
            continue;
        }

        const auto& pmo_extents = pmo.mapped_extents();
        bool match = true;
        for (size_t i = 0; i < mapped_extents_len ; ++i) {
            if (mapped_extents[i] != pmo_extents[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            // The PMO dtor will take care of the actual unpinning.
            pinned_memory_.erase(pmo);
            return MX_OK;
        }
    }

    return MX_ERR_INVALID_ARGS;
}

void BusTransactionInitiatorDispatcher::on_zero_handles() {
    AutoLock guard(&lock_);
    while (!pinned_memory_.is_empty()) {
        pinned_memory_.pop_front();
    }
    zero_handles_ = true;
}
