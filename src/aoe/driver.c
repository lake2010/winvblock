/**
 * Copyright (C) 2009-2011, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 * Copyright 2006-2008, V.
 * For WinAoE contact information, see http://winaoe.org/
 *
 * This file is part of WinVBlock, derived from WinAoE.
 *
 * WinVBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinVBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinVBlock.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * AoE specifics.
 */

#include <stdio.h>
#include <ntddk.h>
#include <scsi.h>

#include "portable.h"
#include "winvblock.h"
#include "wv_stdlib.h"
#include "wv_string.h"
#include "irp.h"
#include "driver.h"
#include "bus.h"
#include "device.h"
#include "dummy.h"
#include "disk.h"
#include "mount.h"
#include "aoe.h"
#include "registry.h"
#include "protocol.h"
#include "debug.h"

#define AOEPROTOCOLVER 1

/* From aoe/bus.c */
extern WVL_S_BUS_T AoeBusMain;
extern NTSTATUS AoeBusCreate(IN PDRIVER_OBJECT);
extern VOID AoeBusFree(void);
extern NTSTATUS STDCALL AoeBusDevCtl(IN PIRP, IN ULONG POINTER_ALIGNMENT);
extern NTSTATUS STDCALL AoeBusAttachFdo(
    IN PDRIVER_OBJECT,
    IN PDEVICE_OBJECT
  );
extern BOOLEAN STDCALL AoeBusAddDev(IN OUT WV_SP_DEV_T);
/* From aoe/registry.c */
extern BOOLEAN STDCALL AoeRegSetup(OUT PNTSTATUS);

/* Forward declarations. */
struct AOE_DISK_;
static VOID STDCALL AoeThread_(IN PVOID);
static VOID AoeProcessAbft_(void);
static struct AOE_DISK_ * AoeDiskCreatePdo_(void);
static WV_F_DEV_FREE AoeDiskFree_;
static WV_F_DISK_IO AoeDiskIo_;
static WV_F_DISK_MAX_XFER_LEN AoeDiskMaxXferLen_;
static BOOLEAN STDCALL AoeDiskInit_(struct AOE_DISK_ *);
static WV_F_DISK_CLOSE AoeDiskClose_;
static WVL_F_DISK_UNIT_NUM AoeDiskUnitNum_;
static DRIVER_DISPATCH AoeIrpNotSupported_;
static __drv_dispatchType(IRP_MJ_POWER) DRIVER_DISPATCH AoeIrpPower_;
static
  __drv_dispatchType(IRP_MJ_CREATE)
  __drv_dispatchType(IRP_MJ_CLOSE)
  DRIVER_DISPATCH AoeIrpCreateClose_;
static __drv_dispatchType(IRP_MJ_SYSTEM_CONTROL)
  DRIVER_DISPATCH AoeIrpSysCtl_;
static __drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
  DRIVER_DISPATCH AoeIrpDevCtl_;
static __drv_dispatchType(IRP_MJ_SCSI) DRIVER_DISPATCH AoeIrpScsi_;
static __drv_dispatchType(IRP_MJ_PNP) DRIVER_DISPATCH AoeIrpPnp_;
static DRIVER_UNLOAD AoeUnload_;

/** Tag types. */
typedef enum AOE_TAG_TYPE_ {
    AoeTagTypeIo_,
    AoeTagTypeSearchDrive_,
    AoeTagTypes_
  } AOE_E_TAG_TYPE_, * AOE_EP_TAG_TYPE_;

#ifdef _MSC_VER
#  pragma pack(1)
#endif
/** AoE packet. */
struct AOE_PACKET_ {
    UCHAR ReservedFlag:2;
    UCHAR ErrorFlag:1;
    UCHAR ResponseFlag:1;
    UCHAR Ver:4;
    UCHAR Error;
    UINT16 Major;
    UCHAR Minor;
    UCHAR Command;
    UINT32 Tag;

    UCHAR WriteAFlag:1;
    UCHAR AsyncAFlag:1;
    UCHAR Reserved1AFlag:2;
    UCHAR DeviceHeadAFlag:1;
    UCHAR Reserved2AFlag:1;
    UCHAR ExtendedAFlag:1;
    UCHAR Reserved3AFlag:1;
    union {
        UCHAR Err;
        UCHAR Feature;
      };
    UCHAR Count;
    union {
        UCHAR Cmd;
        UCHAR Status;
      };

    UCHAR Lba0;
    UCHAR Lba1;
    UCHAR Lba2;
    UCHAR Lba3;
    UCHAR Lba4;
    UCHAR Lba5;
    UINT16 Reserved;

    UCHAR Data[];
  } __attribute__((__packed__));
typedef struct AOE_PACKET_ AOE_S_PACKET_, * AOE_SP_PACKET_;
#ifdef _MSC_VER
#  pragma pack()
#endif

/** An I/O request. */
typedef struct AOE_IO_REQ_ {
    WVL_E_DISK_IO_MODE Mode;
    UINT32 SectorCount;
    PUCHAR Buffer;
    PIRP Irp;
    UINT32 TagCount;
    UINT32 TotalTags;
  } AOE_S_IO_REQ_, * AOE_SP_IO_REQ_;

/** A work item "tag". */
typedef struct AOE_WORK_TAG_ {
    AOE_E_TAG_TYPE_ type;
    WV_SP_DEV_T device;
    AOE_SP_IO_REQ_ request_ptr;
    UINT32 Id;
    AOE_SP_PACKET_ packet_data;
    UINT32 PacketSize;
    LARGE_INTEGER FirstSendTime;
    LARGE_INTEGER SendTime;
    UINT32 BufferOffset;
    UINT32 SectorCount;
    struct AOE_WORK_TAG_ * next;
    struct AOE_WORK_TAG_ * previous;
  } AOE_S_WORK_TAG_, * AOE_SP_WORK_TAG_;

/** A disk search. */
typedef struct AOE_DISK_SEARCH_ {
    WV_SP_DEV_T device;
    AOE_SP_WORK_TAG_ tag;
    struct AOE_DISK_SEARCH_ * next;
  } AOE_S_DISK_SEARCH_, * AOE_SP_DISK_SEARCH_;

typedef enum AOE_SEARCH_STATE_ {
    AoeSearchStateSearchNic_,
    AoeSearchStateGetSize_,
    AoeSearchStateGettingSize_,
    AoeSearchStateGetGeometry_,
    AoeSearchStateGettingGeometry_,
    AoeSearchStateGetMaxSectsPerPacket_,
    AoeSearchStateGettingMaxSectsPerPacket_,
    AoeSearchStateDone_,
    AoeSearchStates
  } AOE_E_SEARCH_STATE_, * AOE_EP_SEARCH_STATE_;

/** The AoE disk type. */
typedef struct AOE_DISK_ {
    WV_S_DEV_EXT DevExt[1];
    WV_S_DISK_T disk[1];
    UINT32 MTU;
    UCHAR ClientMac[6];
    UCHAR ServerMac[6];
    UINT32 Major;
    UINT32 Minor;
    UINT32 MaxSectorsPerPacket;
    UINT32 Timeout;
    AOE_E_SEARCH_STATE_ search_state;
    WV_FP_DEV_FREE prev_free;
    LIST_ENTRY tracking;
  } AOE_S_DISK_, * AOE_SP_DISK_;

typedef struct AOE_TARGET_LIST_ {
    AOE_S_MOUNT_TARGET Target;
    struct AOE_TARGET_LIST_ * next;
  } AOE_S_TARGET_LIST_, * AOE_SP_TARGET_LIST_;

/** Private globals. */
static PDRIVER_OBJECT AoeDriverObj_ = NULL;
static AOE_SP_TARGET_LIST_ AoeTargetList_ = NULL;
static KSPIN_LOCK AoeTargetListLock_;
static BOOLEAN AoeStop_ = FALSE;
static KSPIN_LOCK AoeLock_;
static KEVENT AoeSignal_;
static AOE_SP_WORK_TAG_ AoeTagListFirst_ = NULL;
static AOE_SP_WORK_TAG_ AoeTagListLast_ = NULL;
static AOE_SP_WORK_TAG_ AoeProbeTag_ = NULL;
static AOE_SP_DISK_SEARCH_ AoeDiskSearchList_ = NULL;
static LONG AoePendingTags_ = 0;
static HANDLE AoeThreadHandle_;
static PETHREAD AoeThreadObj_ = NULL;
static BOOLEAN AoeStarted_ = FALSE;

/* Yield a pointer to the AoE disk. */
static AOE_SP_DISK_ AoeDiskFromDev_(WV_SP_DEV_T dev_ptr) {
    return disk__get_ptr(dev_ptr)->ext;
  }

typedef enum AOE_CLEANUP_ {
    AoeCleanupReg_,
    AoeCleanupProtocol_,
    AoeCleanupProbeTag_,
    AoeCleanupProbeTagPacket_,
    AoeCleanupBus_,
    AoeCleanupThread_,
    AoeCleanupThreadRef_,
    AoeCleanupAll_
  } AOE_E_CLEANUP_, * AOE_EP_CLEANUP_;
  
