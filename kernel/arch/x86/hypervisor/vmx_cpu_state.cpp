// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <bits.h>
#include <string.h>

#include <kernel/auto_lock.h>
#include <kernel/mp.h>
#include <kernel/vm/pmm.h>

#include "vmx_cpu_state_priv.h"

static mxtl::Mutex vmx_mutex;
static size_t vcpus TA_GUARDED(vmx_mutex) = 0;
static mxtl::unique_ptr<VmxCpuState> vmx_cpu_state TA_GUARDED(vmx_mutex);

static status_t vmxon(paddr_t pa) {
    uint8_t err;

    __asm__ volatile (
        "vmxon %[pa];"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err)
        : [pa] "m"(pa)
        : "cc", "memory");

    return err ? MX_ERR_INTERNAL : MX_OK;
}

static status_t vmxoff() {
    uint8_t err;

    __asm__ volatile (
        "vmxoff;"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err)
        :
        : "cc");

    return err ? MX_ERR_INTERNAL : MX_OK;
}

VmxInfo::VmxInfo() {
    // From Volume 3, Appendix A.1.
    uint64_t basic_info = read_msr(X86_MSR_IA32_VMX_BASIC);
    revision_id = static_cast<uint32_t>(BITS(basic_info, 30, 0));
    region_size = static_cast<uint16_t>(BITS_SHIFT(basic_info, 44, 32));
    write_back = BITS_SHIFT(basic_info, 53, 50) == VMX_MEMORY_TYPE_WRITE_BACK;
    io_exit_info = BIT_SHIFT(basic_info, 54);
    vmx_controls = BIT_SHIFT(basic_info, 55);
}

MiscInfo::MiscInfo() {
    // From Volume 3, Appendix A.6.
    uint64_t misc_info = read_msr(X86_MSR_IA32_VMX_MISC);
    wait_for_sipi = BIT_SHIFT(misc_info, 8);
    msr_list_limit = static_cast<uint32_t>(BITS_SHIFT(misc_info, 27, 25) + 1) * 512;
}

EptInfo::EptInfo() {
    // From Volume 3, Appendix A.10.
    uint64_t ept_info = read_msr(X86_MSR_IA32_VMX_EPT_VPID_CAP);
    page_walk_4 = BIT_SHIFT(ept_info, 6);
    write_back = BIT_SHIFT(ept_info, 14);
    pde_2mb_page = BIT_SHIFT(ept_info, 16);
    pdpe_1gb_page = BIT_SHIFT(ept_info, 17);
    ept_flags = BIT_SHIFT(ept_info, 21);
    exit_info = BIT_SHIFT(ept_info, 22);
    invept =
        // INVEPT instruction is supported.
        BIT_SHIFT(ept_info, 20) &&
        // Single-context INVEPT type is supported.
        BIT_SHIFT(ept_info, 25) &&
        // All-context INVEPT type is supported.
        BIT_SHIFT(ept_info, 26);
}

status_t VmxPage::Alloc(const VmxInfo& vmx_info, uint8_t fill) {
    // From Volume 3, Appendix A.1: Bits 44:32 report the number of bytes that
    // software should allocate for the VMXON region and any VMCS region. It is
    // a value greater than 0 and at most 4096 (bit 44 is set if and only if
    // bits 43:32 are clear).
    if (vmx_info.region_size > PAGE_SIZE)
        return MX_ERR_NOT_SUPPORTED;

    // Check use of write-back memory for VMX regions is supported.
    if (!vmx_info.write_back)
        return MX_ERR_NOT_SUPPORTED;

    // The maximum size for a VMXON or VMCS region is 4096, therefore
    // unconditionally allocating a page is adequate.
    if (pmm_alloc_page(0, &pa_) == nullptr)
        return MX_ERR_NO_MEMORY;

    memset(VirtualAddress(), fill, PAGE_SIZE);
    return MX_OK;
}

