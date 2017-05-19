// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdbool.h>
#include <magenta/syscalls/iommu.h>
#include <stdio.h>

#include "init.h"

#define ACPI_MAX_INIT_TABLES 32

static mx_status_t find_iommus(void);

/* @brief Switch interrupts to APIC model (controls IRQ routing) */
static ACPI_STATUS set_apic_irq_mode(void) {
    ACPI_OBJECT selector = {
        .Integer.Type = ACPI_TYPE_INTEGER,
        .Integer.Value = 1, // 1 means APIC mode according to ACPI v5 5.8.1
    };
    ACPI_OBJECT_LIST params = {
        .Count = 1,
        .Pointer = &selector,
    };
    return AcpiEvaluateObject(NULL, (char*)"\\_PIC", &params, NULL);
}

ACPI_STATUS init(void) {
    // This sequence is described in section 10.1.2.1 (Full ACPICA Initialization)
    // of the ACPICA developer's reference.
    ACPI_STATUS status = AcpiInitializeSubsystem();
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI\n");
        return status;
    }

    status = AcpiInitializeTables(NULL, ACPI_MAX_INIT_TABLES, FALSE);
    if (status == AE_NOT_FOUND) {
        printf("WARNING: could not find ACPI tables\n");
        return status;
    } else if (status == AE_NO_MEMORY) {
        printf("WARNING: could not initialize ACPI tables\n");
        return status;
    } else if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI tables for unknown reason\n");
        return status;
    }

    status = AcpiLoadTables();
    if (status != AE_OK) {
        printf("WARNING: could not load ACPI tables: %d\n", status);
        return status;
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not enable ACPI\n");
        return status;
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI objects\n");
        return status;
    }

    mx_status_t mx_status = find_iommus();
    if (mx_status != MX_OK) {
        printf("Failed to publish iommus\n");
    }

    status = set_apic_irq_mode();
    if (status == AE_NOT_FOUND) {
        printf("WARNING: Could not find ACPI IRQ mode switch\n");
    } else if (status != AE_OK) {
        printf("Failed to set APIC IRQ mode\n");
        return status;
    }

    // TODO(teisenbe): Maybe back out of ACPI mode on failure, but we rely on
    // ACPI for some critical things right now, so failure will likely prevent
    // successful boot anyway.
    return AE_OK;
}

static mx_status_t acpi_scope_to_desc(ACPI_DMAR_DEVICE_SCOPE* acpi_scope,
                                      mx_iommu_desc_intel_scope_t* desc_scope) {
    switch (acpi_scope->EntryType) {
        case ACPI_DMAR_SCOPE_TYPE_ENDPOINT:
            desc_scope->type = MX_IOMMU_INTEL_SCOPE_ENDPOINT;
            break;
        case ACPI_DMAR_SCOPE_TYPE_BRIDGE:
            desc_scope->type = MX_IOMMU_INTEL_SCOPE_BRIDGE;
            break;
        default:
            // Skip this scope, since it's not a type we care about.
            return MX_ERR_WRONG_TYPE;
    }
    desc_scope->start_bus = acpi_scope->Bus;
    // TODO(teisenbe): Check for overflow
    desc_scope->num_hops = (acpi_scope->Length - 6) / 2;
    if (countof(desc_scope->dev_func) < desc_scope->num_hops) {
        return MX_ERR_NOT_SUPPORTED;
    }
    for (ssize_t i = 0; i < desc_scope->num_hops; ++i) {
        uint16_t v = *(uint16_t*)((uintptr_t)acpi_scope + 6 + 2 * i);
        const uint8_t dev = v & 0x1f;
        const uint8_t func = (v >> 8) & 0x7;
        desc_scope->dev_func[i] = (dev << 3) | func;
    }
    return MX_OK;
}