static VOID AoeCleanup_(AOE_E_CLEANUP_ cleanup) {
    switch (cleanup) {
        default:
          DBG("Unknown cleanup request!\n");
        case AoeCleanupAll_:

        if (AoeThreadHandle_) {
            AoeStop_ = TRUE;
            KeSetEvent(&AoeSignal_, 0, FALSE);
            KeWaitForSingleObject(
                AoeThreadObj_,
                Executive,
                KernelMode,
                FALSE,
                NULL
              );
            ObDereferenceObject(AoeThreadObj_);
          }
        case AoeCleanupThreadRef_:

        /* Duplication from above, but is hopefully harmless. */
        if (AoeThreadHandle_) {
            AoeStop_ = TRUE;
            KeSetEvent(&AoeSignal_, 0, FALSE);
            ZwClose(AoeThreadHandle_);
          }
        case AoeCleanupThread_:

        AoeBusFree();
        case AoeCleanupBus_:

        wv_free(AoeProbeTag_->packet_data);
        case AoeCleanupProbeTagPacket_:
    
        wv_free(AoeProbeTag_);
        case AoeCleanupProbeTag_:
    
        Protocol_Stop();
        case AoeCleanupProtocol_:

        case AoeCleanupReg_:

        DBG("Done.\n");
      }
    return;
  }

/**
 * Start AoE operations.
 *
 * @v DriverObject      Unique to this driver.
 * @v RegistryPath      Unique to this driver.
 * @ret NTSTATUS        The status of driver startup.
 */
NTSTATUS STDCALL DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
  ) {
    NTSTATUS status;
    OBJECT_ATTRIBUTES obj_attrs;
    UINT i;

    DBG("Entry\n");

    if (AoeStarted_)
      return STATUS_SUCCESS;

    /* Note our driver object. */
    AoeDriverObj_ = DriverObject;

    /* Setup the Registry. */
    AoeRegSetup(&status);
    if (!NT_SUCCESS(status)) {
        DBG("Could not update Registry!\n");
        AoeCleanup_(AoeCleanupReg_);
        return status;
      } else {
        DBG("Registry updated\n");
      }

    /* Start up the protocol. */
    status = Protocol_Start();
    if (!NT_SUCCESS(status)) {
        DBG("Protocol startup failure!\n");
        AoeCleanup_(AoeCleanupProtocol_);
        return status;
      }

    /* Allocate and zero-fill the global probe tag. */
    AoeProbeTag_ = wv_mallocz(sizeof *AoeProbeTag_);
    if (AoeProbeTag_ == NULL) {
        DBG("Couldn't allocate probe tag; bye!\n");
        AoeCleanup_(AoeCleanupProbeTag_);
        return STATUS_INSUFFICIENT_RESOURCES;
      }

    /* Set up the probe tag's AoE packet reference. */
    AoeProbeTag_->PacketSize = sizeof (AOE_S_PACKET_);

    /* Allocate and zero-fill the probe tag's packet reference. */
    AoeProbeTag_->packet_data = wv_mallocz(AoeProbeTag_->PacketSize);
    if (AoeProbeTag_->packet_data == NULL) {
        DBG("Couldn't allocate AoeProbeTag_->packet_data\n");
        AoeCleanup_(AoeCleanupProbeTagPacket_);
        return STATUS_INSUFFICIENT_RESOURCES;
      }

    AoeProbeTag_->SendTime.QuadPart = 0LL;

    /* Initialize the probe tag's AoE packet. */
    AoeProbeTag_->packet_data->Ver = AOEPROTOCOLVER;
    AoeProbeTag_->packet_data->Major = htons((UINT16) -1);
    AoeProbeTag_->packet_data->Minor = (UCHAR) -1;
    AoeProbeTag_->packet_data->Cmd = 0xec;           /* IDENTIFY DEVICE */
    AoeProbeTag_->packet_data->Count = 1;

    /* Initialize global target-list spinlock. */
    KeInitializeSpinLock(&AoeTargetListLock_);

    /* Initialize global spin-lock and global thread signal event. */
    KeInitializeSpinLock(&AoeLock_);
    KeInitializeEvent(&AoeSignal_, SynchronizationEvent, FALSE);

    /* Establish the AoE bus. */
    status = AoeBusCreate(DriverObject);
    if (!NT_SUCCESS(status)) {
        DBG("Unable to create AoE bus!\n");
        AoeCleanup_(AoeCleanupBus_);
        return status;
      }

    /* Initialize object attributes for the global thread. */
    InitializeObjectAttributes(
        &obj_attrs,
        NULL,
        OBJ_KERNEL_HANDLE,
        NULL,
        NULL
      );

    /* Create the global thread. */
    status = PsCreateSystemThread(
        &AoeThreadHandle_,
        THREAD_ALL_ACCESS,
        &obj_attrs,
        NULL,
        NULL,
        AoeThread_,
        NULL
      );
    if (!NT_SUCCESS(status)) {
        WvlError("PsCreateSystemThread", status);
        AoeCleanup_(AoeCleanupThread_);
        return status;
      }

    /* Increment reference count on thread object. */
    status = ObReferenceObjectByHandle(
        AoeThreadHandle_,
        THREAD_ALL_ACCESS,
        *PsThreadType,
        KernelMode,
        &AoeThreadObj_,
        NULL
      );
    if (!NT_SUCCESS(status)) {
        WvlError("ObReferenceObjectByHandle", status);
        AoeCleanup_(AoeCleanupThreadRef_);
        return status;
      }

    /* Initialize DriverObject for IRPs. */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
      DriverObject->MajorFunction[i] = AoeIrpNotSupported_;
    DriverObject->MajorFunction[IRP_MJ_PNP] = AoeIrpPnp_;
    DriverObject->MajorFunction[IRP_MJ_POWER] = AoeIrpPower_;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = AoeIrpCreateClose_;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = AoeIrpCreateClose_;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = AoeIrpSysCtl_;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AoeIrpDevCtl_;
    DriverObject->MajorFunction[IRP_MJ_SCSI] = AoeIrpScsi_;
    /* Set the driver Unload callback. */
    DriverObject->DriverUnload = AoeUnload_;
    /* Set the driver AddDevice callback. */
    DriverObject->DriverExtension->AddDevice = AoeBusAttachFdo;
    AoeStarted_ = TRUE;

    AoeProcessAbft_();
    DBG("Exit\n");
    return status;
  }

/**
 * Stop AoE operations.
 *
 * @v DriverObject      Unique to this driver.
 */
static VOID STDCALL AoeUnload_(IN PDRIVER_OBJECT DriverObject) {
    NTSTATUS Status;
    AOE_SP_DISK_SEARCH_ disk_searcher, previous_disk_searcher;
    AOE_SP_WORK_TAG_ tag;
    KIRQL Irql, Irql2;
    AOE_SP_TARGET_LIST_ Walker, Next;

    DBG("Entry\n");
    /* If we're not already started, there's nothing to do. */
    if (!AoeStarted_)
      return;
    /* General cleanup. */
    AoeCleanup_(AoeCleanupAll_);
    /* Free the target list. */
    KeAcquireSpinLock(&AoeTargetListLock_, &Irql2);
    Walker = AoeTargetList_;
    while (Walker != NULL) {
        Next = Walker->next;
        wv_free(Walker);
        Walker = Next;
      }
    KeReleaseSpinLock(&AoeTargetListLock_, Irql2);

    /* Wait until we have the global spin-lock. */
    KeAcquireSpinLock(&AoeLock_, &Irql);

    /* Free disk searches in the global disk search list. */
    disk_searcher = AoeDiskSearchList_;
    while (disk_searcher != NULL) {
        KeSetEvent(
            &(disk__get_ptr(disk_searcher->device)->SearchEvent),
            0,
            FALSE
          );
        previous_disk_searcher = disk_searcher;
        disk_searcher = disk_searcher->next;
        wv_free(previous_disk_searcher);
      }

    /* Cancel and free all tags in the global tag list. */
    tag = AoeTagListFirst_;
    while (tag != NULL) {
        if (tag->request_ptr != NULL && --tag->request_ptr->TagCount == 0) {
            tag->request_ptr->Irp->IoStatus.Information = 0;
            tag->request_ptr->Irp->IoStatus.Status = STATUS_CANCELLED;
            IoCompleteRequest(tag->request_ptr->Irp, IO_NO_INCREMENT);
            wv_free(tag->request_ptr);
          }
        if (tag->next == NULL) {
            wv_free(tag->packet_data);
            wv_free(tag);
            tag = NULL;
          } else {
            tag = tag->next;
            wv_free(tag->previous->packet_data);
            wv_free(tag->previous);
          }
      }
    AoeTagListFirst_ = NULL;
    AoeTagListLast_ = NULL;

    /* Release the global spin-lock. */
    KeReleaseSpinLock(&AoeLock_, Irql);
    AoeStarted_ = FALSE;
    DBG("Unloaded.\n");
  }