paddr_t VmxPage::PhysicalAddress() const {
    DEBUG_ASSERT(pa_ != 0);
    return pa_;
}

void* VmxPage::VirtualAddress() const {
    DEBUG_ASSERT(pa_ != 0);
    return paddr_to_kvaddr(pa_);
}

VmxPage::~VmxPage() {
    vm_page_t* page = paddr_to_vm_page(pa_);
    if (page != nullptr)
        pmm_free_page(page);
}

struct vmxon_context {
    mxtl::Array<VmxPage>* vmxon_pages;
    mxtl::atomic<mp_cpu_mask_t> cpu_mask;

    vmxon_context(mxtl::Array<VmxPage>* vp) : vmxon_pages(vp), cpu_mask(0) {}
};

static void vmxon_task(void* arg) {
    auto ctx = static_cast<vmxon_context*>(arg);
    uint cpu_num = arch_curr_cpu_num();
    VmxPage& page = (*ctx->vmxon_pages)[cpu_num];

    // Check that we have instruction information when we VM exit on IO.
    VmxInfo vmx_info;
    if (!vmx_info.io_exit_info)
        return;

    // Check that full VMX controls are supported.
    if (!vmx_info.vmx_controls)
        return;

    // Check that a page-walk length of 4 is supported.
    EptInfo ept_info;
    if (!ept_info.page_walk_4)
        return;

    // Check use write-back memory for EPT is supported.
    if (!ept_info.write_back)
        return;

    // Check that accessed and dirty flags for EPT are supported.
    if (!ept_info.ept_flags)
        return;

    // Check that the INVEPT instruction is supported.
    if (!ept_info.invept)
        return;

    // Check that wait for startup IPI is a supported activity state.
    MiscInfo misc_info;
    if (!misc_info.wait_for_sipi)
        return;

    // Enable VMXON, if required.
    uint64_t feature_control = read_msr(X86_MSR_IA32_FEATURE_CONTROL);
    if (!(feature_control & X86_MSR_IA32_FEATURE_CONTROL_LOCK) ||
        !(feature_control & X86_MSR_IA32_FEATURE_CONTROL_VMXON)) {
        if ((feature_control & X86_MSR_IA32_FEATURE_CONTROL_LOCK) &&
            !(feature_control & X86_MSR_IA32_FEATURE_CONTROL_VMXON)) {
            return;
        }
        feature_control |= X86_MSR_IA32_FEATURE_CONTROL_LOCK;
        feature_control |= X86_MSR_IA32_FEATURE_CONTROL_VMXON;
        write_msr(X86_MSR_IA32_FEATURE_CONTROL, feature_control);
    }

    // Check control registers are in a VMX-friendly state.
    uint64_t cr0 = x86_get_cr0();
    if (cr_is_invalid(cr0, X86_MSR_IA32_VMX_CR0_FIXED0, X86_MSR_IA32_VMX_CR0_FIXED1))
        return;
    uint64_t cr4 = x86_get_cr4() | X86_CR4_VMXE;
    if (cr_is_invalid(cr4, X86_MSR_IA32_VMX_CR4_FIXED0, X86_MSR_IA32_VMX_CR4_FIXED1))
        return;

    // Enable VMX using the VMXE bit.
    x86_set_cr4(cr4);

    // Setup VMXON page.
    VmxRegion* region = page.VirtualAddress<VmxRegion>();
    region->revision_id = vmx_info.revision_id;

    // Execute VMXON.
    status_t status = vmxon(page.PhysicalAddress());
    if (status != MX_OK) {
        dprintf(CRITICAL, "Failed to turn on VMX on CPU %u\n", cpu_num);
        return;
    }

    ctx->cpu_mask.fetch_or(1 << cpu_num);
}

static void vmxoff_task(void* arg) {
    // Execute VMXOFF.
    status_t status = vmxoff();
    if (status != MX_OK) {
        dprintf(CRITICAL, "Failed to turn off VMX on CPU %u\n", arch_curr_cpu_num());
        return;
    }

    // Disable VMX.
    x86_set_cr4(x86_get_cr4() & ~X86_CR4_VMXE);
}

