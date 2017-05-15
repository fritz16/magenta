# mx_bti_create

## NAME

bti_create - create a new bus transaction initiator

## SYNOPSIS

```
#include <magenta/syscalls.h>

mx_status_t mx_bti_create(mx_handle_t iommu, uint64_t bti_id, mx_handle_t* out);

```

## DESCRIPTION

**bti_create**() creates a new [bus transaction initiator](../objects/bus_transaction_initiator.md)
given a handle to an IOMMU and a hardware transaction identifier for a device
downstream of that IOMMU.

Upon success a handle for the new BTI is returned.  This handle will have rights
**MX_RIGHT_READ**, **MX_RIGHT_MAP**, **MX_RIGHT_DUPLICATE**, and
**MX_RIGHT_TRANSFER**.

## RETURN VALUE

**bti_create**() returns MX_OK and a handle to the new BTI
(via *out*) on success.  In the event of failure, a negative error value
is returned.

## ERRORS

**MX_ERR_BAD_HANDLE**  *iommu_resource* is not a valid handle.

**MX_ERR_WRONG_TYPE**  *iommu_resource* is not a resource handle.

**MX_ERR_INVALID_ARGS**  *bti_id* is invalid on the given I/O MMU,
or *out* is an invalid pointer.

**MX_ERR_NO_MEMORY**  (Temporary) Failure due to lack of memory.

## SEE ALSO

[bti_pin](bti_pin.md),
[bti_unpin](bti_unpin.md).