/**
 * Search for disk parameters.
 *
 * @v aoe_disk          The disk to initialize (for AoE, match).
 * @ret BOOLEAN         See below.
 *
 * Returns TRUE if the disk could be matched, FALSE otherwise.
 */
static BOOLEAN STDCALL AoeDiskInit_(IN AOE_SP_DISK_ aoe_disk) {
    AOE_SP_DISK_SEARCH_
      disk_searcher, disk_search_walker, previous_disk_searcher;
    LARGE_INTEGER Timeout, CurrentTime;
    AOE_SP_WORK_TAG_ tag, tag_walker;
    KIRQL Irql, InnerIrql;
    LARGE_INTEGER MaxSectorsPerPacketSendTime;
    UINT32 MTU;
    WV_SP_DISK_T disk_ptr = aoe_disk->disk;

    /* Allocate our disk search. */
    if ((disk_searcher = wv_malloc(sizeof *disk_searcher)) == NULL) {
        DBG("Couldn't allocate for disk_searcher; bye!\n");
        return FALSE;
      }

    /* Initialize the disk search. */
    disk_searcher->device = disk_ptr->Dev;
    disk_searcher->next = NULL;
    aoe_disk->search_state = AoeSearchStateSearchNic_;
    KeResetEvent(&disk_ptr->SearchEvent);

    /* Wait until we have the global spin-lock. */
    KeAcquireSpinLock(&AoeLock_, &Irql);

    /* Add our disk search to the global list of disk searches. */
    if (AoeDiskSearchList_ == NULL) {
        AoeDiskSearchList_ = disk_searcher;
      } else {
        disk_search_walker = AoeDiskSearchList_;
        while (disk_search_walker->next)
          disk_search_walker = disk_search_walker->next;
        disk_search_walker->next = disk_searcher;
      }

    /* Release the global spin-lock. */
    KeReleaseSpinLock(&AoeLock_, Irql);

    /* We go through all the states until the disk is ready for use. */
    while (TRUE) {
        /* Wait for our device's extension's search to be signalled.
         *
         * TODO: Make the below value a #defined constant:
         * 500.000 * 100ns = 50.000.000 ns = 50ms 
         */
        Timeout.QuadPart = -500000LL;
        KeWaitForSingleObject(
            &disk_ptr->SearchEvent,
            Executive,
            KernelMode,
            FALSE,
            &Timeout
          );
        if (AoeStop_) {
            DBG("AoE is shutting down; bye!\n");
            return FALSE;
          }

        /* Wait until we have the device extension's spin-lock. */
        KeAcquireSpinLock(&disk_ptr->SpinLock, &Irql);

        if (aoe_disk->search_state == AoeSearchStateSearchNic_) {
            if (!Protocol_SearchNIC(aoe_disk->ClientMac)) {
                KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
                continue;
              } else {
                /* We found the adapter to use, get MTU next. */
                aoe_disk->MTU = Protocol_GetMTU(aoe_disk->ClientMac);
                aoe_disk->search_state = AoeSearchStateGetSize_;
              }
          }

        if (aoe_disk->search_state == AoeSearchStateGettingSize_) {
            /* Still getting the disk's size. */
            KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
            continue;
          }
        if (aoe_disk->search_state == AoeSearchStateGettingGeometry_) {
            /* Still getting the disk's geometry. */
            KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
            continue;
          }
        if (aoe_disk->search_state ==
          AoeSearchStateGettingMaxSectsPerPacket_) {
            KeQuerySystemTime(&CurrentTime);
            /*
             * TODO: Make the below value a #defined constant:
             * 2.500.000 * 100ns = 250.000.000 ns = 250ms 
             */
            if (
                CurrentTime.QuadPart >
                MaxSectorsPerPacketSendTime.QuadPart + 2500000LL
              ) {
                DBG(
                    "No reply after 250ms for MaxSectorsPerPacket %d, "
                      "giving up\n",
                    aoe_disk->MaxSectorsPerPacket
                  );
                aoe_disk->MaxSectorsPerPacket--;
                aoe_disk->search_state = AoeSearchStateDone_;
              } else {
                /* Still getting the maximum sectors per packet count. */
                KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
                continue;
              }
          }

        if (aoe_disk->search_state == AoeSearchStateDone_) {
            /* We've finished the disk search; perform clean-up. */
            KeAcquireSpinLock(&AoeLock_, &InnerIrql);

            /* Tag clean-up: Find out if our tag is in the global tag list. */
            tag_walker = AoeTagListFirst_;
            while (tag_walker != NULL && tag_walker != tag)
              tag_walker = tag_walker->next;
            if (tag_walker != NULL) {
                /*
                 * We found it.  If it's at the beginning of the list, adjust
                 * the list to point the the next tag.
                 */
                if (tag->previous == NULL)
                  AoeTagListFirst_ = tag->next;
                  else
                  /* Remove our tag from the list. */
                  tag->previous->next = tag->next;
                /*
                 * If we 're at the end of the list, adjust the list's end to
                 * point to the penultimate tag.
                 */
                if (tag->next == NULL)
                  AoeTagListLast_ = tag->previous;
                  else
                  /* Remove our tag from the list. */
                  tag->next->previous = tag->previous;
                AoePendingTags_--;
                if (AoePendingTags_ < 0)
                  DBG("AoePendingTags_ < 0!!\n");
                /* Free our tag and its AoE packet. */
                wv_free(tag->packet_data);
                wv_free(tag);
              } /* found tag. */

            /* Disk search clean-up. */
            if (AoeDiskSearchList_ == NULL) {
                DBG("AoeDiskSearchList_ == NULL!!\n");
              } else {
                /* Find our disk search in the global list of disk searches. */
                disk_search_walker = AoeDiskSearchList_;
                while (
                    disk_search_walker &&
                    disk_search_walker->device != disk_ptr->Dev
                  ) {
                    previous_disk_searcher = disk_search_walker;
                    disk_search_walker = disk_search_walker->next;
                  }
                if (disk_search_walker) {
                    /*
                     * We found our disk search.  If it's the first one in
                     * the list, adjust the list and remove it
                     */
                    if (disk_search_walker == AoeDiskSearchList_)
                      AoeDiskSearchList_ = disk_search_walker->next;
                      else
                      /* Just remove it. */
                      previous_disk_searcher->next = disk_search_walker->next;
                    /* Free our disk search. */
                    wv_free(disk_search_walker);
                  } else {
                    DBG("Disk not found in AoeDiskSearchList_!!\n");
                  }
              } /* if AoeDiskSearchList_ */

            /* Release global and device extension spin-locks. */
            KeReleaseSpinLock(&AoeLock_, InnerIrql);
            KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);

            DBG(
                "Disk size: %I64uM cylinders: %I64u heads: %u"
                  "sectors: %u sectors per packet: %u\n",
                disk_ptr->LBADiskSize / 2048,
                disk_ptr->Cylinders,
                disk_ptr->Heads,
                disk_ptr->Sectors,
                aoe_disk->MaxSectorsPerPacket
              );
            return TRUE;
          } /* if AoeSearchStateDone */

        #if 0
        if (aoe_disk->search_state == AoeSearchStateDone_)
        #endif
        /* Establish our tag. */
        if ((tag = wv_mallocz(sizeof *tag)) == NULL) {
            DBG("Couldn't allocate tag\n");
            KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
            /* Maybe next time around. */
            continue;
          }
        tag->type = AoeTagTypeSearchDrive_;
        tag->device = disk_ptr->Dev;

        /* Establish our tag's AoE packet. */
        tag->PacketSize = sizeof (AOE_S_PACKET_);
        if ((tag->packet_data = wv_mallocz(tag->PacketSize)) == NULL) {
            DBG("Couldn't allocate tag->packet_data\n");
            wv_free(tag);
            tag = NULL;
            KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
            /* Maybe next time around. */
            continue;
          }
        tag->packet_data->Ver = AOEPROTOCOLVER;
        tag->packet_data->Major = htons((UINT16) aoe_disk->Major);
        tag->packet_data->Minor = (UCHAR) aoe_disk->Minor;
        tag->packet_data->ExtendedAFlag = TRUE;

        /* Initialize the packet appropriately based on our current phase. */
        switch (aoe_disk->search_state) {
            case AoeSearchStateGetSize_:
              /* TODO: Make the below value into a #defined constant. */
              tag->packet_data->Cmd = 0xec;  /* IDENTIFY DEVICE */
              tag->packet_data->Count = 1;
              aoe_disk->search_state = AoeSearchStateGettingSize_;
              break;

            case AoeSearchStateGetGeometry_:
              /* TODO: Make the below value into a #defined constant. */
              tag->packet_data->Cmd = 0x24;  /* READ SECTOR */
              tag->packet_data->Count = 1;
              aoe_disk->search_state = AoeSearchStateGettingGeometry_;
              break;

            case AoeSearchStateGetMaxSectsPerPacket_:
              /* TODO: Make the below value into a #defined constant. */
              tag->packet_data->Cmd = 0x24;  /* READ SECTOR */
              tag->packet_data->Count = (UCHAR) (
                  ++aoe_disk->MaxSectorsPerPacket
                );
              KeQuerySystemTime(&MaxSectorsPerPacketSendTime);
              aoe_disk->search_state =
                AoeSearchStateGettingMaxSectsPerPacket_;
              /* TODO: Make the below value into a #defined constant. */
              aoe_disk->Timeout = 200000;
              break;

            default:
              DBG("Undefined search_state!!\n");
              wv_free(tag->packet_data);
              wv_free(tag);
              /* TODO: Do we need to nullify tag here? */
              KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
              continue;
              break;
          }

        /* Enqueue our tag. */
        tag->next = NULL;
        KeAcquireSpinLock(&AoeLock_, &InnerIrql);
        if (AoeTagListFirst_ == NULL) {
            AoeTagListFirst_ = tag;
            tag->previous = NULL;
          } else {
            AoeTagListLast_->next = tag;
            tag->previous = AoeTagListLast_;
          }
        AoeTagListLast_ = tag;
        KeReleaseSpinLock(&AoeLock_, InnerIrql);
        KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
      } /* while TRUE */
  }

