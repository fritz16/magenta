// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT


#include <magenta/pinned_memory_object.h>

#include <assert.h>
#include <err.h>
#include <kernel/auto_lock.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_object.h>
#include <magenta/bus_transaction_initiator_dispatcher.h>
#include <mxalloc/new.h>
#include <mxtl/algorithm.h>
#include <mxtl/auto_call.h>
#include <trace.h>

#define LOCAL_TRACE 0

namespace {

struct IommuMapPageContext {
    mxtl::RefPtr<Iommu> iommu;
    uint64_t bus_txn_id;
    PinnedMemoryObject::Extent* page_array;
    size_t num_entries;
    uint32_t perms;
};

// Callback for VmObject::Lookup that handles mapping individual pages into the IOMMU.
status_t IommuMapPage(void* context, size_t offset, size_t index, paddr_t pa) {
    IommuMapPageContext* ctx = static_cast<IommuMapPageContext*>(context);

    dev_vaddr_t vaddr;
    status_t status = ctx->iommu->Map(ctx->bus_txn_id, pa, PAGE_SIZE, ctx->perms, &vaddr);
    if (status != MX_OK) {
        return status;
    }

    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    // TODO(teisenbe): Re-enable this to get run-encoding.
    /*
    if (ctx->num_entries == 0) {
        ctx->page_array[0] = PinnedMemoryObject::Extent(vaddr, 1);
        ctx->num_entries++;
        return MX_OK;
    }

    PinnedMemoryObject::Extent* prev_extent = &ctx->page_array[ctx->num_entries - 1];
    if (prev_extent->base() + prev_extent->pages() * PAGE_SIZE == vaddr &&
        prev_extent->extend(1) == MX_OK) {

        return MX_OK;
    }
    ctx->page_array[ctx->num_entries] = PinnedMemoryObject::Extent(vaddr, 1);
    ctx->num_entries++;
    */
    ctx->page_array[ctx->num_entries] = PinnedMemoryObject::Extent(vaddr, 1);
    ctx->num_entries++;
    return MX_OK;
}

} // namespace {}

status_t PinnedMemoryObject::Create(const BusTransactionInitiatorDispatcher& bti,
                                    mxtl::RefPtr<VmObject> vmo, size_t offset,
                                    size_t size, uint32_t perms,
                                    mxtl::unique_ptr<PinnedMemoryObject>* out) {
    LTRACE_ENTRY;
    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset));

    // Pin the memory to make sure it doesn't change from underneath us for the
    // lifetime of the created PMO.
    status_t status = vmo->Pin(offset, size);
    if (status != MX_OK) {
        LTRACEF("vmo->Pin failed: %d\n", status);
        return status;
    }

    uint64_t expected_addr = 0;
    auto check_contiguous = [](void* context, size_t offset, size_t index, paddr_t pa) {
        auto expected_addr = static_cast<uint64_t*>(context);
        if (index != 0 && pa != *expected_addr) {
            return MX_ERR_NOT_FOUND;
        }
        *expected_addr = pa + PAGE_SIZE;
        return MX_OK;
    };
    status = vmo->Lookup(offset, size, 0, check_contiguous, &expected_addr);
    bool is_contiguous = (status == MX_OK);

    // Set up a cleanup function to undo the pin if we need to fail this
    // operation.
    auto unpin_vmo = mxtl::MakeAutoCall([vmo, offset, size]() {
        vmo->Unpin(offset, size);
    });

    // TODO(teisenbe): Be more intelligent about allocating this, since if this
    // is backed by a real IOMMU, we will likely compress the page array greatly
    // using extents.
    AllocChecker ac;
    const size_t num_pages = is_contiguous ? 1 : ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
    mxtl::unique_ptr<Extent[]> page_array(new (&ac) Extent[num_pages]);
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    mxtl::unique_ptr<PinnedMemoryObject> pmo(
            new (&ac) PinnedMemoryObject(bti, mxtl::move(vmo), offset, size, is_contiguous,
                                         mxtl::move(page_array)));
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    // Now that the pmo object has been created, it is responsible for
    // unpinning.
    unpin_vmo.cancel();

    status = pmo->MapIntoIommu(perms);
    if (status != MX_OK) {
        LTRACEF("MapIntoIommu failed: %d\n", status);
        return status;
    }

    *out = mxtl::move(pmo);
    return MX_OK;
}

