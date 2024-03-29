/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */ 

#include "blockring.h"
#include "frontend.h"
#include "pdo.h"
#include "fdo.h"
#include "util.h"
#include "debug.h"
#include "srbext.h"
#include "driver.h"
#include <stdlib.h>
#include <xenvbd-ntstrsafe.h>

#define TAG_HEADER                  'gaTX'

struct _XENVBD_BLOCKRING {
    PXENVBD_FRONTEND                Frontend;
    BOOLEAN                         Connected;
    BOOLEAN                         Enabled;

    PXENBUS_STORE_INTERFACE         StoreInterface;

    KSPIN_LOCK                      Lock;
    PMDL                            Mdl;
    blkif_sring_t*                  SharedRing;
    blkif_front_ring_t              FrontRing;
    ULONG                           DeviceId;
    ULONG                           Order;
    PVOID                           Grants[XENVBD_MAX_RING_PAGES];
    ULONG                           Submitted;
    ULONG                           Received;
};

#define MAX_NAME_LEN                64
#define BLOCKRING_POOL_TAG          'gnRX'

#define XEN_IO_PROTO_ABI    "x86_64-abi"

static FORCEINLINE PVOID
__BlockRingAllocate(
    IN  ULONG                       Length
    )
{
    return __AllocateNonPagedPoolWithTag(__FUNCTION__,
                                        __LINE__,
                                        Length,
                                        BLOCKRING_POOL_TAG);
}

static FORCEINLINE VOID
__BlockRingFree(
    IN  PVOID                       Buffer
    )
{
    if (Buffer)
        __FreePoolWithTag(Buffer, BLOCKRING_POOL_TAG);
}

static FORCEINLINE VOID
xen_mb()
{
    KeMemoryBarrier();
    _ReadWriteBarrier();
}

static FORCEINLINE VOID
xen_wmb()
{
    KeMemoryBarrier();
    _WriteBarrier();
}

static FORCEINLINE PFN_NUMBER
__Pfn(
    __in  PVOID                   VirtAddr
    )
{
    return (PFN_NUMBER)(ULONG_PTR)(MmGetPhysicalAddress(VirtAddr).QuadPart >> PAGE_SHIFT);
}

static FORCEINLINE ULONG64
__BlockRingGetTag(
    IN  PXENVBD_BLOCKRING           BlockRing,
    IN  PXENVBD_REQUEST             Request
    )
{
    UNREFERENCED_PARAMETER(BlockRing);
    return ((ULONG64)TAG_HEADER << 32) | (ULONG64)Request->Id;
}

static FORCEINLINE BOOLEAN
__BlockRingPutTag(
    IN  PXENVBD_BLOCKRING           BlockRing,
    IN  ULONG64                     Id,
    OUT PULONG                      Tag
    )
{
    ULONG   Header = (ULONG)((Id >> 32) & 0xFFFFFFFF);

    UNREFERENCED_PARAMETER(BlockRing);

    *Tag    = (ULONG)(Id & 0xFFFFFFFF);
    if (Header != TAG_HEADER) {
        Error("PUT_TAG (%llx) TAG_HEADER (%08x%08x)\n", Id, Header, *Tag);
        return FALSE;
    }

    return TRUE;
}

