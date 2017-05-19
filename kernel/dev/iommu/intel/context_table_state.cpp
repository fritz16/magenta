// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "context_table_state.h"

#include <mxalloc/new.h>
#include <mxtl/unique_ptr.h>

#include "device_context.h"
#include "hw.h"
#include "iommu_impl.h"

namespace intel_iommu {

ContextTableState::ContextTableState(uint8_t bus, bool extended, bool upper,
                                     IommuImpl* parent, volatile ds::RootEntrySubentry* root_entry,
                                     IommuPage page)
        : parent_(parent), root_entry_(root_entry), page_(mxtl::move(page)),
          bus_(bus), extended_(extended), upper_(upper) {
}

ContextTableState::~ContextTableState() {
    ds::RootEntrySubentry entry;
    entry.ReadFrom(root_entry_);
    entry.set_present(0);
    entry.WriteTo(root_entry_);
    // TODO(teisenbe): Perform a context cache flush
}

status_t ContextTableState::Create(uint8_t bus, bool extended, bool upper,
                                   IommuImpl* parent, volatile ds::RootEntrySubentry* root_entry,
                                   mxtl::unique_ptr<ContextTableState>* table) {
    ds::RootEntrySubentry entry;
    entry.ReadFrom(root_entry);
    DEBUG_ASSERT(!entry.present());

    IommuPage page;
    status_t status = IommuPage::AllocatePage(&page);
    if (status != MX_OK) {
        return status;
    }

    AllocChecker ac;
    mxtl::unique_ptr<ContextTableState> tbl(new (&ac) ContextTableState(bus, extended, upper,
                                                                        parent, root_entry,
                                                                        mxtl::move(page)));
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    entry.set_present(1);
    entry.set_context_table(tbl->page_.paddr() >> 12);
    entry.WriteTo(root_entry);

    *table = mxtl::move(tbl);
    return MX_OK;
}

status_t ContextTableState::CreateDeviceContext(uint8_t bus, uint8_t dev_func,
                                                DeviceContext** context) {
    mxtl::unique_ptr<DeviceContext> dev;
    status_t status;
    if (extended_) {
        volatile ds::ExtendedContextTable* tbl = extended_table();
        volatile ds::ExtendedContextEntry* entry = &tbl->entry[dev_func & 0x7f];
        status = DeviceContext::Create(bus, dev_func, parent_, entry, &dev);
    } else {
        volatile ds::ContextTable* tbl = table();
        volatile ds::ContextEntry* entry = &tbl->entry[dev_func];
        status = DeviceContext::Create(bus, dev_func, parent_, entry, &dev);
    }
    if (status != MX_OK) {
        return status;
    }

    *context = dev.get();
    devices_.push_back(mxtl::move(dev));
    return MX_OK;
}

status_t ContextTableState::GetDeviceContext(uint8_t bus, uint8_t dev_func, DeviceContext** context) {
    for (auto& dev : devices_) {
        if (dev.is_bdf(bus, dev_func)) {
            *context = &dev;
            return MX_OK;
        }
    }
    return MX_ERR_NOT_FOUND;
}

} // namespace intel_iommu
