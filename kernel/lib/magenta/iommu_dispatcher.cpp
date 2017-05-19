// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <magenta/iommu_dispatcher.h>

#include <dev/iommu.h>
#include <magenta/rights.h>
#include <magenta/syscalls/iommu.h>
#include <mxalloc/new.h>

#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <trace.h>

#if WITH_DEV_IOMMU_DUMMY
#include <dev/iommu/dummy.h>
#endif // WITH_DEV_IOMMU_DUMMY
#if WITH_DEV_IOMMU_INTEL
#include <dev/iommu/intel.h>
#endif // WITH_DEV_IOMMU_INTEL

#define LOCAL_TRACE 0

mx_status_t IommuDispatcher::Create(uint32_t type, mxtl::unique_ptr<const uint8_t[]> desc,
                                    uint32_t desc_len, mxtl::RefPtr<Dispatcher>* dispatcher,
                                    mx_rights_t* rights) {

    mxtl::RefPtr<Iommu> iommu;
    mx_status_t status;
    switch (type) {
#if WITH_DEV_IOMMU_DUMMY
        case MX_IOMMU_TYPE_DUMMY:
            status = DummyIommu::Create(mxtl::move(desc), desc_len, &iommu);
            break;
#endif // WITH_DEV_IOMMU_DUMMY
#if WITH_DEV_IOMMU_INTEL
        case MX_IOMMU_TYPE_INTEL:
            status = IntelIommu::Create(mxtl::move(desc), desc_len, &iommu);
            break;
#endif // WITH_DEV_IOMMU_INTEL
        default:
            return MX_ERR_NOT_SUPPORTED;
    }
    if (status != MX_OK) {
        return status;
    }

    AllocChecker ac;
    auto disp = new (&ac) IommuDispatcher(mxtl::move(iommu));
    if (!ac.check())
        return MX_ERR_NO_MEMORY;

    *rights = MX_DEFAULT_IOMMU_RIGHTS;
    *dispatcher = mxtl::AdoptRef<Dispatcher>(disp);
    return MX_OK;
}

IommuDispatcher::IommuDispatcher(mxtl::RefPtr<Iommu> iommu)
    : iommu_(iommu) {}

IommuDispatcher::~IommuDispatcher() {
}