static FORCEINLINE VOID
__BlockRingInsert(
    IN  PXENVBD_BLOCKRING           BlockRing,
    IN  PXENVBD_REQUEST             Request,
    IN  blkif_request_t*            req
    )
{
    PXENVBD_GRANTER                 Granter = FrontendGetGranter(BlockRing->Frontend);

    switch (Request->Operation) {
    case BLKIF_OP_READ:
    case BLKIF_OP_WRITE:
        if (Request->NrSegments > BLKIF_MAX_SEGMENTS_PER_REQUEST) {
            // Indirect
            ULONG                       PageIdx;
            ULONG                       SegIdx;
            PLIST_ENTRY                 PageEntry;
            PLIST_ENTRY                 SegEntry;
            blkif_request_indirect_t*   req_indirect;

            req_indirect = (blkif_request_indirect_t*)req;
            req_indirect->operation         = BLKIF_OP_INDIRECT;
            req_indirect->indirect_op       = Request->Operation;
            req_indirect->nr_segments       = Request->NrSegments;
            req_indirect->id                = __BlockRingGetTag(BlockRing, Request);
            req_indirect->sector_number     = Request->FirstSector;
            req_indirect->handle            = (USHORT)BlockRing->DeviceId;

            for (PageIdx = 0,
                 PageEntry = Request->Indirects.Flink,
                 SegEntry = Request->Segments.Flink;
                    PageIdx < BLKIF_MAX_INDIRECT_PAGES_PER_REQUEST &&
                    PageEntry != &Request->Indirects &&
                    SegEntry != &Request->Segments;
                        ++PageIdx, PageEntry = PageEntry->Flink) {
                PXENVBD_INDIRECT Page = CONTAINING_RECORD(PageEntry, XENVBD_INDIRECT, Entry);

                req_indirect->indirect_grefs[PageIdx] = GranterReference(Granter, Page->Grant);

                for (SegIdx = 0;
                        SegIdx < XENVBD_MAX_SEGMENTS_PER_PAGE &&
                        SegEntry != &Request->Segments;
                            ++SegIdx, SegEntry = SegEntry->Flink) {
                    PXENVBD_SEGMENT Segment = CONTAINING_RECORD(SegEntry, XENVBD_SEGMENT, Entry);

                    Page->Page[SegIdx].GrantRef = GranterReference(Granter, Segment->Grant);
                    Page->Page[SegIdx].First    = Segment->FirstSector;
                    Page->Page[SegIdx].Last     = Segment->LastSector;
                }
            }
        } else {
            // Direct
            ULONG           Index;
            PLIST_ENTRY     Entry;

            req->operation                  = Request->Operation;
            req->nr_segments                = (UCHAR)Request->NrSegments;
            req->handle                     = (USHORT)BlockRing->DeviceId;
            req->id                         = __BlockRingGetTag(BlockRing, Request);
            req->sector_number              = Request->FirstSector;

            for (Index = 0, Entry = Request->Segments.Flink;
                    Index < BLKIF_MAX_SEGMENTS_PER_REQUEST &&
                    Entry != &Request->Segments;
                        ++Index, Entry = Entry->Flink) {
                PXENVBD_SEGMENT Segment = CONTAINING_RECORD(Entry, XENVBD_SEGMENT, Entry);
                req->seg[Index].gref        = GranterReference(Granter, Segment->Grant);
                req->seg[Index].first_sect  = Segment->FirstSector;
                req->seg[Index].last_sect   = Segment->LastSector;
            }
        }
        break;

    case BLKIF_OP_WRITE_BARRIER:
        req->operation                  = Request->Operation;
        req->nr_segments                = 0;
        req->handle                     = (USHORT)BlockRing->DeviceId;
        req->id                         = __BlockRingGetTag(BlockRing, Request);
        req->sector_number              = Request->FirstSector;
        break;

    case BLKIF_OP_DISCARD: {
        blkif_request_discard_t*        req_discard;
        req_discard = (blkif_request_discard_t*)req;
        req_discard->operation          = BLKIF_OP_DISCARD;
        req_discard->flag               = Request->Flags;
        req_discard->handle             = (USHORT)BlockRing->DeviceId;
        req_discard->id                 = __BlockRingGetTag(BlockRing, Request);
        req_discard->sector_number      = Request->FirstSector;
        req_discard->nr_sectors         = Request->NrSectors;
        } break;

    default:
        ASSERT(FALSE);
        break;
    }
    ++BlockRing->Submitted;
}

