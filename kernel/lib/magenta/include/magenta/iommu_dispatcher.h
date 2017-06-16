// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <dev/iommu.h>
#include <magenta/dispatcher.h>
#include <magenta/state_tracker.h>
#include <mxtl/canary.h>

#include <sys/types.h>

class IommuDispatcher final : public Dispatcher {
public:
    static mx_status_t Create(uint32_t type, mxtl::unique_ptr<const void> desc,
                              uint32_t desc_len, mxtl::RefPtr<Dispatcher>* dispatcher,
                              mx_rights_t* rights) {

    ~IommuDispatcher() final;
    mx_obj_type_t get_type() const final { return MX_OBJ_TYPE_IOMMU; }

    mxtl::RefPtr<Iommu> iommu() const { return iommu_; }

private:
    explicit IommuDispatcher(mxtl::RefPtr<Iommu> iommu);

    mxtl::Canary<mxtl::magic("IOMD")> canary_;
    const mxtl::RefPtr<Iommu> iommu_;
};