static NTSTATUS STDCALL AoeDiskIo_(
    IN WV_SP_DEV_T dev_ptr,
    IN WVL_E_DISK_IO_MODE mode,
    IN LONGLONG start_sector,
    IN UINT32 sector_count,
    IN PUCHAR buffer,
    IN PIRP irp
  ) {
    AOE_SP_IO_REQ_ request_ptr;
    AOE_SP_WORK_TAG_ tag, new_tag_list = NULL, previous_tag = NULL;
    KIRQL Irql;
    UINT32 i;
    PHYSICAL_ADDRESS PhysicalAddress;
    PUCHAR PhysicalMemory;
    WV_SP_DISK_T disk_ptr;
    AOE_SP_DISK_ aoe_disk_ptr;

    /* Establish pointers to the disk and AoE disk. */
    disk_ptr = disk__get_ptr(dev_ptr);
    aoe_disk_ptr = AoeDiskFromDev_(dev_ptr);

    if (AoeStop_) {
        /* Shutting down AoE; we can't service this request. */
        irp->IoStatus.Information = 0;
        irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
      }

    if (sector_count < 1) {
        /* A silly request. */
        DBG("sector_count < 1; cancelling\n");
        irp->IoStatus.Information = 0;
        irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
      }

    /* Allocate and zero-fill our request. */
    if ((request_ptr = wv_mallocz(sizeof *request_ptr)) == NULL) {
        DBG("Couldn't allocate for reques_ptr; bye!\n");
        irp->IoStatus.Information = 0;
        irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
      }

    /* Initialize the request. */
    request_ptr->Mode = mode;
    request_ptr->SectorCount = sector_count;
    request_ptr->Buffer = buffer;
    request_ptr->Irp = irp;
    request_ptr->TagCount = 0;

    /* Split the requested sectors into packets in tags. */
    for (i = 0; i < sector_count; i += aoe_disk_ptr->MaxSectorsPerPacket) {
        /* Allocate each tag. */
        if ((tag = wv_mallocz(sizeof *tag)) == NULL) {
            DBG("Couldn't allocate tag; bye!\n");
            /* We failed while allocating tags; free the ones we built. */
            tag = new_tag_list;
            while (tag != NULL) {
                previous_tag = tag;
                tag = tag->next;
                wv_free(previous_tag->packet_data);
                wv_free(previous_tag);
              }
            wv_free(request_ptr);
            irp->IoStatus.Information = 0;
            irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            IoCompleteRequest ( irp, IO_NO_INCREMENT );
            return STATUS_INSUFFICIENT_RESOURCES;
          } /* if !tag */

        /* Initialize each tag. */
        tag->type = AoeTagTypeIo_;
        tag->request_ptr = request_ptr;
        tag->device = dev_ptr;
        request_ptr->TagCount++;
        tag->Id = 0;
        tag->BufferOffset = i * disk_ptr->SectorSize;
        tag->SectorCount = (
            (sector_count - i) <
            aoe_disk_ptr->MaxSectorsPerPacket ?
            sector_count - i :
            aoe_disk_ptr->MaxSectorsPerPacket
          );

        /* Allocate and initialize each tag's AoE packet. */
        tag->PacketSize = sizeof (AOE_S_PACKET_);
        if (mode == WvlDiskIoModeWrite)
          tag->PacketSize += tag->SectorCount * disk_ptr->SectorSize;
        if ((tag->packet_data = wv_mallocz(tag->PacketSize)) == NULL) {
            DBG("Couldn't allocate tag->packet_data; bye!\n");
            /*
             * We failed while allocating an AoE packet; free
             * the tags we built.
             */
            wv_free(tag);
            tag = new_tag_list;
            while (tag != NULL) {
                previous_tag = tag;
                tag = tag->next;
                wv_free(previous_tag->packet_data);
                wv_free(previous_tag);
              }
            wv_free(request_ptr);
            irp->IoStatus.Information = 0;
            irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return STATUS_INSUFFICIENT_RESOURCES;
          } /* if !tag->packet_data */
        tag->packet_data->Ver = AOEPROTOCOLVER;
        tag->packet_data->Major = htons ((UINT16) aoe_disk_ptr->Major);
        tag->packet_data->Minor = (UCHAR) aoe_disk_ptr->Minor;
        tag->packet_data->Tag = 0;
        tag->packet_data->Command = 0;
        tag->packet_data->ExtendedAFlag = TRUE;
        if (mode == WvlDiskIoModeRead)
          tag->packet_data->Cmd = 0x24;  /* READ SECTOR */
          else {
            tag->packet_data->Cmd = 0x34;  /* WRITE SECTOR */
            tag->packet_data->WriteAFlag = 1;
          }
        tag->packet_data->Count = (UCHAR) tag->SectorCount;
        tag->packet_data->Lba0 = (UCHAR) (((start_sector + i) >> 0) & 255);
        tag->packet_data->Lba1 = (UCHAR) (((start_sector + i) >> 8) & 255);
        tag->packet_data->Lba2 = (UCHAR) (((start_sector + i) >> 16) & 255);
        tag->packet_data->Lba3 = (UCHAR) (((start_sector + i) >> 24) & 255);
        tag->packet_data->Lba4 = (UCHAR) (((start_sector + i) >> 32) & 255);
        tag->packet_data->Lba5 = (UCHAR) (((start_sector + i) >> 40) & 255);

        /* For a write request, copy from the buffer into the AoE packet. */
        if (mode == WvlDiskIoModeWrite) {
            RtlCopyMemory(
                tag->packet_data->Data,
                buffer + (tag->BufferOffset),
                tag->SectorCount * disk_ptr->SectorSize
              );
          }
        /* Add this tag to the request's tag list. */
        tag->previous = previous_tag;
        tag->next = NULL;
        if (new_tag_list == NULL)
          new_tag_list = tag;
          else
          previous_tag->next = tag;
        previous_tag = tag;
      } /* for */
    /* Split the requested sectors into packets in tags. */
    request_ptr->TotalTags = request_ptr->TagCount;

    /* Wait until we have the global spin-lock. */
    KeAcquireSpinLock(&AoeLock_, &Irql);

    /* Enqueue our request's tag list to the global tag list. */
    if (AoeTagListLast_ == NULL)
      AoeTagListFirst_ = new_tag_list;
      else {
        AoeTagListLast_->next = new_tag_list;
        new_tag_list->previous = AoeTagListLast_;
      }
    /* Adjust the global list to reflect our last tag. */
    AoeTagListLast_ = tag;

    irp->IoStatus.Information = 0;
    irp->IoStatus.Status = STATUS_PENDING;
    IoMarkIrpPending(irp);

    KeReleaseSpinLock(&AoeLock_, Irql);
    KeSetEvent(&AoeSignal_, 0, FALSE);
    return STATUS_PENDING;
  }

static VOID STDCALL add_target(
    IN PUCHAR ClientMac,
    IN PUCHAR ServerMac,
    UINT16 Major,
    UCHAR Minor,
    LONGLONG LBASize
  ) {
    AOE_SP_TARGET_LIST_ Walker, Last;
    KIRQL Irql;

    KeAcquireSpinLock(&AoeTargetListLock_, &Irql);
    Walker = Last = AoeTargetList_;
    while (Walker != NULL) {
        if (
            wv_memcmpeq(&Walker->Target.ClientMac, ClientMac, 6) &&
            wv_memcmpeq(&Walker->Target.ServerMac, ServerMac, 6) &&
            Walker->Target.Major == Major &&
            Walker->Target.Minor == Minor
          ) {
            if (Walker->Target.LBASize != LBASize) {
                DBG(
                    "LBASize changed for e%d.%d " "(%I64u->%I64u)\n",
                    Major,
                    Minor,
                    Walker->Target.LBASize,
                    LBASize
                  );
                Walker->Target.LBASize = LBASize;
              }
            KeQuerySystemTime(&Walker->Target.ProbeTime);
            KeReleaseSpinLock(&AoeTargetListLock_, Irql);
            return;
          }
        Last = Walker;
        Walker = Walker->next;
      } /* while Walker */

    if ((Walker = wv_malloc(sizeof *Walker)) == NULL) {
        DBG("wv_malloc Walker\n");
        KeReleaseSpinLock(&AoeTargetListLock_, Irql);
        return;
      }
    Walker->next = NULL;
    RtlCopyMemory(Walker->Target.ClientMac, ClientMac, 6);
    RtlCopyMemory(Walker->Target.ServerMac, ServerMac, 6);
    Walker->Target.Major = Major;
    Walker->Target.Minor = Minor;
    Walker->Target.LBASize = LBASize;
    KeQuerySystemTime(&Walker->Target.ProbeTime);

    if (Last == NULL)
      AoeTargetList_ = Walker;
      else
      Last->next = Walker;
    KeReleaseSpinLock(&AoeTargetListLock_, Irql);
  }