NTSTATUS
BlockRingCreate(
    IN  PXENVBD_FRONTEND            Frontend,
    IN  ULONG                       DeviceId,
    OUT PXENVBD_BLOCKRING*          BlockRing
    )
{
    *BlockRing = __BlockRingAllocate(sizeof(XENVBD_BLOCKRING));
    if (*BlockRing == NULL)
        goto fail1;

    (*BlockRing)->Frontend = Frontend;
    (*BlockRing)->DeviceId = DeviceId;
    KeInitializeSpinLock(&(*BlockRing)->Lock);

    return STATUS_SUCCESS;

fail1:
    return STATUS_NO_MEMORY;
}

VOID
BlockRingDestroy(
    IN  PXENVBD_BLOCKRING           BlockRing
    )
{
    BlockRing->Frontend = NULL;
    BlockRing->DeviceId = 0;
    RtlZeroMemory(&BlockRing->Lock, sizeof(KSPIN_LOCK));
    
    ASSERT(IsZeroMemory(BlockRing, sizeof(XENVBD_BLOCKRING)));
    
    __BlockRingFree(BlockRing);
}

NTSTATUS
BlockRingConnect(
    IN  PXENVBD_BLOCKRING           BlockRing
    )
{
    NTSTATUS        status;
    PCHAR           Value;
    ULONG           Index, RingPages;
    PXENVBD_FDO     Fdo = PdoGetFdo(FrontendGetPdo(BlockRing->Frontend));
    PXENVBD_GRANTER Granter = FrontendGetGranter(BlockRing->Frontend);

    ASSERT(BlockRing->Connected == FALSE);

    BlockRing->StoreInterface = FdoAcquireStore(Fdo);

    status = STATUS_UNSUCCESSFUL;
    if (BlockRing->StoreInterface == NULL)
        goto fail1;

    status = FrontendStoreReadBackend(BlockRing->Frontend, "max-ring-page-order", &Value);
    if (NT_SUCCESS(status)) {
        BlockRing->Order = __min(strtoul(Value, NULL, 10), XENVBD_MAX_RING_PAGE_ORDER);
        FrontendStoreFree(BlockRing->Frontend, Value);
    } else {
        BlockRing->Order = 0;
    }

    status = STATUS_NO_MEMORY;
    BlockRing->SharedRing = __AllocPages((SIZE_T)PAGE_SIZE << BlockRing->Order, &BlockRing->Mdl);
    if (BlockRing->SharedRing == NULL)
        goto fail2;

#pragma warning(push)
#pragma warning(disable: 4305)
#pragma warning(disable: 4311)
    SHARED_RING_INIT(BlockRing->SharedRing);
    FRONT_RING_INIT(&BlockRing->FrontRing, BlockRing->SharedRing, PAGE_SIZE << BlockRing->Order);
#pragma warning(pop)

    RingPages = (1 << BlockRing->Order);
    for (Index = 0; Index < RingPages; ++Index) {
        status = GranterGet(Granter, __Pfn((PUCHAR)BlockRing->SharedRing + (Index * PAGE_SIZE)), 
                                FALSE, &BlockRing->Grants[Index]);
        if (!NT_SUCCESS(status))
            goto fail3;
    }

    BlockRing->Connected = TRUE;
    return STATUS_SUCCESS;

fail3:
    for (Index = 0; Index < XENVBD_MAX_RING_PAGES; ++Index) {
        if (BlockRing->Grants[Index])
            GranterPut(Granter, BlockRing->Grants[Index]);
        BlockRing->Grants[Index] = 0;
    }

    RtlZeroMemory(&BlockRing->FrontRing, sizeof(BlockRing->FrontRing));
    __FreePages(BlockRing->SharedRing, BlockRing->Mdl);
    BlockRing->SharedRing = NULL;
    BlockRing->Mdl = NULL;

fail2:
fail1:
    return status;
}