// Walks the given unit's scopes and appends them to the given descriptor.
// |max_scopes| is the number of scopes |scopes| can hold.  |num_scopes_found|
// is the number of scopes found on |unit|, even if they wouldn't all fit in |scopes|.
static mx_status_t append_scopes(ACPI_DMAR_HARDWARE_UNIT* unit,
                                 size_t max_scopes,
                                 mx_iommu_desc_intel_scope_t* scopes,
                                 size_t* num_scopes_found) {
    size_t num_scopes = 0;
    uintptr_t scope;
    const uintptr_t addr = (uintptr_t)unit;
    for (scope = addr + 16; scope < addr + unit->Header.Length; ) {
        ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;
        printf("  DMAR Scope: %u, bus %u\n", s->EntryType, s->Bus);
        for (ssize_t i = 0; i < (s->Length - 6) / 2; ++i) {
            uint16_t v = *(uint16_t*)(scope + 6 + 2 * i);
            printf("    Path %ld: %02x.%02x\n", i, v & 0xffu, (uint16_t)(v >> 8));
        }
        scope += s->Length;

        // Count the scopes we care about
        switch (s->EntryType) {
            case ACPI_DMAR_SCOPE_TYPE_ENDPOINT:
            case ACPI_DMAR_SCOPE_TYPE_BRIDGE:
                num_scopes++;
                break;
        }
    }

    if (num_scopes_found) {
        *num_scopes_found = num_scopes;
    }
    if (!scopes) {
        return MX_OK;
    }

    if (num_scopes > max_scopes) {
        return MX_ERR_BUFFER_TOO_SMALL;
    }

    size_t cur_num_scopes = 0;
    for (scope = addr + 16; scope < addr + unit->Header.Length && cur_num_scopes < max_scopes;) {
        ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;

        mx_status_t status = acpi_scope_to_desc(s, &scopes[cur_num_scopes]);
        if (status != MX_OK && status != MX_ERR_WRONG_TYPE) {
            return status;
        }
        if (status == MX_OK) {
            cur_num_scopes++;
        }

        scope += s->Length;
    }
    assert(cur_num_scopes == num_scopes);
    return MX_OK;
}

static bool scope_eq(mx_iommu_desc_intel_scope_t* scope,
                     ACPI_DMAR_DEVICE_SCOPE* acpi_scope) {

    mx_iommu_desc_intel_scope_t other_scope;
    mx_status_t status = acpi_scope_to_desc(acpi_scope, &other_scope);
    if (status != MX_OK) {
        return false;
    }

    if (scope->type != other_scope.type || scope->start_bus != other_scope.start_bus ||
        scope->num_hops != other_scope.num_hops) {

        return false;
    }

    for (size_t i = 0; i < scope->num_hops; ++i) {
        if (scope->dev_func[i] != other_scope.dev_func[i]) {
            return false;
        }
    }

    return true;
}