/**
 * Process an AoE reply.
 *
 * @v SourceMac         The AoE server's MAC address.
 * @v DestinationMac    The AoE client's MAC address.
 * @v Data              The AoE packet.
 * @v DataSize          The AoE packet's size.
 */
NTSTATUS STDCALL aoe__reply(
    IN PUCHAR SourceMac,
    IN PUCHAR DestinationMac,
    IN PUCHAR Data,
    IN UINT32 DataSize
  )
  {
    AOE_SP_PACKET_ reply = (AOE_SP_PACKET_) Data;
    LONGLONG LBASize;
    AOE_SP_WORK_TAG_ tag;
    KIRQL Irql;
    BOOLEAN Found = FALSE;
    LARGE_INTEGER CurrentTime;
    WV_SP_DISK_T disk_ptr;
    AOE_SP_DISK_ aoe_disk_ptr;

    /* Discard non-responses. */
    if (!reply->ResponseFlag)
      return STATUS_SUCCESS;

    /* If the response matches our probe, add the AoE disk device. */
    if (AoeProbeTag_->Id == reply->Tag) {
        RtlCopyMemory(&LBASize, &reply->Data[200], sizeof (LONGLONG));
        add_target(
            DestinationMac,
            SourceMac,
            ntohs(reply->Major),
            reply->Minor,
            LBASize
          );
        return STATUS_SUCCESS;
      }

    /* Wait until we have the global spin-lock. */
    KeAcquireSpinLock(&AoeLock_, &Irql);

    /* Search for request tag. */
    if (AoeTagListFirst_ == NULL) {
        KeReleaseSpinLock(&AoeLock_, Irql);
        return STATUS_SUCCESS;
      }
    tag = AoeTagListFirst_;
    while (tag != NULL) {
        if (
            (tag->Id == reply->Tag) &&
            (tag->packet_data->Major == reply->Major) &&
            (tag->packet_data->Minor == reply->Minor)
          ) {
            Found = TRUE;
            break;
          }
        tag = tag->next;
      } /* while tag */
    if (!Found) {
        KeReleaseSpinLock(&AoeLock_, Irql);
        return STATUS_SUCCESS;
      } else {
        /* Remove the tag from the global tag list. */
        if (tag->previous == NULL)
          AoeTagListFirst_ = tag->next;
          else
          tag->previous->next = tag->next;
        if (tag->next == NULL)
          AoeTagListLast_ = tag->previous;
          else
          tag->next->previous = tag->previous;
        AoePendingTags_--;
        if (AoePendingTags_ < 0)
          DBG("AoePendingTags_ < 0!!\n");
        KeSetEvent(&AoeSignal_, 0, FALSE);
      } /* if !Found */
    KeReleaseSpinLock(&AoeLock_, Irql);

    /* Establish pointers to the disk device and AoE disk. */
    disk_ptr = disk__get_ptr(tag->device);
    aoe_disk_ptr = AoeDiskFromDev_(tag->device);

    /* If our tag was a discovery request, note the server. */
    if (wv_memcmpeq(aoe_disk_ptr->ServerMac, "\xff\xff\xff\xff\xff\xff", 6)) {
        RtlCopyMemory(aoe_disk_ptr->ServerMac, SourceMac, 6);
        DBG(
            "Major: %d minor: %d found on server "
              "%02x:%02x:%02x:%02x:%02x:%02x\n",
            aoe_disk_ptr->Major,
            aoe_disk_ptr->Minor,
            SourceMac[0],
            SourceMac[1],
            SourceMac[2],
            SourceMac[3],
            SourceMac[4],
            SourceMac[5]
          );
      }

    KeQuerySystemTime(&CurrentTime);
    aoe_disk_ptr->Timeout -= (UINT32) ((
        aoe_disk_ptr->Timeout -
        (CurrentTime.QuadPart - tag->FirstSendTime.QuadPart)
      ) / 1024);
    /* TODO: Replace the values below with #defined constants. */
    if (aoe_disk_ptr->Timeout > 100000000)
      aoe_disk_ptr->Timeout = 100000000;

    switch (tag->type) {
        case AoeTagTypeSearchDrive_:
          KeAcquireSpinLock(&disk_ptr->SpinLock, &Irql);
          switch (aoe_disk_ptr->search_state) {
              case AoeSearchStateGettingSize_:
                /* The reply tells us the disk size. */
                RtlCopyMemory(
                    &disk_ptr->LBADiskSize,
                    &reply->Data[200],
                    sizeof (LONGLONG)
                  );
                /* Next we are concerned with the disk geometry. */
                aoe_disk_ptr->search_state = AoeSearchStateGetGeometry_;
                break;

              case AoeSearchStateGettingGeometry_:
                /*
                 * FIXME: use real values from partition table.
                 * We used to truncate a fractional end cylinder, but
                 * now leave it be in the hopes everyone uses LBA
                 */
                disk_ptr->SectorSize = 512;
                disk_ptr->Heads = 255;
                disk_ptr->Sectors = 63;
                disk_ptr->Cylinders =
                  disk_ptr->LBADiskSize /
                  (disk_ptr->Heads * disk_ptr->Sectors);
                /* Next we are concerned with the max. sectors per packet. */
                aoe_disk_ptr->search_state =
                  AoeSearchStateGetMaxSectsPerPacket_;
                break;

              case AoeSearchStateGettingMaxSectsPerPacket_:
                DataSize -= sizeof (AOE_S_PACKET_);
                if (DataSize < (
                    aoe_disk_ptr->MaxSectorsPerPacket *
                    disk_ptr->SectorSize
                  )) {
                    DBG(
                        "Packet size too low while getting "
                          "MaxSectorsPerPacket (tried %d, got size of %d)\n",
                        aoe_disk_ptr->MaxSectorsPerPacket,
                        DataSize
                      );
                    aoe_disk_ptr->MaxSectorsPerPacket--;
                    aoe_disk_ptr->search_state = AoeSearchStateDone_;
                  } else if (
                    aoe_disk_ptr->MTU <
                    (sizeof (AOE_S_PACKET_) +
                    ((aoe_disk_ptr->MaxSectorsPerPacket + 1)
                    * disk_ptr->SectorSize))
                  ) {
                      DBG(
                          "Got MaxSectorsPerPacket %d at size of %d. "
                            "MTU of %d reached\n",
                          aoe_disk_ptr->MaxSectorsPerPacket,
                          DataSize,
                          aoe_disk_ptr->MTU
                        );
                      aoe_disk_ptr->search_state = AoeSearchStateDone_;
                    } else {
                      DBG(
                          "Got MaxSectorsPerPacket %d at size of %d, "
                            "trying next...\n",
                            aoe_disk_ptr->MaxSectorsPerPacket,
                            DataSize
                        );
                      aoe_disk_ptr->search_state =
                        AoeSearchStateGetMaxSectsPerPacket_;
                    }
                break;

              default:
                DBG("Undefined search_state!\n");
                break;
            } /* switch search state. */
          KeReleaseSpinLock(&disk_ptr->SpinLock, Irql);
          KeSetEvent(&disk_ptr->SearchEvent, 0, FALSE);
          break;

        case AoeTagTypeIo_:
          /* If the reply is in response to a read request, get our data! */
          if (tag->request_ptr->Mode == WvlDiskIoModeRead) {
              RtlCopyMemory(
                  tag->request_ptr->Buffer + (tag->BufferOffset),
                  reply->Data,
                  tag->SectorCount * disk_ptr->SectorSize
                );
            }
          /*
           * If this is the last reply expected for the read request,
           * complete the IRP and free the request.
           */
          if (InterlockedDecrement(&tag->request_ptr->TagCount) == 0) {
              WvlIrpComplete(
                  tag->request_ptr->Irp,
                  tag->request_ptr->SectorCount * disk_ptr->SectorSize,
                  STATUS_SUCCESS
                );
              wv_free(tag->request_ptr);
            }
          break;

        default:
          DBG("Unknown tag type!!\n");
          break;
      } /* switch tag type. */

    KeSetEvent(&AoeSignal_, 0, FALSE);
    wv_free(tag->packet_data);
    wv_free(tag);
    return STATUS_SUCCESS;
  }