NTSTATUS
BlockRingStoreWrite(
    IN  PXENVBD_BLOCKRING           BlockRing,
    IN  PXENBUS_STORE_TRANSACTION   Transaction,
    IN  PCHAR                       FrontendPath
    )
{
    PXENVBD_GRANTER                 Granter = FrontendGetGranter(BlockRing->Frontend);
    NTSTATUS                        status;

    if (BlockRing->Order == 0) {
        status = XENBUS_STORE(Printf, 
                              BlockRing->StoreInterface, 
                              Transaction, 
                              FrontendPath,
                              "ring-ref", 
                              "%u", 
                              GranterReference(Granter, BlockRing->Grants[0]));
        if (!NT_SUCCESS(status))
            return status;
    } else {
        ULONG   Index, RingPages;

        status = XENBUS_STORE(Printf, 
                              BlockRing->StoreInterface, 
                              Transaction, 
                              FrontendPath, 
                              "ring-page-order", 
                              "%u", 
                              BlockRing->Order);
        if (!NT_SUCCESS(status))
            return status;

        RingPages = (1 << BlockRing->Order);
        for (Index = 0; Index < RingPages; ++Index) {
            CHAR    Name[MAX_NAME_LEN+1];
            status = RtlStringCchPrintfA(Name, MAX_NAME_LEN, "ring-ref%u", Index);
            if (!NT_SUCCESS(status))
                return status;
            status = XENBUS_STORE(Printf, 
                                  BlockRing->StoreInterface, 
                                  Transaction, 
                                  FrontendPath,
                                  Name, 
                                  "%u", 
                                  GranterReference(Granter, BlockRing->Grants[Index]));
            if (!NT_SUCCESS(status))
                return status;
        }
    }

    status = XENBUS_STORE(Printf, 
                          BlockRing->StoreInterface, 
                          Transaction, 
                          FrontendPath,
                          "protocol", 
                          XEN_IO_PROTO_ABI);
    if (!NT_SUCCESS(status))
        return status;

    return STATUS_SUCCESS;
}

VOID
BlockRingEnable(
    IN  PXENVBD_BLOCKRING           BlockRing
    )
{
    ASSERT(BlockRing->Enabled == FALSE);

    BlockRing->Enabled = TRUE;
}

VOID
BlockRingDisable(
    IN  PXENVBD_BLOCKRING           BlockRing
    )
{
    ASSERT(BlockRing->Enabled == TRUE);

    BlockRing->Enabled = FALSE;
}

VOID
BlockRingDisconnect(
    IN  PXENVBD_BLOCKRING           BlockRing
    )
{
    ULONG           Index;
    PXENVBD_GRANTER Granter = FrontendGetGranter(BlockRing->Frontend);

    ASSERT(BlockRing->Connected == TRUE);

    BlockRing->Submitted = 0;
    BlockRing->Received = 0;

    for (Index = 0; Index < XENVBD_MAX_RING_PAGES; ++Index) {
        if (BlockRing->Grants[Index]) {
            GranterPut(Granter, BlockRing->Grants[Index]);
        }
        BlockRing->Grants[Index] = 0;
    }

    RtlZeroMemory(&BlockRing->FrontRing, sizeof(BlockRing->FrontRing));
    __FreePages(BlockRing->SharedRing, BlockRing->Mdl);
    BlockRing->SharedRing = NULL;
    BlockRing->Mdl = NULL;

    BlockRing->Order = 0;

    XENBUS_STORE(Release, BlockRing->StoreInterface);
    BlockRing->StoreInterface = NULL;

    BlockRing->Connected = FALSE;
}