// Appends to desc any reserved memory regions that match its scopes.  If
// |desc_len| is not large enough to include the reserved memory descriptors, this
// function will not append all of the found entries.  |bytes_needed| will
// always return the number of bytes needed to represent all of the reserved
// descriptors.  This function does not modify desc->reserved_mem_bytes.
static mx_status_t append_reserved_mem(ACPI_TABLE_DMAR* table,
                                       mx_iommu_desc_intel_t* desc,
                                       size_t desc_len,
                                       size_t* bytes_needed) {

    const uintptr_t records_start = (uintptr_t)table + sizeof(*table);
    const uintptr_t records_end = (uintptr_t)table + table->Header.Length;

    mx_iommu_desc_intel_scope_t* desc_scopes = (mx_iommu_desc_intel_scope_t*)(
            (uintptr_t)desc + sizeof(*desc));
    const size_t num_desc_scopes = desc->scope_bytes / sizeof(mx_iommu_desc_intel_scope_t);

    uintptr_t next_reserved_mem_desc_base = (uintptr_t)desc + sizeof(mx_iommu_desc_intel_t) +
            desc->scope_bytes + desc->reserved_memory_bytes;

    *bytes_needed = 0;
    for (uintptr_t addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_RESERVED_MEMORY: {
                ACPI_DMAR_RESERVED_MEMORY* rec = (ACPI_DMAR_RESERVED_MEMORY*)record_hdr;

                if (desc->pci_segment != rec->Segment) {
                    break;
                }

                mx_iommu_desc_intel_reserved_memory_t* mem_desc =
                        (mx_iommu_desc_intel_reserved_memory_t*)next_reserved_mem_desc_base;
                size_t mem_desc_size = sizeof(*mem_desc);

                // Search for scopes that match
                for (uintptr_t scope = addr + 24; scope < addr + rec->Header.Length; ) {
                    ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;
                    // TODO(teisenbe): We should skip scope types we don't
                    // care about here

                    // Search for a scope in the descriptor that matches this
                    // ACPI scope.
                    bool no_matches = true;
                    for (size_t i = 0; i < num_desc_scopes; ++i) {
                        mx_iommu_desc_intel_scope_t* scope_desc = &desc_scopes[i];
                        const bool scope_matches = scope_eq(scope_desc, s);

                        no_matches &= !scope_matches;

                        // If this is a whole segment descriptor, then a match
                        // corresponds to an entry we should ignore.
                        if (scope_matches && !desc->whole_segment) {
                            mx_iommu_desc_intel_scope_t* new_scope_desc =
                                    (mx_iommu_desc_intel_scope_t*)(next_reserved_mem_desc_base +
                                                                   mem_desc_size);
                            mem_desc_size += sizeof(mx_iommu_desc_intel_scope_t);

                            if (next_reserved_mem_desc_base + mem_desc_size <=
                                (uintptr_t)desc + desc_len) {

                                memcpy(new_scope_desc, scope_desc, sizeof(*scope_desc));
                            }
                            break;
                        }
                    }

                    if (no_matches && desc->whole_segment) {
                        mx_iommu_desc_intel_scope_t other_scope;
                        mx_status_t status = acpi_scope_to_desc(s, &other_scope);
                        if (status != MX_ERR_WRONG_TYPE && status != MX_OK) {
                            return status;
                        }
                        if (status == MX_OK) {
                            mx_iommu_desc_intel_scope_t* new_scope_desc =
                                    (mx_iommu_desc_intel_scope_t*)(next_reserved_mem_desc_base +
                                                                   mem_desc_size);
                            mem_desc_size += sizeof(mx_iommu_desc_intel_scope_t);

                            if (next_reserved_mem_desc_base + mem_desc_size <=
                                (uintptr_t)desc + desc_len) {

                                memcpy(new_scope_desc, &other_scope, sizeof(other_scope));
                            }
                        }
                    }

                    scope += s->Length;
                }

                // If this descriptor does not have any scopes, ignore it
                if (mem_desc_size == sizeof(*mem_desc)) {
                    break;
                }

                if (next_reserved_mem_desc_base + mem_desc_size <= (uintptr_t)desc + desc_len) {
                    mem_desc->base_addr = rec->BaseAddress;
                    mem_desc->len = rec->EndAddress - rec->BaseAddress + 1;
                    mem_desc->scope_bytes = mem_desc_size - sizeof(*mem_desc);
                    next_reserved_mem_desc_base += mem_desc_size;
                }
                *bytes_needed += mem_desc_size;

                break;
            }
        }

        addr += record_hdr->Length;
    }
    if (*bytes_needed + sizeof(mx_iommu_desc_intel_t) +
        desc->scope_bytes + desc->reserved_memory_bytes > desc_len) {
        return MX_ERR_BUFFER_TOO_SMALL;
    }

    return MX_OK;
}