VOID aoe__reset_probe(void) {
    AoeProbeTag_->SendTime.QuadPart = 0LL;
  }

static VOID STDCALL AoeThread_(IN PVOID StartContext) {
    NTSTATUS status;
    LARGE_INTEGER Timeout, CurrentTime, ProbeTime, ReportTime;
    UINT32 NextTagId = 1;
    AOE_SP_WORK_TAG_ tag;
    KIRQL Irql;
    UINT32 Sends = 0;
    UINT32 Resends = 0;
    UINT32 ResendFails = 0;
    UINT32 Fails = 0;
    UINT32 RequestTimeout = 0;
    WV_SP_DISK_T disk_ptr;
    AOE_SP_DISK_ aoe_disk_ptr;

    DBG("Entry\n");

    ReportTime.QuadPart = 0LL;
    ProbeTime.QuadPart = 0LL;

    while (TRUE) {
        /*
         * TODO: Make the below value a #defined constant:
         * 100.000 * 100ns = 10.000.000 ns = 10ms
         */
        Timeout.QuadPart = -100000LL;
        KeWaitForSingleObject(
            &AoeSignal_,
            Executive,
            KernelMode,
            FALSE,
            &Timeout
          );
        KeResetEvent(&AoeSignal_);
        if (AoeStop_) {
            DBG("Stopping...\n");
            PsTerminateSystemThread(STATUS_SUCCESS);
            DBG("Yikes!\n");
            return;
          }
        KeQuerySystemTime(&CurrentTime);
        /* TODO: Make the below value a #defined constant. */
        if (CurrentTime.QuadPart > (ReportTime.QuadPart + 10000000LL)) {
            DBG(
                "Sends: %d  Resends: %d  ResendFails: %d  Fails: %d  "
                  "AoePendingTags_: %d  RequestTimeout: %d\n",
                Sends,
                Resends,
                ResendFails,
                Fails,
                AoePendingTags_,
                RequestTimeout
              );
            Sends = 0;
            Resends = 0;
            ResendFails = 0;
            Fails = 0;
            KeQuerySystemTime(&ReportTime);
          }

        /* TODO: Make the below value a #defined constant. */
        if (
            CurrentTime.QuadPart >
            (AoeProbeTag_->SendTime.QuadPart + 100000000LL)
          ) {
            AoeProbeTag_->Id = NextTagId++;
            if (NextTagId == 0)
              NextTagId++;
            AoeProbeTag_->packet_data->Tag = AoeProbeTag_->Id;
            Protocol_Send(
                "\xff\xff\xff\xff\xff\xff",
                "\xff\xff\xff\xff\xff\xff",
                (PUCHAR) AoeProbeTag_->packet_data,
                AoeProbeTag_->PacketSize,
                NULL
              );
            KeQuerySystemTime(&AoeProbeTag_->SendTime);
          }

        KeAcquireSpinLock(&AoeLock_, &Irql);
        if (AoeTagListFirst_ == NULL) {
            KeReleaseSpinLock(&AoeLock_, Irql);
            continue;
          }
        tag = AoeTagListFirst_;
        while (tag != NULL) {
            /* Establish pointers to the disk and AoE disk. */
            disk_ptr = disk__get_ptr(tag->device);
            aoe_disk_ptr = AoeDiskFromDev_(tag->device);
      
            RequestTimeout = aoe_disk_ptr->Timeout;
            if (tag->Id == 0) {
                #if 0
                if (AoePendingTags_ <= 102400) {
                  }
                #endif
                if (AoePendingTags_ <= 64) {
                    if (AoePendingTags_ < 0)
                      DBG("AoePendingTags_ < 0!!\n");
                    tag->Id = NextTagId++;
                    if (NextTagId == 0)
                      NextTagId++;
                    tag->packet_data->Tag = tag->Id;
                    if (Protocol_Send(
                        aoe_disk_ptr->ClientMac,
                        aoe_disk_ptr->ServerMac,
                        (PUCHAR) tag->packet_data,
                        tag->PacketSize,
                        tag
                      )) {
                        KeQuerySystemTime(&tag->FirstSendTime);
                        KeQuerySystemTime(&tag->SendTime);
                        AoePendingTags_++;
                        Sends++;
                      } else {
                        Fails++;
                        tag->Id = 0;
                        break;
                      } /* if send succeeds. */
                    } /* if pending tags < 64 */
              } else {
                KeQuerySystemTime(&CurrentTime);
                if (
                    CurrentTime.QuadPart >
                    (tag->SendTime.QuadPart + (LONGLONG) (aoe_disk_ptr->Timeout * 2))
                  ) {
                    if (Protocol_Send(
                        aoe_disk_ptr->ClientMac,
                        aoe_disk_ptr->ServerMac,
                        (PUCHAR) tag->packet_data,
                        tag->PacketSize,
                        tag
                      )) {
                        KeQuerySystemTime(&tag->SendTime);
                        aoe_disk_ptr->Timeout += aoe_disk_ptr->Timeout / 1000;
                        if (aoe_disk_ptr->Timeout > 100000000)
                          aoe_disk_ptr->Timeout = 100000000;
                        Resends++;
                      } else {
                        ResendFails++;
                        break;
                      }
                  }
              } /* if tag ID == 0 */
            tag = tag->next;
            if (tag == AoeTagListFirst_) {
                DBG("Taglist Cyclic!!\n");
                break;
              }
          } /* while tag */
        KeReleaseSpinLock(&AoeLock_, Irql);
      } /* while TRUE */
    DBG("Exit\n");
  }

static UINT32 AoeDiskMaxXferLen_(IN WV_SP_DISK_T disk_ptr) {
    AOE_SP_DISK_ aoe_disk_ptr = AoeDiskFromDev_(disk_ptr->Dev);

    return disk_ptr->SectorSize * aoe_disk_ptr->MaxSectorsPerPacket;
  }

static UINT32 STDCALL query_id(
    IN WV_SP_DEV_T dev,
    IN BUS_QUERY_ID_TYPE query_type,
    IN OUT WCHAR (*buf)[512]
  ) {
    AOE_SP_DISK_ aoe_disk = AoeDiskFromDev_(dev);

    switch (query_type) {
        case BusQueryDeviceID:
          return swprintf(*buf, WVL_M_WLIT L"\\AoEHardDisk") + 1;

        case BusQueryInstanceID:
          return swprintf(
              *buf,
              L"AoE_at_Shelf_%d.Slot_%d",
              aoe_disk->Major,
              aoe_disk->Minor
            ) + 1;

        case BusQueryHardwareIDs: {
            UINT32 tmp;

            tmp = swprintf(*buf, WVL_M_WLIT L"\\AoEHardDisk") + 1;
            tmp += swprintf(*buf + tmp, L"GenDisk") + 4;
            return tmp;
          }

        case BusQueryCompatibleIDs:
          return swprintf(*buf, L"GenDisk") + 4;

        default:
          return 0;
      }
  }

#ifdef _MSC_VER
#  pragma pack(1)
#endif
struct AOE_ABFT {
    UINT32 Signature;  /* 0x54464261 (aBFT) */
    UINT32 Length;
    UCHAR Revision;
    UCHAR Checksum;
    UCHAR OEMID[6];
    UCHAR OEMTableID[8];
    UCHAR Reserved1[12];
    UINT16 Major;
    UCHAR Minor;
    UCHAR Reserved2;
    UCHAR ClientMac[6];
  } __attribute__((__packed__));
typedef struct AOE_ABFT AOE_S_ABFT, * AOE_SP_ABFT;
#ifdef _MSC_VER
#  pragma pack()
#endif

static VOID STDCALL AoeDiskClose_(IN WV_SP_DISK_T disk_ptr) {
    return;
  }

