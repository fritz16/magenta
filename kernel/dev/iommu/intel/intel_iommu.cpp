// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <dev/iommu/intel.h>
#include "iommu_impl.h"

status_t IntelIommu::Create(mxtl::unique_ptr<const uint8_t[]> desc, uint32_t desc_len,
                            mxtl::RefPtr<Iommu>* out) {
    return intel_iommu::IommuImpl::Create(mxtl::move(desc), desc_len, out);
}