static mx_status_t create_whole_segment_iommu_desc(ACPI_TABLE_DMAR* table,
                                                   ACPI_DMAR_HARDWARE_UNIT* unit,
                                                   mx_iommu_desc_intel_t** desc_out,
                                                   size_t* desc_len_out) {
    assert(unit->Flags & ACPI_DMAR_INCLUDE_ALL);

    // The VT-d spec requires that whole-segment hardware units appear in the
    // DMAR table after all other hardware units on their segment.  Search those
    // entries for scopes to specify as excluded from this descriptor.

    size_t num_scopes = 0;
    size_t num_scopes_on_unit;

    const uintptr_t records_start = ((uintptr_t)table) + sizeof(*table);
    const uintptr_t records_end = (uintptr_t)unit + unit->Header.Length;

    // TODO: Check scopes on self

    uintptr_t addr;
    for (addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
                ACPI_DMAR_HARDWARE_UNIT* rec = (ACPI_DMAR_HARDWARE_UNIT*)record_hdr;
                if (rec->Segment != unit->Segment) {
                    break;
                }
                mx_status_t status = append_scopes(rec, 0, NULL, &num_scopes_on_unit);
                if (status != MX_OK) {
                    return status;
                }
                num_scopes += num_scopes_on_unit;
            }
        }
        addr += record_hdr->Length;
    }

    size_t desc_len = sizeof(mx_iommu_desc_intel_t) +
            sizeof(mx_iommu_desc_intel_scope_t) * num_scopes;
    mx_iommu_desc_intel_t* desc = malloc(desc_len);
    if (!desc) {
        return MX_ERR_NO_MEMORY;
    }
    desc->register_base = unit->Address;
    desc->pci_segment = unit->Segment;
    desc->whole_segment = true;
    desc->scope_bytes = 0;
    desc->reserved_memory_bytes = 0;

    for (addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
                ACPI_DMAR_HARDWARE_UNIT* rec = (ACPI_DMAR_HARDWARE_UNIT*)record_hdr;
                if (rec->Segment != unit->Segment) {
                    break;
                }
                size_t scopes_found = 0;
                mx_iommu_desc_intel_scope_t* scopes = (mx_iommu_desc_intel_scope_t*)(
                        (uintptr_t)desc + sizeof(*desc) + desc->scope_bytes);
                mx_status_t status = append_scopes(rec, num_scopes, scopes, &scopes_found);
                if (status != MX_OK) {
                    free(desc);
                    return status;
                }
                desc->scope_bytes += scopes_found * sizeof(mx_iommu_desc_intel_scope_t);
                num_scopes -= scopes_found;
            }
        }
        addr += record_hdr->Length;
    }

    size_t reserved_mem_bytes = 0;
    mx_status_t status = append_reserved_mem(table, desc, desc_len, &reserved_mem_bytes);
    if (status == MX_ERR_BUFFER_TOO_SMALL) {
        mx_iommu_desc_intel_t* new_desc = realloc(desc, desc_len + reserved_mem_bytes);
        if (new_desc == NULL) {
            free(desc);
            return MX_ERR_NO_MEMORY;
        }
        desc = new_desc;
        desc_len += reserved_mem_bytes;
        status = append_reserved_mem(table, desc, desc_len, &reserved_mem_bytes);
    }
    if (status != MX_OK) {
        free(desc);
        return status;
    }
    desc->reserved_memory_bytes += reserved_mem_bytes;

    *desc_out = desc;
    *desc_len_out = desc_len;
    return MX_OK;
}

static mx_status_t create_non_whole_segment_iommu_desc(ACPI_TABLE_DMAR* table,
                                                       ACPI_DMAR_HARDWARE_UNIT* unit,
                                                       mx_iommu_desc_intel_t** desc_out,
                                                       size_t* desc_len_out) {
    assert((unit->Flags & ACPI_DMAR_INCLUDE_ALL) == 0);

    size_t num_scopes;
    mx_status_t status = append_scopes(unit, 0, NULL, &num_scopes);
    if (status != MX_OK) {
        return status;
    }

    size_t desc_len = sizeof(mx_iommu_desc_intel_t) +
            sizeof(mx_iommu_desc_intel_scope_t) * num_scopes;
    mx_iommu_desc_intel_t* desc = malloc(desc_len);
    if (!desc) {
        return MX_ERR_NO_MEMORY;
    }
    desc->register_base = unit->Address;
    desc->pci_segment = unit->Segment;
    desc->whole_segment = false;
    desc->scope_bytes = 0;
    desc->reserved_memory_bytes = 0;
    mx_iommu_desc_intel_scope_t* scopes = (mx_iommu_desc_intel_scope_t*)(
            (uintptr_t)desc + sizeof(*desc));
    size_t actual_num_scopes;
    status = append_scopes(unit, num_scopes, scopes, &actual_num_scopes);
    if (status != MX_OK) {
        free(desc);
        return status;
    }
    desc->scope_bytes = actual_num_scopes * sizeof(mx_iommu_desc_intel_scope_t);

    size_t reserved_mem_bytes = 0;
    status = append_reserved_mem(table, desc, desc_len, &reserved_mem_bytes);
    if (status == MX_ERR_BUFFER_TOO_SMALL) {
        mx_iommu_desc_intel_t* new_desc = realloc(desc, desc_len + reserved_mem_bytes);
        if (new_desc == NULL) {
            free(desc);
            return MX_ERR_NO_MEMORY;
        }
        desc = new_desc;
        desc_len += reserved_mem_bytes;
        status = append_reserved_mem(table, desc, desc_len, &reserved_mem_bytes);
    }
    if (status != MX_OK) {
        free(desc);
        return status;
    }
    desc->reserved_memory_bytes += reserved_mem_bytes;

    *desc_out = desc;
    *desc_len_out = desc_len;
    return MX_OK;
}

