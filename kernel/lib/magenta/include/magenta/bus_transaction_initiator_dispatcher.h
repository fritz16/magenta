// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <dev/iommu.h>
#include <kernel/mutex.h>
#include <magenta/dispatcher.h>
#include <magenta/pinned_memory_object.h>
#include <magenta/state_tracker.h>
#include <mxtl/canary.h>

#include <sys/types.h>

class Iommu;

class BusTransactionInitiatorDispatcher final : public Dispatcher {
public:
    static status_t Create(mxtl::RefPtr<Iommu> iommu, uint64_t bti_id,
                           mxtl::RefPtr<Dispatcher>* dispatcher, mx_rights_t* rights);

    ~BusTransactionInitiatorDispatcher() final;
    mx_obj_type_t get_type() const final { return MX_OBJ_TYPE_BTI; }
    StateTracker* get_state_tracker() final { return &state_tracker_; }

    // Pins the given VMO range and writes the addresses into |mapped_extents|.  The
    // number of addresses returned is given in |actual_mapped_extents_len|.
    //
    // Returns MX_ERR_INVALID_ARGS if |offset| or |size| are not PAGE_SIZE aligned.
    // Returns MX_ERR_INVALID_ARGS if |perms| is not suitable to pass to the Iommu::Map() interface.
    // Returns MX_ERR_BUFFER_TOO_SMALL if |mapped_extents_len| is not at least |size|/PAGE_SIZE.
    status_t Pin(mxtl::RefPtr<VmObject> vmo, uint64_t offset, uint64_t size, uint32_t perms,
                 uint64_t* mapped_extents, size_t mapped_extents_len,
                 size_t* actual_mapped_extents_len);

    // Unpins the given list of extents.  Returns an error if the described
    // list of extents do not correspond to the exact set created in a
    // previous call to Pin().
    status_t Unpin(const uint64_t* mapped_extents, size_t mapped_extents_len);

    void on_zero_handles() final;

    mxtl::RefPtr<Iommu> iommu() const { return iommu_; }
    uint64_t bti_id() const { return bti_id_; }

private:
    BusTransactionInitiatorDispatcher(mxtl::RefPtr<Iommu> iommu, uint64_t bti_id);

    mxtl::Canary<mxtl::magic("BTID")> canary_;

    Mutex lock_;
    const mxtl::RefPtr<Iommu> iommu_;
    const uint64_t bti_id_;

    StateTracker state_tracker_;

    mxtl::DoublyLinkedList<mxtl::unique_ptr<PinnedMemoryObject>> pinned_memory_ TA_GUARDED(lock_);
    bool zero_handles_ TA_GUARDED(lock_);
};