// Used during initialization to set up the IOMMU state for this PMO.
status_t PinnedMemoryObject::MapIntoIommu(uint32_t perms) {
    if (is_contiguous_) {
        paddr_t paddr = 1;
        auto get_paddr = [](void* context, size_t offset, size_t index, paddr_t pa) {
            *static_cast<paddr_t*>(context) = pa;
            return MX_OK;
        };
        const size_t lookup_size = mxtl::min<size_t>(size_, PAGE_SIZE);
        status_t status = vmo_->Lookup(offset_, lookup_size, 0, get_paddr, &paddr);
        if (status != MX_OK) {
            return status;
        }
        ASSERT(paddr != 1);

        dev_vaddr_t vaddr;
        status = bti_.iommu()->Map(bti_.bti_id(), paddr, ROUNDUP(size_, PAGE_SIZE), perms, &vaddr);
        if (status != MX_OK) {
            return status;
        }
        mapped_extents_[0] = PinnedMemoryObject::Extent(vaddr, 1);
        mapped_extents_len_ = 1;
        return MX_OK;
    }

    IommuMapPageContext context = {
        .iommu = bti_.iommu(),
        .bus_txn_id = bti_.bti_id(),
        .page_array = mapped_extents_.get(),
        .num_entries = 0,
        .perms = perms,
    };
    status_t status = vmo_->Lookup(offset_, size_, 0, IommuMapPage, static_cast<void*>(&context));
    mapped_extents_len_ = context.num_entries;
    if (status != MX_OK) {
        status_t err = UnmapFromIommu();
        ASSERT(err == MX_OK);
        return status;
    }

    return MX_OK;
}

status_t PinnedMemoryObject::UnmapFromIommu() {
    auto iommu = bti_.iommu();
    const uint64_t bus_txn_id = bti_.bti_id();

    if (unlikely(mapped_extents_len_ == 0)) {
        return MX_OK;
    }

    status_t status = MX_OK;
    if (is_contiguous_) {
        status = iommu->Unmap(bus_txn_id, mapped_extents_[0].base(), ROUNDUP(size_, PAGE_SIZE));
    } else {
        for (size_t i = 0; i < mapped_extents_len_; ++i) {
            // Try to unmap all pages even if we get an error, and return the
            // first error encountered.
            status_t err = iommu->Unmap(bus_txn_id, mapped_extents_[i].base(),
                                        mapped_extents_[i].pages() * PAGE_SIZE);
            DEBUG_ASSERT(err == MX_OK);
            if (err != MX_OK && status == MX_OK) {
                status = err;
            }
        }
    }
    // Clear this so we won't try again if this gets called again in the
    // destructor.
    mapped_extents_len_ = 0;
    return status;
}

PinnedMemoryObject::~PinnedMemoryObject() {
    status_t status = UnmapFromIommu();
    ASSERT(status == MX_OK);
    vmo_->Unpin(offset_, size_);
}

PinnedMemoryObject::PinnedMemoryObject(const BusTransactionInitiatorDispatcher& bti,
                                       mxtl::RefPtr<VmObject> vmo, size_t offset, size_t size,
                                       bool is_contiguous,
                                       mxtl::unique_ptr<Extent[]> mapped_extents)
    : vmo_(mxtl::move(vmo)), offset_(offset), size_(size), is_contiguous_(is_contiguous), bti_(bti),
      mapped_extents_(mxtl::move(mapped_extents)), mapped_extents_len_(0) {
}