static VOID AoeProcessAbft_(void) {
    PHYSICAL_ADDRESS PhysicalAddress;
    PUCHAR PhysicalMemory;
    UINT32 Offset, Checksum, i;
    BOOLEAN FoundAbft = FALSE;
    AOE_S_ABFT AoEBootRecord;
    AOE_SP_DISK_ aoe_disk;

    /* Find aBFT. */
    PhysicalAddress.QuadPart = 0LL;
    PhysicalMemory = MmMapIoSpace(PhysicalAddress, 0xa0000, MmNonCached);
    if (!PhysicalMemory) {
        DBG("Could not map low memory\n");
        goto err_map_mem;
      }
    for (Offset = 0; Offset < 0xa0000; Offset += 0x10) {
        if (!(
            ((AOE_SP_ABFT) (PhysicalMemory + Offset))->Signature ==
            0x54464261
          ))
          continue;
        Checksum = 0;
        for (
            i = 0;
            i < ((AOE_SP_ABFT) (PhysicalMemory + Offset))->Length;
            i++
          )
          Checksum += PhysicalMemory[Offset + i];
        if (Checksum & 0xff)
          continue;
        if (((AOE_SP_ABFT) (PhysicalMemory + Offset))->Revision != 1) {
            DBG(
                "Found aBFT with mismatched revision v%d at "
                  "segment 0x%4x. want v1.\n",
                ((AOE_SP_ABFT) (PhysicalMemory + Offset))->Revision,
                (Offset / 0x10)
              );
            continue;
          }
        DBG("Found aBFT at segment: 0x%04x\n", (Offset / 0x10));
        RtlCopyMemory(
            &AoEBootRecord,
            PhysicalMemory + Offset,
            sizeof AoEBootRecord
          );
        FoundAbft = TRUE;
        break;
      }
    MmUnmapIoSpace(PhysicalMemory, 0xa0000);

    #ifdef RIS
    FoundAbft = TRUE;
    RtlCopyMemory(AoEBootRecord.ClientMac, "\x00\x0c\x29\x34\x69\x34", 6);
    AoEBootRecord.Major = 0;
    AoEBootRecord.Minor = 10;
    #endif

    if (!FoundAbft)
      goto out_no_abft;
    aoe_disk = AoeDiskCreatePdo_();
    if(aoe_disk == NULL) {
        DBG("Could not create AoE disk from aBFT!\n");
        return;
      }
    DBG(
        "Attaching AoE disk from client NIC "
          "%02x:%02x:%02x:%02x:%02x:%02x to major: %d minor: %d\n",
        AoEBootRecord.ClientMac[0],
        AoEBootRecord.ClientMac[1],
        AoEBootRecord.ClientMac[2],
        AoEBootRecord.ClientMac[3],
        AoEBootRecord.ClientMac[4],
        AoEBootRecord.ClientMac[5],
        AoEBootRecord.Major,
        AoEBootRecord.Minor
      );
    RtlCopyMemory(aoe_disk->ClientMac, AoEBootRecord.ClientMac, 6);
    RtlFillMemory(aoe_disk->ServerMac, 6, 0xff);
    aoe_disk->Major = AoEBootRecord.Major;
    aoe_disk->Minor = AoEBootRecord.Minor;
    aoe_disk->MaxSectorsPerPacket = 1;
    aoe_disk->Timeout = 200000;          /* 20 ms. */
    aoe_disk->disk->Dev->Boot = TRUE;
    if (!AoeDiskInit_(aoe_disk)) {
        DBG("Couldn't find AoE disk!\n");
        AoeDiskFree_(aoe_disk->disk->Dev);
        return;
      }
    AoeBusAddDev(aoe_disk->disk->Dev);
    return;

    out_no_abft:
    DBG("No aBFT found\n");

    err_map_mem:

    return;
  }

NTSTATUS STDCALL AoeBusDevCtlScan(IN PIRP irp) {
    KIRQL irql;
    UINT32 count;
    AOE_SP_TARGET_LIST_ target_walker;
    AOE_SP_MOUNT_TARGETS targets;
    PIO_STACK_LOCATION io_stack_loc = IoGetCurrentIrpStackLocation(irp);

    DBG("Got IOCTL_AOE_SCAN...\n");
    KeAcquireSpinLock(&AoeTargetListLock_, &irql);

    count = 0;
    target_walker = AoeTargetList_;
    while (target_walker != NULL) {
        count++;
        target_walker = target_walker->next;
      }

    targets = wv_malloc(sizeof *targets + (count * sizeof targets->Target[0]));
    if (targets == NULL) {
        DBG("wv_malloc targets\n");
        return WvlIrpComplete(
            irp,
            0,
            STATUS_INSUFFICIENT_RESOURCES
          );
      }
    irp->IoStatus.Information =
      sizeof (AOE_S_MOUNT_TARGETS) + (count * sizeof (AOE_S_MOUNT_TARGET));
    targets->Count = count;

    count = 0;
    target_walker = AoeTargetList_;
    while (target_walker != NULL) {
        RtlCopyMemory(
            &targets->Target[count],
            &target_walker->Target,
            sizeof (AOE_S_MOUNT_TARGET)
          );
        count++;
        target_walker = target_walker->next;
      }
    RtlCopyMemory(
        irp->AssociatedIrp.SystemBuffer,
        targets,
        (io_stack_loc->Parameters.DeviceIoControl.OutputBufferLength <
          (sizeof (AOE_S_MOUNT_TARGETS) + (count * sizeof (AOE_S_MOUNT_TARGET))) ?
          io_stack_loc->Parameters.DeviceIoControl.OutputBufferLength :
          (sizeof (AOE_S_MOUNT_TARGETS) + (count * sizeof (AOE_S_MOUNT_TARGET)))
        )
      );
    wv_free(targets);

    KeReleaseSpinLock(&AoeTargetListLock_, irql);
    return WvlIrpComplete(irp, irp->IoStatus.Information, STATUS_SUCCESS);
  }

NTSTATUS STDCALL AoeBusDevCtlShow(IN PIRP irp) {
    UINT32 count;
    WVL_SP_BUS_NODE walker;
    AOE_SP_MOUNT_DISKS disks;
    wv_size_t size;
    PIO_STACK_LOCATION io_stack_loc = IoGetCurrentIrpStackLocation(irp);

    DBG("Got IOCTL_AOE_SHOW...\n");

    count = WvlBusGetNodeCount(&AoeBusMain);
    size = sizeof *disks + (count * sizeof disks->Disk[0]);

    disks = wv_malloc(size);
    if (disks == NULL) {
        DBG("wv_malloc disks\n");
        return WvlIrpComplete(
            irp,
            0,
            STATUS_INSUFFICIENT_RESOURCES
          );
      }
    irp->IoStatus.Information = size;
    disks->Count = count;

    count = 0;
    walker = NULL;
    /* For each node on the bus... */
    while (walker = WvlBusGetNextNode(&AoeBusMain, walker)) {
        WV_SP_DEV_T dev = WvDevFromDevObj(WvlBusGetNodePdo(walker));
        WV_SP_DISK_T disk = disk__get_ptr(dev);
        AOE_SP_DISK_ aoe_disk = AoeDiskFromDev_(dev);

        disks->Disk[count].Disk = dev->DevNum;
        RtlCopyMemory(
            &disks->Disk[count].ClientMac,
            &aoe_disk->ClientMac,
            6
          );
        RtlCopyMemory(
            &disks->Disk[count].ServerMac,
            &aoe_disk->ServerMac,
            6
          );
        disks->Disk[count].Major = aoe_disk->Major;
        disks->Disk[count].Minor = aoe_disk->Minor;
        disks->Disk[count].LBASize = disk->LBADiskSize;
        count++;
      }
    RtlCopyMemory(
        irp->AssociatedIrp.SystemBuffer,
        disks,
        (io_stack_loc->Parameters.DeviceIoControl.OutputBufferLength <
          (sizeof (AOE_S_MOUNT_DISKS) + (count * sizeof (AOE_S_MOUNT_DISK))) ?
          io_stack_loc->Parameters.DeviceIoControl.OutputBufferLength :
          (sizeof (AOE_S_MOUNT_DISKS) + (count * sizeof (AOE_S_MOUNT_DISK)))
        )
      );
    wv_free(disks);
    return WvlIrpComplete(irp, irp->IoStatus.Information, STATUS_SUCCESS);
  }

NTSTATUS STDCALL AoeBusDevCtlMount(IN PIRP irp) {
    PUCHAR buffer = irp->AssociatedIrp.SystemBuffer;
    AOE_SP_DISK_ aoe_disk;

    DBG(
        "Got IOCTL_AOE_MOUNT for client: %02x:%02x:%02x:%02x:%02x:%02x "
          "Major:%d Minor:%d\n",
        buffer[0],
        buffer[1],
        buffer[2],
        buffer[3],
        buffer[4],
        buffer[5],
        *(PUINT16) (buffer + 6),
        (UCHAR) buffer[8]
      );
    aoe_disk = AoeDiskCreatePdo_();
    if (aoe_disk == NULL) {
        DBG("Could not create AoE disk!\n");
        return WvlIrpComplete(
            irp,
            0,
            STATUS_INSUFFICIENT_RESOURCES
          );
      }
    RtlCopyMemory(aoe_disk->ClientMac, buffer, 6);
    RtlFillMemory(aoe_disk->ServerMac, 6, 0xff);
    aoe_disk->Major = *(PUINT16) (buffer + 6);
    aoe_disk->Minor = (UCHAR) buffer[8];
    aoe_disk->MaxSectorsPerPacket = 1;
    aoe_disk->Timeout = 200000;             /* 20 ms. */
    aoe_disk->disk->Dev->Boot = FALSE;
    if (!AoeDiskInit_(aoe_disk)) {
        DBG("Couldn't find AoE disk!\n");
        AoeDiskFree_(aoe_disk->disk->Dev);
        return WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
      }
    AoeBusAddDev(aoe_disk->disk->Dev);

    return WvlIrpComplete(irp, 0, STATUS_SUCCESS);
  }

/**
 * Create an AoE disk PDO.
 *
 * @ret aoe_disk        The address of a new AoE disk, or NULL for failure.
 */