static mx_status_t find_iommus(void) {
    ACPI_TABLE_HEADER* table = NULL;
    ACPI_STATUS status = AcpiGetTable((char*)ACPI_SIG_DMAR, 1, &table);
    if (status != AE_OK) {
        printf("could not find DMAR\n");
        return MX_ERR_NOT_FOUND;
    }
    ACPI_TABLE_DMAR* dmar = (ACPI_TABLE_DMAR*)table;
    const uintptr_t records_start = ((uintptr_t)dmar) + sizeof(*dmar);
    const uintptr_t records_end = ((uintptr_t)dmar) + dmar->Header.Length;
    if (records_start >= records_end) {
        printf("DMAR wraps around address space\n");
        return MX_ERR_IO_DATA_INTEGRITY;
    }
    // Shouldn't be too many records
    if (dmar->Header.Length > 4096) {
        printf("DMAR suspiciously long: %u\n", dmar->Header.Length);
        return MX_ERR_IO_DATA_INTEGRITY;
    }

    uintptr_t addr;
    mx_handle_t iommu_handle = MX_HANDLE_INVALID;
    for (addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        printf("DMAR record: %d\n", record_hdr->Type);
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
                ACPI_DMAR_HARDWARE_UNIT* rec = (ACPI_DMAR_HARDWARE_UNIT*)record_hdr;

                printf("DMAR Hardware Unit: %u %#llx %#x\n", rec->Segment, rec->Address, rec->Flags);
                const bool whole_segment = rec->Flags & ACPI_DMAR_INCLUDE_ALL;

                mx_iommu_desc_intel_t* desc = NULL;
                size_t desc_len = 0;
                mx_status_t mx_status;
                if (whole_segment) {
                    mx_status = create_whole_segment_iommu_desc(dmar, rec, &desc, &desc_len);
                } else {
                    mx_status = create_non_whole_segment_iommu_desc(dmar, rec, &desc, &desc_len);
                }
                if (mx_status != MX_OK) {
                    printf("Failed to create iommu desc: %d\n", mx_status);
                    return mx_status;
                }

                mx_status = mx_iommu_create(root_resource_handle, MX_IOMMU_TYPE_INTEL,
                                            desc, desc_len, &iommu_handle);
                free(desc);
                if (mx_status != MX_OK) {
                    printf("Failed to create iommu: %d\n", mx_status);
                    return mx_status;
                }
                // TODO(teisenbe): Do something with these handles
                //mx_handle_close(iommu_handle);
                break;
            }
            case ACPI_DMAR_TYPE_RESERVED_MEMORY: {
                ACPI_DMAR_RESERVED_MEMORY* rec = (ACPI_DMAR_RESERVED_MEMORY*)record_hdr;
                printf("DMAR Reserved Memory: %u %#llx %#llx\n", rec->Segment, rec->BaseAddress, rec->EndAddress);
                for (uintptr_t scope = addr + 24; scope < addr + rec->Header.Length; ) {
                    ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;
                    printf("  DMAR Scope: %u, bus %u\n", s->EntryType, s->Bus);
                    for (ssize_t i = 0; i < (s->Length - 6) / 2; ++i) {
                        uint16_t v = *(uint16_t*)(scope + 6 + 2 * i);
                        printf("    Path %ld: %02x.%02x\n", i, v & 0xffu, (uint16_t)(v >> 8));
                    }
                    scope += s->Length;
                }
                break;
            }
        }

        addr += record_hdr->Length;
    }
    if (addr != records_end) {
        return MX_ERR_IO_DATA_INTEGRITY;
    }

    return MX_OK;
}
