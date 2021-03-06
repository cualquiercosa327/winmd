/* Copyright (c) Mark Harmstone 2019
 *
 * This file is part of WinMD.
 *
 * WinMD is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinMD.  If not, see <http://www.gnu.org/licenses/>. */

#include "winmd.h"

NTSTATUS set_pdo::read_raid45(PIRP Irp, bool* no_complete) {
    auto IrpSp = IoGetCurrentIrpStackLocation(Irp);
    bool mdl_locked = true;
    uint64_t offset = IrpSp->Parameters.Read.ByteOffset.QuadPart;
    uint32_t length = IrpSp->Parameters.Read.Length;
    void* dummypage = nullptr;
    PMDL dummy_mdl = nullptr;
    uint8_t* tmpbuf = nullptr;
    PMDL tmpmdl = nullptr;

    shared_eresource l(&lock);

    if (array_info.level == RAID_LEVEL_5 && array_info.layout != RAID_LAYOUT_LEFT_SYMMETRIC &&
        array_info.layout != RAID_LAYOUT_RIGHT_SYMMETRIC && array_info.layout != RAID_LAYOUT_LEFT_ASYMMETRIC &&
        array_info.layout != RAID_LAYOUT_RIGHT_ASYMMETRIC)
        return STATUS_INVALID_DEVICE_REQUEST;

    bool asymmetric = array_info.level == RAID_LEVEL_5 && (array_info.layout == RAID_LAYOUT_LEFT_ASYMMETRIC || array_info.layout == RAID_LAYOUT_RIGHT_ASYMMETRIC);

    if (array_info.chunksize == 0 || (array_info.chunksize * 512) % PAGE_SIZE != 0)
        return STATUS_INTERNAL_ERROR;

    uint64_t startoff, endoff;
    uint32_t startoffstripe, endoffstripe;
    uint32_t stripe_length = array_info.chunksize * 512;

    get_raid0_offset(offset, stripe_length, array_info.raid_disks - 1, &startoff, &startoffstripe);
    get_raid0_offset(offset + length - 1, stripe_length, array_info.raid_disks - 1, &endoff, &endoffstripe);

    uint64_t start_chunk = offset / stripe_length;
    uint64_t end_chunk = (offset + length - 1) / stripe_length;

    if (start_chunk == end_chunk) { // small reads, on one device
        auto parity = get_parity_volume(offset);
        uint32_t disk_num = get_physical_stripe(startoffstripe, parity);

        auto c = child_list[disk_num];

        IoCopyCurrentIrpStackLocationToNext(Irp);

        auto IrpSp2 = IoGetNextIrpStackLocation(Irp);

        uint64_t start = (start_chunk / (array_info.raid_disks - 1)) * stripe_length;

        start += offset % stripe_length;
        start += c->disk_info.data_offset * 512;

        IrpSp2->FileObject = c->fileobj;
        IrpSp2->Parameters.Read.ByteOffset.QuadPart = start;

        *no_complete = true;

        return IoCallDriver(c->device, Irp);
    }

    uint32_t skip_first = offset % PAGE_SIZE;

    startoff -= skip_first;
    offset -= skip_first;
    length += skip_first;

    auto ctxs = (io_context*)ExAllocatePoolWithTag(NonPagedPool, sizeof(io_context) * array_info.raid_disks, ALLOC_TAG);
    if (!ctxs) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ctxs, sizeof(io_context) * array_info.raid_disks);

    bool need_dummy = false;

    uint32_t pos = 0;
    while (pos < length) {
        auto parity = get_parity_volume(offset + pos);

        if (pos == 0) {
            uint32_t stripe = get_physical_stripe(startoffstripe, parity);

            for (uint32_t i = startoffstripe; i < array_info.raid_disks - 1; i++) {
                if (i == startoffstripe) {
                    auto readlen = min(length, (uint32_t)(stripe_length - (startoff % stripe_length)));

                    ctxs[stripe].stripe_start = startoff;
                    ctxs[stripe].stripe_end = startoff + readlen;

                    pos += readlen;

                    if (pos == length)
                        break;
                } else {
                    auto readlen = min(length - pos, (uint32_t)stripe_length);

                    ctxs[stripe].stripe_start = startoff - (startoff % stripe_length);
                    ctxs[stripe].stripe_end = ctxs[stripe].stripe_start + readlen;

                    pos += readlen;

                    if (pos == length)
                        break;
                }

                if (asymmetric) {
                    stripe++;

                    if (stripe == parity)
                        stripe++;
                } else
                    stripe = (stripe + 1) % array_info.raid_disks;
            }

            if (pos == length)
                break;

            for (uint32_t i = 0; i < startoffstripe; i++) {
                uint32_t stripe2 = get_physical_stripe(i, parity);

                ctxs[stripe2].stripe_start = ctxs[stripe2].stripe_end = startoff - (startoff % stripe_length) + stripe_length;
            }

            ctxs[parity].stripe_start = ctxs[parity].stripe_end = startoff - (startoff % stripe_length) + stripe_length;

            if (length - pos > array_info.raid_disks * (array_info.raid_disks - 1) * stripe_length) {
                auto skip = (uint32_t)(((length - pos) / (array_info.raid_disks * (array_info.raid_disks - 1) * stripe_length)) - 1);

                for (uint32_t i = 0; i < array_info.raid_disks; i++) {
                    ctxs[i].stripe_end += skip * array_info.raid_disks * stripe_length;
                }

                pos += (uint32_t)(skip * (array_info.raid_disks - 1) * array_info.raid_disks * stripe_length);
                need_dummy = true;
            }
        } else if (length - pos >= stripe_length * (array_info.raid_disks - 1)) {
            for (uint32_t i = 0; i < array_info.raid_disks; i++) {
                ctxs[i].stripe_end += stripe_length;
            }

            pos += (uint32_t)(stripe_length * (array_info.raid_disks - 1));
            need_dummy = true;
        } else {
            uint32_t stripe = get_physical_stripe(0, parity);

            for (uint32_t i = 0; i < array_info.raid_disks - 1; i++) {
                if (endoffstripe == i) {
                    ctxs[stripe].stripe_end = endoff + 1;
                    break;
                } else if (endoffstripe > i)
                    ctxs[stripe].stripe_end = endoff - (endoff % stripe_length) + stripe_length;

                if (asymmetric) {
                    stripe++;

                    if (stripe == parity)
                        stripe++;
                } else
                    stripe = (stripe + 1) % array_info.raid_disks;
            }

            break;
        }
    }

    NTSTATUS Status;

    for (unsigned int i = 0; i < array_info.raid_disks; i++) {
        if (ctxs[i].stripe_end != ctxs[i].stripe_start) {
            ctxs[i].Irp = IoAllocateIrp(child_list[i]->device->StackSize, false);

            if (!ctxs[i].Irp) {
                ERR("IoAllocateIrp failed\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            auto IrpSp2 = IoGetNextIrpStackLocation(ctxs[i].Irp);
            IrpSp2->MajorFunction = IRP_MJ_READ;

            ctxs[i].mdl = IoAllocateMdl(nullptr, (ULONG)(ctxs[i].stripe_end - ctxs[i].stripe_start), false, false, nullptr);
            if (!ctxs[i].mdl) {
                ERR("IoAllocateMdl failed\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            ctxs[i].mdl->MdlFlags |= MDL_PARTIAL;

            ctxs[i].Irp->MdlAddress = ctxs[i].mdl;

            IrpSp2->FileObject = child_list[i]->fileobj;
            IrpSp2->Parameters.Read.Length = (ULONG)(ctxs[i].stripe_end - ctxs[i].stripe_start);
            IrpSp2->Parameters.Read.ByteOffset.QuadPart = ctxs[i].stripe_start + (child_list[i]->disk_info.data_offset * 512);

            ctxs[i].Irp->UserIosb = &ctxs[i].iosb;

            KeInitializeEvent(&ctxs[i].Event, NotificationEvent, false);
            ctxs[i].Irp->UserEvent = &ctxs[i].Event;

            IoSetCompletionRoutine(ctxs[i].Irp, io_completion, &ctxs[i], true, true, true);
        } else
            ctxs[i].Status = STATUS_SUCCESS;
    }

    mdl_locked = Irp->MdlAddress->MdlFlags & (MDL_PAGES_LOCKED | MDL_PARTIAL);

    if (!mdl_locked) {
        Status = STATUS_SUCCESS;

        seh_try {
            MmProbeAndLockPages(Irp->MdlAddress, KernelMode, IoWriteAccess);
        } seh_except (EXCEPTION_EXECUTE_HANDLER) {
            Status = GetExceptionCode();
        }

        if (!NT_SUCCESS(Status)) {
            ERR("MmProbeAndLockPages threw exception %08x\n", Status);
            mdl_locked = true;
            goto end;
        }
    }

    if (Irp->MdlAddress->ByteOffset != 0 || skip_first != 0) {
        tmpbuf = (uint8_t*)ExAllocatePoolWithTag(NonPagedPool, length, ALLOC_TAG);
        if (!tmpbuf) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        tmpmdl = IoAllocateMdl(tmpbuf, length, false, false, nullptr);
        if (!tmpmdl) {
            ERR("IoAllocateMdl failed\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        MmBuildMdlForNonPagedPool(tmpmdl);
    }

    {
        uint32_t pos = 0;
        PFN_NUMBER dummy;

        if (need_dummy) {
            dummypage = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, ALLOC_TAG);
            if (!dummypage) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            dummy_mdl = IoAllocateMdl(dummypage, PAGE_SIZE, FALSE, FALSE, NULL);
            if (!dummy_mdl) {
                ERR("IoAllocateMdl failed\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            MmBuildMdlForNonPagedPool(dummy_mdl);

            dummy = *(PFN_NUMBER*)(dummy_mdl + 1);
        }

        MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);

        for (unsigned int i = 0; i < array_info.raid_disks; i++) {
            if (ctxs[i].mdl)
                ctxs[i].pfnp = ctxs[i].pfns = MmGetMdlPfnArray(ctxs[i].mdl);
        }

        auto src_pfns = MmGetMdlPfnArray((tmpmdl ? tmpmdl : Irp->MdlAddress));

        while (pos < length) {
            auto parity = get_parity_volume(offset + pos);

            if (pos == 0) {
                uint32_t stripe = get_physical_stripe(startoffstripe, parity);

                for (uint32_t i = startoffstripe; i < array_info.raid_disks - 1; i++) {
                    uint32_t len, pages;

                    if (pos == 0) {
                        len = stripe_length - (startoff % stripe_length);

                        if (len % PAGE_SIZE != 0) {
                            pages = len / PAGE_SIZE;
                            pages++;
                        } else
                            pages = len / PAGE_SIZE;
                    } else {
                        len = stripe_length;
                        pages = len / PAGE_SIZE;
                    }

                    if (pos + len > length) {
                        len = length - pos;

                        if (len % PAGE_SIZE != 0) {
                            pages = len / PAGE_SIZE;
                            pages++;
                        } else
                            pages = len / PAGE_SIZE;
                    }

                    RtlCopyMemory(ctxs[stripe].pfnp, src_pfns, sizeof(PFN_NUMBER) * pages);
                    src_pfns = &src_pfns[pages];
                    ctxs[stripe].pfnp = &ctxs[stripe].pfnp[pages];

                    pos += len;

                    if (asymmetric) {
                        stripe++;

                        if (stripe == parity)
                            stripe++;
                    } else
                        stripe = (stripe + 1) % array_info.raid_disks;
                }
            } else if (length - pos >= stripe_length * (array_info.raid_disks - 1)) {
                uint32_t stripe = get_physical_stripe(0, parity);
                uint32_t pages = stripe_length / PAGE_SIZE;

                for (uint32_t i = 0; i < array_info.raid_disks - 1; i++) {
                    RtlCopyMemory(ctxs[stripe].pfnp, src_pfns, sizeof(PFN_NUMBER) * pages);
                    src_pfns = &src_pfns[pages];
                    ctxs[stripe].pfnp = &ctxs[stripe].pfnp[pages];

                    pos += stripe_length;

                    if (asymmetric) {
                        stripe++;

                        if (stripe == parity)
                            stripe++;
                    } else
                        stripe = (stripe + 1) % array_info.raid_disks;
                }

                for (uint32_t k = 0; k < stripe_length / PAGE_SIZE; k++) {
                    ctxs[parity].pfnp[0] = dummy;
                    ctxs[parity].pfnp = &ctxs[parity].pfnp[1];
                }
            } else {
                uint32_t stripe = get_physical_stripe(0, parity);

                for (uint32_t i = 0; i < array_info.raid_disks - 1; i++) {
                    uint32_t readlen, pages;

                    if (length - pos < stripe_length) {
                        readlen = length - pos;
                        pages = readlen / PAGE_SIZE;

                        if ((length - pos) % PAGE_SIZE != 0)
                            pages++;
                    } else {
                        readlen = stripe_length;
                        pages = stripe_length / PAGE_SIZE;
                    }

                    RtlCopyMemory(ctxs[stripe].pfnp, src_pfns, sizeof(PFN_NUMBER) * pages);
                    src_pfns = &src_pfns[pages];
                    ctxs[stripe].pfnp = &ctxs[stripe].pfnp[pages];

                    pos += readlen;

                    if (pos == length)
                        break;

                    if (asymmetric) {
                        stripe++;

                        if (stripe == parity)
                            stripe++;
                    } else
                        stripe = (stripe + 1) % array_info.raid_disks;
                }
            }
        }
    }

    for (unsigned int i = 0; i < array_info.raid_disks; i++) {
        if (ctxs[i].Irp) {
            ctxs[i].Status = IoCallDriver(child_list[i]->device, ctxs[i].Irp);
            if (!NT_SUCCESS(ctxs[i].Status))
                ERR("IoCallDriver returned %08x\n", ctxs[i].Status);
        }
    }

    Status = STATUS_SUCCESS;

    for (unsigned int i = 0; i < array_info.raid_disks; i++) {
        if (ctxs[i].Status == STATUS_PENDING) {
            KeWaitForSingleObject(&ctxs[i].Event, Executive, KernelMode, false, nullptr);
            ctxs[i].Status = ctxs[i].iosb.Status;
        }

        if (!NT_SUCCESS(ctxs[i].Status))
            Status = ctxs[i].Status;
    }

    if (tmpbuf) {
        auto dest = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);

        RtlCopyMemory(dest, tmpbuf + skip_first, length - skip_first);
    }

end:
    if (!mdl_locked)
        MmUnlockPages(Irp->MdlAddress);

    if (dummy_mdl)
        IoFreeMdl(dummy_mdl);

    if (dummypage)
        ExFreePool(dummypage);

    for (unsigned int i = 0; i < array_info.raid_disks; i++) {
        if (ctxs[i].mdl)
            IoFreeMdl(ctxs[i].mdl);

        if (ctxs[i].va)
            ExFreePool(ctxs[i].va);

        if (ctxs[i].Irp)
            IoFreeIrp(ctxs[i].Irp);
    }

    ExFreePool(ctxs);

    if (tmpmdl)
        IoFreeMdl(tmpmdl);

    if (tmpbuf)
        ExFreePool(tmpbuf);

    return Status;
}

#ifdef DEBUG_PARANOID
void set_pdo::paranoid_raid5_check(uint64_t parity_offset, uint32_t parity_length) {
    uint32_t data_disks = array_info.raid_disks - 1;
    uint64_t read_offset = parity_offset / data_disks;

    parity_length /= data_disks;

    klist<io_context> ctxs;

    for (uint32_t i = 0; i < array_info.raid_disks; i++) {
        ctxs.emplace_back_np(child_list[i], read_offset + (child_list[i]->disk_info.data_offset * 512), parity_length);
        auto& last = ctxs.back();

        if (!NT_SUCCESS(last.Status)) {
            ERR("io_context constructor returned %08x\n", last.Status);
            return;
        }

        last.va = ExAllocatePoolWithTag(NonPagedPool, parity_length, ALLOC_TAG);
        if (!last.va) {
            ERR("out of memory\n");
            return;
        }
    }

    LIST_ENTRY* le = ctxs.list.Flink;
    while (le != &ctxs.list) {
        auto& ctx = ctxs.entry(le);

        auto IrpSp = IoGetNextIrpStackLocation(ctx.Irp);
        IrpSp->MajorFunction = IRP_MJ_READ;

        ctx.mdl = IoAllocateMdl(ctx.va, parity_length, false, false, nullptr);
        if (!ctx.mdl) {
            ERR("IoAllocateMdl failed\n");
            return;
        }

        MmBuildMdlForNonPagedPool(ctx.mdl);

        ctx.Irp->MdlAddress = ctx.mdl;

        IrpSp->FileObject = ctx.sc->fileobj;
        IrpSp->Parameters.Read.ByteOffset.QuadPart = ctx.stripe_start;
        IrpSp->Parameters.Read.Length = parity_length;

        ctx.Status = IoCallDriver(ctx.sc->device, ctx.Irp);

        le = le->Flink;
    }

    le = ctxs.list.Flink;
    while (le != &ctxs.list) {
        auto& ctx = ctxs.entry(le);

        if (ctx.Status == STATUS_PENDING) {
            KeWaitForSingleObject(&ctx.Event, Executive, KernelMode, false, nullptr);
            ctx.Status = ctx.iosb.Status;
        }

        if (!NT_SUCCESS(ctx.Status))
            ERR("writing returned %08x\n", ctx.Status);

        le = le->Flink;
    }

    le = ctxs.list.Flink;

    auto& first = ctxs.entry(le);

    le = le->Flink;
    while (le != &ctxs.list) {
        auto& ctx = ctxs.entry(le);

        do_xor((uint8_t*)first.va, (uint8_t*)ctx.va, parity_length);

        le = le->Flink;
    }

    for (unsigned int i = 0; i < parity_length; i++) {
        if (((uint8_t*)first.va)[i] != 0) {
            ERR("parity error\n");
            __debugbreak();
        }
    }
}
#endif

NTSTATUS set_pdo::write_raid45(PIRP Irp, bool* no_complete) {
    auto IrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;
    uint64_t offset = IrpSp->Parameters.Write.ByteOffset.QuadPart, parity_offset = offset;
    uint32_t length = IrpSp->Parameters.Write.Length, parity_length = length;
    uint8_t* data;
    uint8_t* parity_data = nullptr;
    PMDL parity_mdl = nullptr;
    uint8_t* tmpbuf = nullptr;
    PMDL tmpmdl = nullptr;

    if (array_info.level == RAID_LEVEL_5 && array_info.layout != RAID_LAYOUT_LEFT_SYMMETRIC &&
        array_info.layout != RAID_LAYOUT_RIGHT_SYMMETRIC && array_info.layout != RAID_LAYOUT_LEFT_ASYMMETRIC &&
        array_info.layout != RAID_LAYOUT_RIGHT_ASYMMETRIC)
        return STATUS_INVALID_DEVICE_REQUEST;

    bool asymmetric = array_info.level == RAID_LEVEL_5 && (array_info.layout == RAID_LAYOUT_LEFT_ASYMMETRIC || array_info.layout == RAID_LAYOUT_RIGHT_ASYMMETRIC);

    if (array_info.chunksize == 0 || (array_info.chunksize * 512) % PAGE_SIZE != 0)
        return STATUS_INTERNAL_ERROR;

    if ((offset % 512) != 0 || (length % 512) != 0)
        return STATUS_INVALID_PARAMETER;

    uint32_t full_chunk = array_info.chunksize * 512 * (array_info.raid_disks - 1);
    bool mdl_locked = Irp->MdlAddress->MdlFlags & (MDL_PAGES_LOCKED | MDL_PARTIAL);
    io_context* ctxs = nullptr;
    uint64_t startoff, endoff, start_chunk, end_chunk;
    uint32_t startoffstripe, endoffstripe, stripe_length, pos;
    uint32_t skip_first = offset % PAGE_SIZE ? (PAGE_SIZE - (offset % PAGE_SIZE)) : 0;
    io_context first_bit;

    if (!mdl_locked) {
        Status = STATUS_SUCCESS;

        seh_try {
            MmProbeAndLockPages(Irp->MdlAddress, KernelMode, IoReadAccess);
        } seh_except (EXCEPTION_EXECUTE_HANDLER) {
            Status = GetExceptionCode();
        }

        if (!NT_SUCCESS(Status)) {
            ERR("MmProbeAndLockPages threw exception %08x\n", Status);
            mdl_locked = true;
            goto end;
        }
    }

    data = (uint8_t*)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);

    if (offset % full_chunk != 0) {
        Status = add_partial_chunk(offset, min(length, full_chunk - (offset % full_chunk)), data);
        if (!NT_SUCCESS(Status))
            goto end;

        uint32_t skip_start = min(length, full_chunk - (offset % full_chunk));
        parity_offset += skip_start;
        parity_length -= skip_start;
    }

    if (parity_length % full_chunk != 0) {
        // FIXME - don't call if covered by previous add_partial_chunk
        Status = add_partial_chunk(parity_offset + parity_length - (parity_length % full_chunk), parity_length % full_chunk,
                                   data + parity_offset - offset + parity_length - (parity_length % full_chunk));
        if (!NT_SUCCESS(Status))
            goto end;

        parity_length -= parity_length % full_chunk;
    }

    stripe_length = array_info.chunksize * 512;

    get_raid0_offset(offset, stripe_length, array_info.raid_disks - 1, &startoff, &startoffstripe);
    get_raid0_offset(offset + length - 1, stripe_length, array_info.raid_disks - 1, &endoff, &endoffstripe);

    start_chunk = offset / stripe_length;
    end_chunk = (offset + length - 1) / stripe_length;

    if (start_chunk == end_chunk) { // small write, on one device
        auto parity = get_parity_volume(offset);
        uint32_t disk_num = get_physical_stripe(startoffstripe, parity);

        auto c = child_list[disk_num];

        IoCopyCurrentIrpStackLocationToNext(Irp);

        auto IrpSp2 = IoGetNextIrpStackLocation(Irp);

        uint64_t start = (start_chunk / (array_info.raid_disks - 1)) * stripe_length;

        start += offset % stripe_length;
        start += c->disk_info.data_offset * 512;

        IrpSp2->FileObject = c->fileobj;
        IrpSp2->Parameters.Write.ByteOffset.QuadPart = start;

        *no_complete = true;

        return IoCallDriver(c->device, Irp);
    }

    if (skip_first != 0) {
        auto parity = get_parity_volume(offset);
        uint32_t disk_num = get_physical_stripe(startoffstripe, parity);
        first_bit.sc = child_list[disk_num];
        first_bit.Irp = IoAllocateIrp(first_bit.sc->device->StackSize, false);

        if (!first_bit.Irp) {
            ERR("IoAllocateIrp failed\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        auto IrpSp2 = IoGetNextIrpStackLocation(first_bit.Irp);
        IrpSp2->MajorFunction = IRP_MJ_WRITE;

        auto addr = MmGetMdlVirtualAddress(Irp->MdlAddress);

        first_bit.mdl = IoAllocateMdl(addr, skip_first, false, false, nullptr);
        if (!first_bit.mdl) {
            ERR("IoAllocateMdl failed\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        IoBuildPartialMdl(Irp->MdlAddress, first_bit.mdl, addr, skip_first);

        first_bit.Irp->MdlAddress = first_bit.mdl;

        uint64_t start = (start_chunk / (array_info.raid_disks - 1)) * stripe_length;

        start += offset % stripe_length;
        start += first_bit.sc->disk_info.data_offset * 512;

        IrpSp2->FileObject = first_bit.sc->fileobj;
        IrpSp2->Parameters.Write.Length = skip_first;
        IrpSp2->Parameters.Write.ByteOffset.QuadPart = start;

        first_bit.Irp->UserIosb = &first_bit.iosb;

        KeInitializeEvent(&first_bit.Event, NotificationEvent, false);
        first_bit.Irp->UserEvent = &first_bit.Event;

        IoSetCompletionRoutine(first_bit.Irp, io_completion, &first_bit, true, true, true);

        offset += skip_first;
        length -= skip_first;

        get_raid0_offset(offset, stripe_length, array_info.raid_disks - 1, &startoff, &startoffstripe);
    }

    ctxs = (io_context*)ExAllocatePoolWithTag(NonPagedPool, sizeof(io_context) * array_info.raid_disks, ALLOC_TAG);
    if (!ctxs) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    RtlZeroMemory(ctxs, sizeof(io_context) * array_info.raid_disks);

    pos = 0;
    while (pos < length) {
        auto parity = get_parity_volume(offset + pos);

        if (pos == 0) {
            uint32_t stripe = get_physical_stripe(startoffstripe, parity);

            ctxs[stripe].first = true;

            for (uint32_t i = startoffstripe; i < array_info.raid_disks - 1; i++) {
                if (i == startoffstripe) {
                    auto readlen = min(length, (uint32_t)(stripe_length - (startoff % stripe_length)));

                    ctxs[stripe].stripe_start = startoff;
                    ctxs[stripe].stripe_end = startoff + readlen;

                    pos += readlen;
                } else {
                    auto readlen = min(length - pos, (uint32_t)stripe_length);

                    ctxs[stripe].stripe_start = startoff - (startoff % stripe_length);
                    ctxs[stripe].stripe_end = ctxs[stripe].stripe_start + readlen;

                    pos += readlen;
                }

                if (pos == length)
                    break;

                if (asymmetric) {
                    stripe++;

                    if (stripe == parity)
                        stripe++;
                } else
                    stripe = (stripe + 1) % array_info.raid_disks;
            }

            for (uint32_t i = 0; i < startoffstripe; i++) {
                uint32_t stripe2 = get_physical_stripe(i, parity);

                ctxs[stripe2].stripe_start = ctxs[stripe2].stripe_end = startoff - (startoff % stripe_length) + stripe_length;
            }

            {
                uint64_t v = parity_offset / (array_info.raid_disks - 1);

                if (v % stripe_length != 0) {
                    v += stripe_length - (startoff % stripe_length);
                    ctxs[parity].stripe_start = ctxs[parity].stripe_end = v;
                } else {
                    ctxs[parity].stripe_start = v;
                    ctxs[parity].stripe_end = v + min(parity_length, stripe_length);
                }
            }

            if (length - pos > array_info.raid_disks * (array_info.raid_disks - 1) * stripe_length) {
                auto skip = (uint32_t)(((length - pos) / (array_info.raid_disks * (array_info.raid_disks - 1) * stripe_length)) - 1);

                for (uint32_t i = 0; i < array_info.raid_disks; i++) {
                    ctxs[i].stripe_end += skip * array_info.raid_disks * stripe_length;
                }

                pos += (uint32_t)(skip * (array_info.raid_disks - 1) * array_info.raid_disks * stripe_length);
            }
        } else if (length - pos >= stripe_length * (array_info.raid_disks - 1)) {
            for (uint32_t i = 0; i < array_info.raid_disks; i++) {
                ctxs[i].stripe_end += stripe_length;
            }

            pos += (uint32_t)(stripe_length * (array_info.raid_disks - 1));
        } else {
            uint32_t stripe = get_physical_stripe(0, parity);

            for (uint32_t i = 0; i < array_info.raid_disks - 1; i++) {
                if (endoffstripe == i) {
                    ctxs[stripe].stripe_end = endoff + 1;
                    break;
                } else if (endoffstripe > i)
                    ctxs[stripe].stripe_end = endoff - (endoff % stripe_length) + stripe_length;

                if (asymmetric) {
                    stripe++;

                    if (stripe == parity)
                        stripe++;
                } else
                    stripe = (stripe + 1) % array_info.raid_disks;
            }

            break;
        }
    }

    for (unsigned int i = 0; i < array_info.raid_disks; i++) {
        if (ctxs[i].stripe_end != ctxs[i].stripe_start) {
            ctxs[i].Irp = IoAllocateIrp(child_list[i]->device->StackSize, false);

            if (!ctxs[i].Irp) {
                ERR("IoAllocateIrp failed\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            auto IrpSp2 = IoGetNextIrpStackLocation(ctxs[i].Irp);
            IrpSp2->MajorFunction = IRP_MJ_WRITE;

            auto mdl_length = (ULONG)(ctxs[i].stripe_end - ctxs[i].stripe_start);

            if (ctxs[i].first)
                mdl_length += startoff % PAGE_SIZE;

            ctxs[i].mdl = IoAllocateMdl(nullptr, (ULONG)mdl_length, false, false, nullptr);
            if (!ctxs[i].mdl) {
                ERR("IoAllocateMdl failed\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            ctxs[i].mdl->MdlFlags |= MDL_PARTIAL;

            ctxs[i].Irp->MdlAddress = ctxs[i].mdl;

            IrpSp2->FileObject = child_list[i]->fileobj;
            IrpSp2->Parameters.Write.Length = (ULONG)(ctxs[i].stripe_end - ctxs[i].stripe_start);
            IrpSp2->Parameters.Write.ByteOffset.QuadPart = ctxs[i].stripe_start + (child_list[i]->disk_info.data_offset * 512);

            ctxs[i].Irp->UserIosb = &ctxs[i].iosb;

            KeInitializeEvent(&ctxs[i].Event, NotificationEvent, false);
            ctxs[i].Irp->UserEvent = &ctxs[i].Event;

            IoSetCompletionRoutine(ctxs[i].Irp, io_completion, &ctxs[i], true, true, true);
        } else
            ctxs[i].Status = STATUS_SUCCESS;
    }

    if (Irp->MdlAddress->ByteOffset != 0 || skip_first != 0) {
        tmpbuf = (uint8_t*)ExAllocatePoolWithTag(NonPagedPool, length, ALLOC_TAG);
        if (!tmpbuf) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        tmpmdl = IoAllocateMdl(tmpbuf, length, false, false, nullptr);
        if (!tmpmdl) {
            ERR("IoAllocateMdl failed\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }

        MmBuildMdlForNonPagedPool(tmpmdl);

        RtlCopyMemory(tmpbuf, (uint8_t*)data + skip_first, length);
    }

    {
        pos = 0;

        uint8_t* pp = nullptr;
        PFN_NUMBER* parity_pfns = nullptr;

        if (parity_length > 0) {
            parity_data = (uint8_t*)ExAllocatePoolWithTag(NonPagedPool, parity_length, ALLOC_TAG);
            if (!parity_data) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            parity_mdl = IoAllocateMdl(parity_data, parity_length, false, false, nullptr);
            if (!parity_mdl) {
                ERR("IoAllocateMdl failed\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }

            MmBuildMdlForNonPagedPool(parity_mdl);

            pp = parity_data;
            parity_pfns = MmGetMdlPfnArray(parity_mdl);
        }

        for (unsigned int i = 0; i < array_info.raid_disks; i++) {
            if (ctxs[i].mdl)
                ctxs[i].pfnp = ctxs[i].pfns = MmGetMdlPfnArray(ctxs[i].mdl);
        }

        auto addr = data;
        auto src_pfns = MmGetMdlPfnArray((tmpmdl ? tmpmdl : Irp->MdlAddress));

        while (pos < length) {
            auto parity = get_parity_volume(offset + pos);

            if (pos == 0 && offset != parity_offset) {
                uint32_t stripe = get_physical_stripe(startoffstripe, parity);

                for (uint32_t i = startoffstripe; i < array_info.raid_disks - 1; i++) {
                    uint32_t writelen, pages;

                    if (i == startoffstripe)
                        writelen = min(length, (uint32_t)(stripe_length - (startoff % stripe_length)));
                    else
                        writelen = min(length - pos, (uint32_t)stripe_length);

                    if (writelen % PAGE_SIZE != 0) {
                        pages = writelen / PAGE_SIZE;
                        pages++;
                    } else
                        pages = writelen / PAGE_SIZE;

                    RtlCopyMemory(ctxs[stripe].pfnp, src_pfns, sizeof(PFN_NUMBER) * pages);
                    src_pfns = &src_pfns[pages];
                    ctxs[stripe].pfnp = &ctxs[stripe].pfnp[pages];

                    pos += writelen;
                    addr += writelen;

                    if (pos == length)
                        break;

                    if (asymmetric) {
                        stripe++;

                        if (stripe == parity)
                            stripe++;
                    } else
                        stripe = (stripe + 1) % array_info.raid_disks;
                }
            } else if (length - pos >= stripe_length * (array_info.raid_disks - 1)) {
                uint32_t stripe = get_physical_stripe(0, parity);
                uint32_t pages = stripe_length / PAGE_SIZE;
                bool first = true;

                for (uint32_t i = 0; i < array_info.raid_disks - 1; i++) {
                    if (first) {
                        RtlCopyMemory(pp, addr, stripe_length);
                        first = false;
                    } else
                        do_xor(pp, addr, stripe_length);

                    pos += stripe_length;
                    addr += stripe_length;

                    RtlCopyMemory(ctxs[stripe].pfnp, src_pfns, sizeof(PFN_NUMBER) * pages);
                    src_pfns = &src_pfns[pages];
                    ctxs[stripe].pfnp = &ctxs[stripe].pfnp[pages];

                    if (asymmetric) {
                        stripe++;

                        if (stripe == parity)
                            stripe++;
                    } else
                        stripe = (stripe + 1) % array_info.raid_disks;
                }

                pp = &pp[stripe_length];

                RtlCopyMemory(ctxs[parity].pfnp, parity_pfns, sizeof(PFN_NUMBER) * pages);
                parity_pfns = &parity_pfns[pages];
                ctxs[parity].pfnp = &ctxs[parity].pfnp[pages];
            } else {
                uint32_t stripe = get_physical_stripe(0, parity);

                for (uint32_t i = 0; i < array_info.raid_disks - 1; i++) {
                    uint32_t writelen = min(length - pos, (uint32_t)stripe_length);
                    uint32_t pages = writelen / PAGE_SIZE;

                    if (writelen % PAGE_SIZE != 0)
                        pages++;

                    RtlCopyMemory(ctxs[stripe].pfnp, src_pfns, sizeof(PFN_NUMBER) * pages);
                    src_pfns = &src_pfns[pages];
                    ctxs[stripe].pfnp = &ctxs[stripe].pfnp[pages];

                    pos += writelen;

                    if (pos == length)
                        break;

                    if (asymmetric) {
                        stripe++;

                        if (stripe == parity)
                            stripe++;
                    } else
                        stripe = (stripe + 1) % array_info.raid_disks;
                }
            }
        }
    }

    for (unsigned int i = 0; i < array_info.raid_disks; i++) {
        if (ctxs[i].Irp) {
            ctxs[i].Status = IoCallDriver(child_list[i]->device, ctxs[i].Irp);
            if (!NT_SUCCESS(ctxs[i].Status))
                ERR("IoCallDriver returned %08x\n", ctxs[i].Status);
        }
    }

    if (skip_first != 0) {
        first_bit.Status = IoCallDriver(first_bit.sc->device, first_bit.Irp);
        if (!NT_SUCCESS(first_bit.Status))
            ERR("IoCallDriver returned %08x\n", first_bit.Status);
    }

    Status = STATUS_SUCCESS;

    for (unsigned int i = 0; i < array_info.raid_disks; i++) {
        if (ctxs[i].Status == STATUS_PENDING) {
            KeWaitForSingleObject(&ctxs[i].Event, Executive, KernelMode, false, nullptr);
            ctxs[i].Status = ctxs[i].iosb.Status;
        }

        if (!NT_SUCCESS(ctxs[i].Status))
            Status = ctxs[i].Status;
    }

    if (skip_first != 0) {
        if (first_bit.Status == STATUS_PENDING) {
            KeWaitForSingleObject(&first_bit.Event, Executive, KernelMode, false, nullptr);
            first_bit.Status = first_bit.iosb.Status;
        }

        if (!NT_SUCCESS(first_bit.Status))
            Status = first_bit.Status;
    }

#ifdef DEBUG_PARANOID
    if (parity_length != 0)
        paranoid_raid5_check(parity_offset, parity_length);
#endif

end:
    if (!mdl_locked)
        MmUnlockPages(Irp->MdlAddress);

    if (parity_mdl)
        IoFreeMdl(parity_mdl);

    if (parity_data)
        ExFreePool(parity_data);

    if (ctxs) {
        for (unsigned int i = 0; i < array_info.raid_disks; i++) {
            if (ctxs[i].mdl)
                IoFreeMdl(ctxs[i].mdl);

            if (ctxs[i].va)
                ExFreePool(ctxs[i].va);

            if (ctxs[i].Irp)
                IoFreeIrp(ctxs[i].Irp);
        }

        ExFreePool(ctxs);
    }

    if (tmpmdl)
        IoFreeMdl(tmpmdl);

    if (tmpbuf)
        ExFreePool(tmpbuf);

    return Status;
}

NTSTATUS set_pdo::flush_partial_chunk_raid45(partial_chunk* pc, RTL_BITMAP* valid_bmp) {
    NTSTATUS Status;
    klist<io_context> ctxs;
    ULONG index;
    auto runlength = RtlFindFirstRunClear(valid_bmp, &index);
    auto parity = get_parity_volume(pc->offset);
    auto parity_dev = child_list[parity];
    uint32_t data_disks = array_info.raid_disks - 1;
    uint32_t chunk_size = array_info.chunksize * 512;

    while (runlength != 0) {
        for (uint32_t i = 1; i < data_disks; i++) {
            do_xor(pc->data + (index * 512), pc->data + (i * chunk_size) + (index * 512), runlength * 512);
        }

        uint64_t stripe_start = (pc->offset / data_disks) + (index * 512) + (parity_dev->disk_info.data_offset * 512);

        ctxs.emplace_back_np(parity_dev, stripe_start, stripe_start + (runlength * 512));

        auto last = &ctxs.back();

        if (!NT_SUCCESS(last->Status)) {
            ERR("io_context constructor returned %08x\n", last->Status);
            return last->Status;
        }

        last->va2 = pc->data + (index * 512);

        runlength = RtlFindNextForwardRunClear(valid_bmp, index + runlength, &index);
    }

    if (!ctxs.empty()) {
        LIST_ENTRY* le = ctxs.list.Flink;
        while (le != &ctxs.list) {
            auto& ctx = ctxs.entry(le);

            auto IrpSp = IoGetNextIrpStackLocation(ctx.Irp);
            IrpSp->MajorFunction = IRP_MJ_WRITE;

            ctx.mdl = IoAllocateMdl(ctx.va2, (ULONG)(ctx.stripe_end - ctx.stripe_start), false, false, nullptr);
            if (!ctx.mdl) {
                ERR("IoAllocateMdl failed\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            MmBuildMdlForNonPagedPool(ctx.mdl);

            ctx.Irp->MdlAddress = ctx.mdl;

            IrpSp->FileObject = ctx.sc->fileobj;
            IrpSp->Parameters.Write.ByteOffset.QuadPart = ctx.stripe_start;
            IrpSp->Parameters.Write.Length = (ULONG)(ctx.stripe_end - ctx.stripe_start);

            ctx.Status = IoCallDriver(ctx.sc->device, ctx.Irp);

            le = le->Flink;
        }

        Status = STATUS_SUCCESS;

        le = ctxs.list.Flink;
        while (le != &ctxs.list) {
            auto& ctx = ctxs.entry(le);

            if (ctx.Status == STATUS_PENDING) {
                KeWaitForSingleObject(&ctx.Event, Executive, KernelMode, false, nullptr);
                ctx.Status = ctx.iosb.Status;
            }

            if (!NT_SUCCESS(ctx.Status)) {
                ERR("writing returned %08x\n", ctx.Status);
                Status = ctx.Status;
            }

            le = le->Flink;
        }

        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}