// static
status_t VmxCpuState::Create(mxtl::unique_ptr<VmxCpuState>* out) {
    // Allocate a VMXON page for each CPU.
    size_t num_cpus = arch_max_num_cpus();
    AllocChecker ac;
    VmxPage* pages = new (&ac) VmxPage[num_cpus];
    if (!ac.check())
        return MX_ERR_NO_MEMORY;
    mxtl::Array<VmxPage> vmxon_pages(pages, num_cpus);
    VmxInfo vmx_info;
    for (auto& page : vmxon_pages) {
        status_t status = page.Alloc(vmx_info, 0);
        if (status != MX_OK)
            return status;
    }

    // Enable VMX for all online CPUs.
    vmxon_context vmxon_ctx(&vmxon_pages);
    mp_cpu_mask_t online_mask = mp_get_online_mask();
    mp_sync_exec(online_mask, vmxon_task, &vmxon_ctx);
    mp_cpu_mask_t cpu_mask = vmxon_ctx.cpu_mask.load();
    if (cpu_mask != online_mask) {
        mp_sync_exec(cpu_mask, vmxoff_task, nullptr);
        return MX_ERR_NOT_SUPPORTED;
    }

    mxtl::unique_ptr<VmxCpuState> vmx_cpu_state(new (&ac) VmxCpuState(mxtl::move(vmxon_pages)));
    if (!ac.check())
        return MX_ERR_NO_MEMORY;
    status_t status = vmx_cpu_state->vpid_bitmap_.Reset(kNumVpids);
    if (status != MX_OK)
        return status;

    *out = mxtl::move(vmx_cpu_state);
    return MX_OK;
}

VmxCpuState::VmxCpuState(mxtl::Array<VmxPage> vmxon_pages)
    : vmxon_pages_(mxtl::move(vmxon_pages)) {}

VmxCpuState::~VmxCpuState() {
    mp_sync_exec(MP_CPU_ALL, vmxoff_task, nullptr);
}

status_t VmxCpuState::AllocVpid(uint16_t* vpid) {
    size_t first_unset;
    bool all_set = vpid_bitmap_.Get(0, kNumVpids, &first_unset);
    if (all_set)
        return MX_ERR_NO_RESOURCES;
    if (first_unset > UINT16_MAX)
        return MX_ERR_OUT_OF_RANGE;
    *vpid = (first_unset + 1) & UINT16_MAX;
    return vpid_bitmap_.SetOne(first_unset);
}

status_t VmxCpuState::ReleaseVpid(uint16_t vpid) {
    if (vpid == 0 || !vpid_bitmap_.GetOne(vpid - 1))
        return MX_ERR_INVALID_ARGS;
    return vpid_bitmap_.ClearOne(vpid - 1);
}

status_t alloc_vpid(uint16_t* vpid) {
    AutoLock lock(&vmx_mutex);
    if (vcpus == 0) {
        status_t status = VmxCpuState::Create(&vmx_cpu_state);
        if (status != MX_OK)
            return status;
    }
    vcpus++;
    return vmx_cpu_state->AllocVpid(vpid);
}

status_t release_vpid(uint16_t vpid) {
    AutoLock lock(&vmx_mutex);
    status_t status = vmx_cpu_state->ReleaseVpid(vpid);
    if (status != MX_OK)
        return status;
    vcpus--;
    if (vcpus == 0)
        vmx_cpu_state.reset();
    return MX_OK;
}

bool cr_is_invalid(uint64_t cr_value, uint32_t fixed0_msr, uint32_t fixed1_msr) {
    uint64_t fixed0 = read_msr(fixed0_msr);
    uint64_t fixed1 = read_msr(fixed1_msr);
    return ~(cr_value | ~fixed0) != 0 || ~(~cr_value | fixed1) != 0;
}
