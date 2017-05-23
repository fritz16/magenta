// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

namespace hwreg {

// Wrap MMIO for easier testing of device drivers

class RegisterIo {
public:
    RegisterIo(volatile void* mmio) : mmio_(reinterpret_cast<uintptr_t>(mmio)) {
    }

    template <class IntType> void Write(uint32_t offset, IntType val) {
        auto ptr = reinterpret_cast<volatile IntType*>(mmio_ + offset);
        *ptr = val;
    }

    template <class IntType> IntType Read(uint32_t offset)
    {
        auto ptr = reinterpret_cast<volatile IntType*>(mmio_ + offset);
        return *ptr;
    }

    uintptr_t base() const { return mmio_; }

private:
    const uintptr_t mmio_;
};

} // namespace hwreg
