// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <assert.h>
#include <magenta/compiler.h>
#include <stdint.h>

__BEGIN_CDECLS

#define MX_IOMMU_MAX_DESC_LEN 4096

#define MX_IOMMU_TYPE_DUMMY 0

typedef struct mx_iommu_desc_dummy {
    uint8_t reserved;
} mx_iommu_desc_dummy_t;

__END_CDECLS