VOID
BlockRingDebugCallback(
    IN  PXENVBD_BLOCKRING           BlockRing,
    IN  PXENBUS_DEBUG_INTERFACE     Debug
    )
{
    ULONG           Index;
    PXENVBD_GRANTER Granter = FrontendGetGranter(BlockRing->Frontend);

    XENBUS_DEBUG(Printf, Debug,
                 "BLOCKRING: Requests  : %d / %d\n",
                 BlockRing->Submitted,
                 BlockRing->Received);

    XENBUS_DEBUG(Printf, Debug,
                 "BLOCKRING: SharedRing : 0x%p\n", 
                 BlockRing->SharedRing);

    if (BlockRing->SharedRing) {
        XENBUS_DEBUG(Printf, Debug,
                     "BLOCKRING: SharedRing : %d / %d - %d / %d\n",
                     BlockRing->SharedRing->req_prod,
                     BlockRing->SharedRing->req_event,
                     BlockRing->SharedRing->rsp_prod,
                     BlockRing->SharedRing->rsp_event);
    }

    XENBUS_DEBUG(Printf, Debug,
                 "BLOCKRING: FrontRing  : %d / %d (%d)\n",
                 BlockRing->FrontRing.req_prod_pvt,
                 BlockRing->FrontRing.rsp_cons,
                 BlockRing->FrontRing.nr_ents);

    XENBUS_DEBUG(Printf, Debug,
                 "BLOCKRING: Order      : %d\n",
                 BlockRing->Order);
    for (Index = 0; Index < (1ul << BlockRing->Order); ++Index) {
        XENBUS_DEBUG(Printf, Debug,
                     "BLOCKRING: Grants[%-2d] : 0x%p (%u)\n", 
                     Index,
                     BlockRing->Grants[Index],
                     GranterReference(Granter, BlockRing->Grants[Index]));
    }

    BlockRing->Submitted = BlockRing->Received = 0;
}

VOID
BlockRingPoll(
    IN  PXENVBD_BLOCKRING           BlockRing
    )
{
    PXENVBD_PDO Pdo = FrontendGetPdo(BlockRing->Frontend);

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    KeAcquireSpinLockAtDpcLevel(&BlockRing->Lock);

    // Guard against this locked region being called after the 
    // lock on FrontendSetState
    if (BlockRing->Enabled == FALSE)
        goto done;

    for (;;) {
        ULONG   rsp_prod;
        ULONG   rsp_cons;

        KeMemoryBarrier();

        rsp_prod = BlockRing->SharedRing->rsp_prod;
        rsp_cons = BlockRing->FrontRing.rsp_cons;

        KeMemoryBarrier();

        if (rsp_cons == rsp_prod)
            break;

        while (rsp_cons != rsp_prod) {
            blkif_response_t*   Response;
            ULONG               Tag;

            Response = RING_GET_RESPONSE(&BlockRing->FrontRing, rsp_cons);
            ++rsp_cons;

            if (__BlockRingPutTag(BlockRing, Response->id, &Tag)) {
                ++BlockRing->Received;
                PdoCompleteResponse(Pdo, Tag, Response->status);
            }

            RtlZeroMemory(Response, sizeof(union blkif_sring_entry));
        }

        KeMemoryBarrier();

        BlockRing->FrontRing.rsp_cons = rsp_cons;
        BlockRing->SharedRing->rsp_event = rsp_cons + 1;
    }

done:
    KeReleaseSpinLockFromDpcLevel(&BlockRing->Lock);
}

BOOLEAN
BlockRingSubmit(
    IN  PXENVBD_BLOCKRING           BlockRing,
    IN  PXENVBD_REQUEST             Request
    )
{
    KIRQL               Irql;
    blkif_request_t*    req;
    BOOLEAN             Notify;

    KeAcquireSpinLock(&BlockRing->Lock, &Irql);
    if (RING_FULL(&BlockRing->FrontRing)) {
        KeReleaseSpinLock(&BlockRing->Lock, Irql);
        return FALSE;
    }

    req = RING_GET_REQUEST(&BlockRing->FrontRing, BlockRing->FrontRing.req_prod_pvt);
    __BlockRingInsert(BlockRing, Request, req);
    KeMemoryBarrier();
    ++BlockRing->FrontRing.req_prod_pvt;

    RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&BlockRing->FrontRing, Notify);
    KeReleaseSpinLock(&BlockRing->Lock, Irql);

    if (Notify)
        NotifierSend(FrontendGetNotifier(BlockRing->Frontend));

    return TRUE;
}