static AOE_SP_DISK_ AoeDiskCreatePdo_(void) {
    NTSTATUS status;
    PDEVICE_OBJECT pdo;
    AOE_SP_DISK_ aoe_disk;

    DBG("Creating AoE disk PDO...\n");

    /* Create the disk PDO. */
    status = WvlDiskCreatePdo(
        AoeDriverObj_,
        sizeof *aoe_disk,
        WvlDiskMediaTypeHard,
        &pdo
      );
    if (!NT_SUCCESS(status)) {
        WvlError("WvlDiskCreatePdo", status);
        return NULL;
      }

    aoe_disk = pdo->DeviceExtension;
    RtlZeroMemory(aoe_disk, sizeof *aoe_disk);
    /* Populate non-zero device defaults. */
    WvDiskInit(aoe_disk->disk);
    WvDevInit(aoe_disk->disk->Dev);
    aoe_disk->disk->Dev->Ops.Free = AoeDiskFree_;
    aoe_disk->disk->Dev->Ops.PnpId = query_id;
    aoe_disk->disk->Dev->ext = aoe_disk->disk;
    aoe_disk->disk->Media = WvlDiskMediaTypeHard;
    aoe_disk->disk->disk_ops.Io = AoeDiskIo_;
    aoe_disk->disk->disk_ops.MaxXferLen = AoeDiskMaxXferLen_;
    aoe_disk->disk->disk_ops.Close = AoeDiskClose_;
    aoe_disk->disk->disk_ops.UnitNum = AoeDiskUnitNum_;
    aoe_disk->disk->ext = aoe_disk;
    aoe_disk->disk->DriverObj = AoeDriverObj_;

    /* Set associations for the PDO, device, disk. */
    WvDevForDevObj(pdo, aoe_disk->disk->Dev);
    KeInitializeEvent(
        &aoe_disk->disk->SearchEvent,
        SynchronizationEvent,
        FALSE
      );
    KeInitializeSpinLock(&aoe_disk->disk->SpinLock);
    aoe_disk->disk->Dev->Self = pdo;

    /* Some device parameters. */
    pdo->Flags |= DO_DIRECT_IO;         /* FIXME? */
    pdo->Flags |= DO_POWER_INRUSH;      /* FIXME? */

    DBG("New PDO: %p\n", pdo);
    return aoe_disk;
  }

/**
 * Default AoE disk deletion operation.
 *
 * @v dev               Points to the AoE disk device to delete.
 */
static VOID STDCALL AoeDiskFree_(IN WV_SP_DEV_T dev) {
    IoDeleteDevice(dev->Self);
  }

static NTSTATUS STDCALL AoeIrpNotSupported_(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    return WvlIrpComplete(irp, 0, STATUS_NOT_SUPPORTED);
  }

/* Handle a power IRP. */
static NTSTATUS AoeIrpPower_(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    WV_SP_DEV_T dev;

    WVL_M_DEBUG_IRP_START(dev_obj, irp);
    /* Check for a bus IRP. */
    if (dev_obj == AoeBusMain.Fdo)
      return WvlBusPower(&AoeBusMain, irp);
    /* WvDevFromDevObj() checks for a NULL dev_obj */
    dev = WvDevFromDevObj(dev_obj);
    /* Check that the device exists. */
    if (!dev || dev->State == WvDevStateDeleted) {
        /* Even if it doesn't, a power IRP is important! */
        PoStartNextPowerIrp(irp);
        return WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
      }
    /* Use the disk routine. */
    return WvDiskIrpPower(dev, irp);
  }

/* Handle an IRP_MJ_CREATE or IRP_MJ_CLOSE IRP. */
static NTSTATUS AoeIrpCreateClose_(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    WV_SP_DEV_T dev;

    WVL_M_DEBUG_IRP_START(dev_obj, irp);
    /* Check for a bus IRP. */
    if (dev_obj == AoeBusMain.Fdo)
      return WvlIrpComplete(irp, 0, STATUS_SUCCESS);
    /* WvDevFromDevObj() checks for a NULL dev_obj */
    dev = WvDevFromDevObj(dev_obj);
    /* Check that the device exists. */
    if (!dev || dev->State == WvDevStateDeleted)
      return WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
    /* Always succeed with nothing to do. */
    return WvlIrpComplete(irp, 0, STATUS_SUCCESS);
  }

/* Handle an IRP_MJ_SYSTEM_CONTROL IRP. */
static NTSTATUS AoeIrpSysCtl_(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    WV_SP_DEV_T dev;

    WVL_M_DEBUG_IRP_START(dev_obj, irp);
    /* Check for a bus IRP. */
    if (dev_obj == AoeBusMain.Fdo)
      return WvlBusSysCtl(&AoeBusMain, irp);
    /* WvDevFromDevObj() checks for a NULL dev_obj */
    dev = WvDevFromDevObj(dev_obj);
    /* Check that the device exists. */
    if (!dev || dev->State == WvDevStateDeleted)
      return WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
    /* Use the disk routine. */
    return WvDiskIrpSysCtl(dev, irp);
  }

/* Handle an IRP_MJ_DEVICE_CONTROL IRP. */
static NTSTATUS AoeIrpDevCtl_(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    WV_SP_DEV_T dev;
    PIO_STACK_LOCATION io_stack_loc = IoGetCurrentIrpStackLocation(irp);

    WVL_M_DEBUG_IRP_START(dev_obj, irp);
    /* Check for a bus IRP. */
    if (dev_obj == AoeBusMain.Fdo) {
        return AoeBusDevCtl(
            irp,
            io_stack_loc->Parameters.DeviceIoControl.IoControlCode
          );
      }
    /* WvDevFromDevObj() checks for a NULL dev_obj */
    dev = WvDevFromDevObj(dev_obj);
    /* Check that the device exists. */
    if (!dev || dev->State == WvDevStateDeleted)
      return WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
    /* Use the disk routine. */
    return disk_dev_ctl__dispatch(
        dev,
        irp,
        io_stack_loc->Parameters.DeviceIoControl.IoControlCode
      );
  }

/* Handle an IRP_MJ_SCSI IRP. */
static NTSTATUS AoeIrpScsi_(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    WV_SP_DEV_T dev;
    PIO_STACK_LOCATION io_stack_loc;

    WVL_M_DEBUG_IRP_START(dev_obj, irp);
    /* Check for a bus IRP. */
    if (dev_obj == AoeBusMain.Fdo)
      return WvlIrpComplete(irp, 0, STATUS_NOT_SUPPORTED);
    /* WvDevFromDevObj() checks for a NULL dev_obj */
    dev = WvDevFromDevObj(dev_obj);
    io_stack_loc = IoGetCurrentIrpStackLocation(irp);
    /* Check that the device exists. */
    if (!dev || dev->State == WvDevStateDeleted)
      return WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
    /* Use the disk routine. */
    return disk_scsi__dispatch(
        dev,
        irp,
        io_stack_loc->Parameters.Scsi.Srb->Function
      );
  }

/* Handle an IRP_MJ_PNP IRP. */
static NTSTATUS AoeIrpPnp_(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    WV_SP_DEV_T dev;
    UCHAR code = IoGetCurrentIrpStackLocation(irp)->MinorFunction;

    WVL_M_DEBUG_IRP_START(dev_obj, irp);
    /* Check for a bus IRP. */
    if (dev_obj == AoeBusMain.Fdo) {
        NTSTATUS status;

        status = WvlBusPnp(&AoeBusMain, irp);
        /* Did the bus detach? */
        if (AoeBusMain.State == WvlBusStateDeleted) {
            /* Stop the thread. */
            AoeStop_ = TRUE;
            KeSetEvent(&AoeSignal_, 0, FALSE);
            KeWaitForSingleObject(
                AoeThreadObj_,
                Executive,
                KernelMode,
                FALSE,
                NULL
              );
            ObDereferenceObject(AoeThreadObj_);
            ZwClose(AoeThreadHandle_);
            /* Prevent AoeCleanup() from trying to stop the thread. */
            AoeThreadHandle_ = NULL;
            /* Delete. */
            IoDeleteDevice(AoeBusMain.Fdo);
            /* Disassociate. */
            AoeBusMain.Fdo = NULL;
          }
        return status;
      }
    /* WvDevFromDevObj() checks for a NULL dev_obj */
    dev = WvDevFromDevObj(dev_obj);
    /* Check that the device exists. */
    if (!dev || dev->State == WvDevStateDeleted)
      return WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
    /* Use the disk routine. */
    return disk_pnp__dispatch(dev, irp, code);
  }

static UCHAR STDCALL AoeDiskUnitNum_(IN WV_SP_DISK_T disk) {
    AOE_SP_DISK_ aoe_disk = CONTAINING_RECORD(
        disk,
        AOE_S_DISK_,
        disk[0]
      );

    /* Possible precision loss. */
    return (UCHAR) WvlBusGetNodeNum(&aoe_disk->disk->Dev->BusNode);
  }
