/**
 * Copyright 2006-2008, V.
 * Portions copyright (C) 2009 Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 * For contact information, see http://winaoe.org/
 *
 * This file is part of WinAoE.
 *
 * WinAoE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinAoE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinAoE.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Protocol specifics
 *
 */

#include "portable.h"
#include <ntddk.h>
#include <ndis.h>
#include <ntddndis.h>
#include "protocol.h"
#include "driver.h"
#include "aoe.h"
#include "debug.h"

/* in this file */
static VOID STDCALL ProtocolOpenAdapterComplete ( IN NDIS_HANDLE
					   ProtocolBindingContext,
					   IN NDIS_STATUS Status,
					   IN NDIS_STATUS OpenErrorStatus );
static VOID STDCALL ProtocolCloseAdapterComplete ( IN NDIS_HANDLE
					    ProtocolBindingContext,
					    IN NDIS_STATUS Status );
static VOID STDCALL ProtocolSendComplete ( IN NDIS_HANDLE ProtocolBindingContext,
				    IN PNDIS_PACKET Packet,
				    IN NDIS_STATUS Status );
static VOID STDCALL ProtocolTransferDataComplete ( IN NDIS_HANDLE
					    ProtocolBindingContext,
					    IN PNDIS_PACKET Packet,
					    IN NDIS_STATUS Status,
					    IN UINT BytesTransferred );
static VOID STDCALL ProtocolResetComplete ( IN NDIS_HANDLE ProtocolBindingContext,
				     IN NDIS_STATUS Status );
static VOID STDCALL ProtocolRequestComplete ( IN NDIS_HANDLE ProtocolBindingContext,
				       IN PNDIS_REQUEST NdisRequest,
				       IN NDIS_STATUS Status );
static NDIS_STATUS STDCALL ProtocolReceive ( IN NDIS_HANDLE ProtocolBindingContext,
				      IN NDIS_HANDLE MacReceiveContext,
				      IN PVOID HeaderBuffer,
				      IN UINT HeaderBufferSize,
				      IN PVOID LookAheadBuffer,
				      IN UINT LookaheadBufferSize,
				      IN UINT PacketSize );
static VOID STDCALL ProtocolReceiveComplete ( IN NDIS_HANDLE ProtocolBindingContext );
static VOID STDCALL ProtocolStatus ( IN NDIS_HANDLE ProtocolBindingContext,
			      IN NDIS_STATUS GeneralStatus,
			      IN PVOID StatusBuffer,
			      IN UINT StatusBufferSize );
static VOID STDCALL ProtocolStatusComplete ( IN NDIS_HANDLE ProtocolBindingContext );
static VOID STDCALL ProtocolBindAdapter ( OUT PNDIS_STATUS Status,
				   IN NDIS_HANDLE BindContext,
				   IN PNDIS_STRING DeviceName,
				   IN PVOID SystemSpecific1,
				   IN PVOID SystemSpecific2 );
static VOID STDCALL ProtocolUnbindAdapter ( OUT PNDIS_STATUS Status,
				     IN NDIS_HANDLE ProtocolBindingContext,
				     IN NDIS_HANDLE UnbindContext );
static NDIS_STATUS STDCALL ProtocolPnPEvent ( IN NDIS_HANDLE ProtocolBindingContext,
				       IN PNET_PNP_EVENT NetPnPEvent );
static PCHAR STDCALL NetEventString ( IN NET_PNP_EVENT_CODE NetEvent );

#ifdef _MSC_VER
#pragma pack(1)
#endif
typedef struct _HEADER {
    UCHAR DestinationMac[6];
    UCHAR SourceMac[6];
    USHORT Protocol;
    UCHAR Data[];
} __attribute__ ( ( __packed__ ) ) HEADER, *PHEADER;
#ifdef _MSC_VER
#pragma pack()
#endif

typedef struct _BINDINGCONTEXT {
    BOOLEAN Active;
    UCHAR Mac[6];
    UINT MTU;
    NDIS_STATUS Status;
    NDIS_HANDLE PacketPoolHandle;
    NDIS_HANDLE BufferPoolHandle;
    NDIS_HANDLE BindingHandle;
    KEVENT Event;
    BOOLEAN OutstandingRequest;
    PWCHAR AdapterName;
    PWCHAR DeviceName;
    struct _BINDINGCONTEXT *Next;
} BINDINGCONTEXT, *PBINDINGCONTEXT;

KEVENT ProtocolStopEvent;
KSPIN_LOCK SpinLock;
PBINDINGCONTEXT BindingContextList = NULL;
NDIS_HANDLE ProtocolHandle = NULL;

NTSTATUS STDCALL ProtocolStart (  )
{
    NDIS_STATUS Status;
    NDIS_STRING ProtocolName;
    NDIS_PROTOCOL_CHARACTERISTICS ProtocolCharacteristics;

    DBG ( "Entry\n" );
    KeInitializeEvent ( &ProtocolStopEvent, SynchronizationEvent, FALSE );
    KeInitializeSpinLock ( &SpinLock );

    RtlInitUnicodeString ( &ProtocolName, L"AoE" );
    NdisZeroMemory ( &ProtocolCharacteristics,
		     sizeof ( NDIS_PROTOCOL_CHARACTERISTICS ) );
    ProtocolCharacteristics.MajorNdisVersion = 5;
    ProtocolCharacteristics.MinorNdisVersion = 0;
    ProtocolCharacteristics.Name = ProtocolName;
    ProtocolCharacteristics.OpenAdapterCompleteHandler =
	ProtocolOpenAdapterComplete;
    ProtocolCharacteristics.CloseAdapterCompleteHandler =
	ProtocolCloseAdapterComplete;
    ProtocolCharacteristics.SendCompleteHandler = ProtocolSendComplete;
    ProtocolCharacteristics.TransferDataCompleteHandler =
	ProtocolTransferDataComplete;
    ProtocolCharacteristics.ResetCompleteHandler = ProtocolResetComplete;
    ProtocolCharacteristics.RequestCompleteHandler = ProtocolRequestComplete;
    ProtocolCharacteristics.ReceiveHandler = ProtocolReceive;
    ProtocolCharacteristics.ReceiveCompleteHandler = ProtocolReceiveComplete;
    ProtocolCharacteristics.StatusHandler = ProtocolStatus;
    ProtocolCharacteristics.StatusCompleteHandler = ProtocolStatusComplete;
    ProtocolCharacteristics.BindAdapterHandler = ProtocolBindAdapter;
    ProtocolCharacteristics.UnbindAdapterHandler = ProtocolUnbindAdapter;
    ProtocolCharacteristics.PnPEventHandler = ProtocolPnPEvent;
    NdisRegisterProtocol ( ( PNDIS_STATUS ) & Status, &ProtocolHandle,
			   &ProtocolCharacteristics,
			   sizeof ( NDIS_PROTOCOL_CHARACTERISTICS ) );
    return Status;
}

VOID STDCALL ProtocolStop (  )
{
    NDIS_STATUS Status;

    DBG ( "Entry\n" );
    KeResetEvent ( &ProtocolStopEvent );
    NdisDeregisterProtocol ( &Status, ProtocolHandle );
    if ( !NT_SUCCESS ( Status ) )
	Error ( "NdisDeregisterProtocol", Status );
    if ( BindingContextList != NULL )
	KeWaitForSingleObject ( &ProtocolStopEvent, Executive,
				KernelMode, FALSE, NULL );
}

BOOLEAN STDCALL ProtocolSearchNIC ( IN PUCHAR Mac )
{
    PBINDINGCONTEXT Context = BindingContextList;

    while ( Context != NULL ) {
	if ( RtlCompareMemory ( Mac, Context->Mac, 6 ) == 6 )
	    break;
	Context = Context->Next;
    }
    if ( Context != NULL )
	return TRUE;
    return FALSE;
}

ULONG STDCALL ProtocolGetMTU ( IN PUCHAR Mac )
{
    PBINDINGCONTEXT Context = BindingContextList;

    while ( Context != NULL ) {
	if ( RtlCompareMemory ( Mac, Context->Mac, 6 ) == 6 )
	    break;
	Context = Context->Next;
    }
    if ( Context == NULL )
	return 0;
    return Context->MTU;
}

BOOLEAN STDCALL ProtocolSend ( IN PUCHAR SourceMac, IN PUCHAR DestinationMac,
			       IN PUCHAR Data, IN ULONG DataSize,
			       IN PVOID PacketContext )
{
    PBINDINGCONTEXT Context = BindingContextList;
    NDIS_STATUS Status;
    PNDIS_PACKET Packet;
    PNDIS_BUFFER Buffer;
    PHEADER DataBuffer;
#if defined(DEBUGALLPROTOCOLCALLS)
    DBG ( "Entry\n" );
#endif

    if ( RtlCompareMemory ( SourceMac, "\xff\xff\xff\xff\xff\xff", 6 ) == 6 ) {
	while ( Context != NULL ) {
	    ProtocolSend ( Context->Mac, DestinationMac, Data,
			   DataSize, NULL );
	    Context = Context->Next;
	}
	return TRUE;
    }

    while ( Context != NULL ) {
	if ( RtlCompareMemory ( SourceMac, Context->Mac, 6 ) == 6 )
	    break;
	Context = Context->Next;
    }
    if ( Context == NULL ) {
	DBG ( "Can't find NIC %02x:%02x:%02x:%02x:%02x:%02x\n",
	      SourceMac[0], SourceMac[1], SourceMac[2], SourceMac[3],
	      SourceMac[4], SourceMac[5] );
	return FALSE;
    }

    if ( DataSize > Context->MTU ) {
	DBG ( "Tried to send oversized packet (size: %d, MTU:)\n",
	      DataSize, Context->MTU );
	return FALSE;
    }

    if ( ( DataBuffer =
	   ( PHEADER ) ExAllocatePool ( NonPagedPool,
					( sizeof ( HEADER ) +
					  DataSize ) ) ) == NULL ) {
	DBG ( "ExAllocatePool DataBuffer\n" );
	return FALSE;
    }

    RtlCopyMemory ( DataBuffer->SourceMac, SourceMac, 6 );
    RtlCopyMemory ( DataBuffer->DestinationMac, DestinationMac, 6 );
    DataBuffer->Protocol = htons ( AOEPROTOCOLID );
    RtlCopyMemory ( DataBuffer->Data, Data, DataSize );

    NdisAllocatePacket ( &Status, &Packet, Context->PacketPoolHandle );
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolSend NdisAllocatePacket", Status );
	ExFreePool ( DataBuffer );
	return FALSE;
    }

    NdisAllocateBuffer ( &Status, &Buffer, Context->BufferPoolHandle,
			 DataBuffer, ( sizeof ( HEADER ) + DataSize ) );
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolSend NdisAllocateBuffer", Status );
	NdisFreePacket ( Packet );
	ExFreePool ( DataBuffer );
	return FALSE;
    }

    NdisChainBufferAtFront ( Packet, Buffer );
    *( PVOID * ) Packet->ProtocolReserved = PacketContext;
    NdisSend ( &Status, Context->BindingHandle, Packet );
    if ( Status != NDIS_STATUS_PENDING )
	ProtocolSendComplete ( Context, Packet, Status );
    return TRUE;
}

static VOID STDCALL ProtocolOpenAdapterComplete ( IN NDIS_HANDLE
					   ProtocolBindingContext,
					   IN NDIS_STATUS Status,
					   IN NDIS_STATUS OpenErrorStatus )
{
#if !defined(DEBUGMOSTPROTOCOLCALLS) && !defined(DEBUGALLPROTOCOLCALLS)
    if ( !NT_SUCCESS ( Status ) )
#endif
	DBG ( "0x%08x 0x%08x\n", Status, OpenErrorStatus );
}

static VOID STDCALL ProtocolCloseAdapterComplete ( IN NDIS_HANDLE
					    ProtocolBindingContext,
					    IN NDIS_STATUS Status )
{
#if !defined(DEBUGMOSTPROTOCOLCALLS) && !defined(DEBUGALLPROTOCOLCALLS)
    if ( !NT_SUCCESS ( Status ) )
#endif
	Error ( "ProtocolCloseAdapterComplete", Status );
}

static VOID STDCALL ProtocolSendComplete ( IN NDIS_HANDLE ProtocolBindingContext,
				    IN PNDIS_PACKET Packet,
				    IN NDIS_STATUS Status )
{
    PNDIS_BUFFER Buffer;
    PUCHAR DataBuffer;
#ifndef DEBUGALLPROTOCOLCALLS
    if ( !NT_SUCCESS ( Status ) && Status != NDIS_STATUS_NO_CABLE )
#endif
	Error ( "ProtocolSendComplete", Status );

    NdisUnchainBufferAtFront ( Packet, &Buffer );
    if ( Buffer != NULL ) {
	DataBuffer = NdisBufferVirtualAddress ( Buffer );
	ExFreePool ( DataBuffer );
	NdisFreeBuffer ( Buffer );
    } else {
	DBG ( "Buffer == NULL\n" );
    }
    NdisFreePacket ( Packet );
}

static VOID STDCALL ProtocolTransferDataComplete ( IN NDIS_HANDLE
					    ProtocolBindingContext,
					    IN PNDIS_PACKET Packet,
					    IN NDIS_STATUS Status,
					    IN UINT BytesTransferred )
{
    PNDIS_BUFFER Buffer;
    PHEADER Header = NULL;
    PUCHAR Data = NULL;
    UINT HeaderSize, DataSize;
#ifndef DEBUGALLPROTOCOLCALLS
    if ( !NT_SUCCESS ( Status ) )
#endif
	Error ( "ProtocolTransferDataComplete", Status );

    NdisUnchainBufferAtFront ( Packet, &Buffer );
    if ( Buffer != NULL ) {
	NdisQueryBuffer ( Buffer, &Data, &DataSize );
	NdisFreeBuffer ( Buffer );
    } else {
	DBG ( "Data (front) Buffer == NULL\n" );
    }
    NdisUnchainBufferAtBack ( Packet, &Buffer );
    if ( Buffer != NULL ) {
	NdisQueryBuffer ( Buffer, &Header, &HeaderSize );
	NdisFreeBuffer ( Buffer );
    } else {
	DBG ( "Header (back) Buffer == NULL\n" );
    }
    if ( Header != NULL && Data != NULL )
	AoEReply ( Header->SourceMac, Header->DestinationMac, Data, DataSize );
    if ( Header != NULL )
	ExFreePool ( Header );
    if ( Data != NULL )
	ExFreePool ( Data );
    NdisFreePacket ( Packet );
}

static VOID STDCALL ProtocolResetComplete ( IN NDIS_HANDLE ProtocolBindingContext,
				     IN NDIS_STATUS Status )
{
#if !defined(DEBUGMOSTPROTOCOLCALLS) && !defined(DEBUGALLPROTOCOLCALLS)
    if ( !NT_SUCCESS ( Status ) )
#endif
	Error ( "ProtocolResetComplete", Status );
}

static VOID STDCALL ProtocolRequestComplete ( IN NDIS_HANDLE ProtocolBindingContext,
				       IN PNDIS_REQUEST NdisRequest,
				       IN NDIS_STATUS Status )
{
    PBINDINGCONTEXT Context = ( PBINDINGCONTEXT ) ProtocolBindingContext;
#if !defined(DEBUGMOSTPROTOCOLCALLS) && !defined(DEBUGALLPROTOCOLCALLS)
    if ( !NT_SUCCESS ( Status ) )
#endif
	Error ( "ProtocolRequestComplete", Status );

    Context->Status = Status;
    KeSetEvent ( &Context->Event, 0, FALSE );
}

static NDIS_STATUS STDCALL ProtocolReceive ( IN NDIS_HANDLE ProtocolBindingContext,
				      IN NDIS_HANDLE MacReceiveContext,
				      IN PVOID HeaderBuffer,
				      IN UINT HeaderBufferSize,
				      IN PVOID LookAheadBuffer,
				      IN UINT LookaheadBufferSize,
				      IN UINT PacketSize )
{
    PBINDINGCONTEXT Context = ( PBINDINGCONTEXT ) ProtocolBindingContext;
    NDIS_STATUS Status;
    PNDIS_PACKET Packet;
    PNDIS_BUFFER Buffer;
    PHEADER Header;
    PUCHAR HeaderCopy, Data;
    UINT BytesTransferred;
#ifdef DEBUGALLPROTOCOLCALLS
    DBG ( "Entry\n" );
#endif

    if ( HeaderBufferSize != sizeof ( HEADER ) ) {
	DBG ( "HeaderBufferSize %d != sizeof(HEADER) %d\n" );
	return NDIS_STATUS_NOT_ACCEPTED;
    }
    Header = ( PHEADER ) HeaderBuffer;
    if ( ntohs ( Header->Protocol ) != AOEPROTOCOLID )
	return NDIS_STATUS_NOT_ACCEPTED;

    if ( LookaheadBufferSize == PacketSize ) {
	AoEReply ( Header->SourceMac, Header->DestinationMac,
		   LookAheadBuffer, PacketSize );
	return NDIS_STATUS_SUCCESS;
    }

    if ( ( HeaderCopy =
	   ( PUCHAR ) ExAllocatePool ( NonPagedPool,
				       HeaderBufferSize ) ) == NULL ) {
	DBG ( "ExAllocatePool HeaderCopy\n" );
	return NDIS_STATUS_NOT_ACCEPTED;
    }
    RtlCopyMemory ( HeaderCopy, HeaderBuffer, HeaderBufferSize );
    if ( ( Data =
	   ( PUCHAR ) ExAllocatePool ( NonPagedPool, PacketSize ) ) == NULL ) {
	DBG ( "ExAllocatePool HeaderData\n" );
	ExFreePool ( HeaderCopy );
	return NDIS_STATUS_NOT_ACCEPTED;
    }
    NdisAllocatePacket ( &Status, &Packet, Context->PacketPoolHandle );
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolReceive NdisAllocatePacket", Status );
	ExFreePool ( Data );
	ExFreePool ( HeaderCopy );
	return NDIS_STATUS_NOT_ACCEPTED;
    }

    NdisAllocateBuffer ( &Status, &Buffer, Context->BufferPoolHandle, Data,
			 PacketSize );
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolReceive NdisAllocateBuffer (Data)", Status );
	NdisFreePacket ( Packet );
	ExFreePool ( Data );
	ExFreePool ( HeaderCopy );
	return NDIS_STATUS_NOT_ACCEPTED;
    }
    NdisChainBufferAtFront ( Packet, Buffer );

    NdisAllocateBuffer ( &Status, &Buffer, Context->BufferPoolHandle,
			 HeaderCopy, PacketSize );
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolReceive NdisAllocateBuffer (HeaderCopy)", Status );
	NdisUnchainBufferAtFront ( Packet, &Buffer );
	NdisFreeBuffer ( Buffer );
	NdisFreePacket ( Packet );
	ExFreePool ( Data );
	ExFreePool ( HeaderCopy );
	return NDIS_STATUS_NOT_ACCEPTED;
    }
    NdisChainBufferAtBack ( Packet, Buffer );

    NdisTransferData ( &Status, Context->BindingHandle, MacReceiveContext,
		       0, PacketSize, Packet, &BytesTransferred );
    if ( Status != NDIS_STATUS_PENDING )
	ProtocolTransferDataComplete ( ProtocolBindingContext, Packet,
				       Status, BytesTransferred );
    return Status;
}

static VOID STDCALL ProtocolReceiveComplete ( IN NDIS_HANDLE ProtocolBindingContext )
{
#ifdef DEBUGALLPROTOCOLCALLS
    DBG ( "Entry\n" );
#endif
}

static VOID STDCALL ProtocolStatus ( IN NDIS_HANDLE ProtocolBindingContext,
			      IN NDIS_STATUS GeneralStatus,
			      IN PVOID StatusBuffer, IN UINT StatusBufferSize )
{
#if !defined(DEBUGMOSTPROTOCOLCALLS) && !defined(DEBUGALLPROTOCOLCALLS)
    if ( !NT_SUCCESS ( GeneralStatus ) )
#endif
	Error ( "ProtocolStatus", GeneralStatus );
}

static VOID STDCALL ProtocolStatusComplete ( IN NDIS_HANDLE ProtocolBindingContext )
{
#if defined(DEBUGMOSTPROTOCOLCALLS) || defined(DEBUGALLPROTOCOLCALLS)
    DBG ( "Entry\n" );
#endif
}

static VOID STDCALL ProtocolBindAdapter ( OUT PNDIS_STATUS StatusOut,
				   IN NDIS_HANDLE BindContext,
				   IN PNDIS_STRING DeviceName,
				   IN PVOID SystemSpecific1,
				   IN PVOID SystemSpecific2 )
{
    PBINDINGCONTEXT Context, Walker;
    NDIS_STATUS Status;
    NDIS_STATUS OpenErrorStatus;
    UINT SelectedMediumIndex, MTU;
    NDIS_MEDIUM MediumArray[] = { NdisMedium802_3 };
    NDIS_STRING AdapterInstanceName;
    NDIS_REQUEST Request;
    ULONG InformationBuffer = NDIS_PACKET_TYPE_DIRECTED;
    UCHAR Mac[6];
    KIRQL Irql;
#if defined(DEBUGMOSTPROTOCOLCALLS) || defined(DEBUGALLPROTOCOLCALLS)
    DBG ( "Entry\n" );
#endif

    if ( ( Context =
	   ( NDIS_HANDLE ) ExAllocatePool ( NonPagedPool,
					    sizeof ( BINDINGCONTEXT ) ) ) ==
	 NULL ) {
	DBG ( "ExAllocatePool Context\n" );
	*StatusOut = NDIS_STATUS_RESOURCES;
	return;
    }
    Context->Next = NULL;
    KeInitializeEvent ( &Context->Event, SynchronizationEvent, FALSE );

    NdisAllocatePacketPool ( &Status, &Context->PacketPoolHandle, POOLSIZE,
			     ( 4 * sizeof ( VOID * ) ) );
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolBindAdapter NdisAllocatePacketPool", Status );
	ExFreePool ( Context );
	*StatusOut = NDIS_STATUS_RESOURCES;
	return;
    }

    NdisAllocateBufferPool ( &Status, &Context->BufferPoolHandle, POOLSIZE );
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolBindAdapter NdisAllocateBufferPool", Status );
	NdisFreePacketPool ( Context->PacketPoolHandle );
	ExFreePool ( Context );
	*StatusOut = NDIS_STATUS_RESOURCES;
	return;
    }

    NdisOpenAdapter ( &Status, &OpenErrorStatus, &Context->BindingHandle,
		      &SelectedMediumIndex, MediumArray,
		      ( sizeof ( MediumArray ) / sizeof ( NDIS_MEDIUM ) ),
		      ProtocolHandle, Context, DeviceName, 0, NULL );
    if ( !NT_SUCCESS ( Status ) ) {
	DBG ( "NdisOpenAdapter 0x%lx 0x%lx\n", Status, OpenErrorStatus );
	NdisFreePacketPool ( Context->PacketPoolHandle );
	NdisFreeBufferPool ( Context->BufferPoolHandle );
	ExFreePool ( Context->AdapterName );
	ExFreePool ( Context->DeviceName );
	ExFreePool ( Context );
	*StatusOut = Status;
	return;
    }
    if ( SelectedMediumIndex != 0 )
	DBG ( "NdisOpenAdapter SelectedMediumIndex: %d\n",
	      SelectedMediumIndex );

    Context->AdapterName = NULL;
    if ( NT_SUCCESS
	 ( Status =
	   NdisQueryAdapterInstanceName ( &AdapterInstanceName,
					  Context->BindingHandle ) ) ) {
	if ( ( Context->AdapterName =
	       ( PWCHAR ) ExAllocatePool ( NonPagedPool,
					   AdapterInstanceName.Length +
					   sizeof ( WCHAR ) ) ) == NULL ) {
	    DBG ( "ExAllocatePool AdapterName\n" );
	} else {
	    RtlZeroMemory ( Context->AdapterName,
			    AdapterInstanceName.Length + sizeof ( WCHAR ) );
	    RtlCopyMemory ( Context->AdapterName,
			    AdapterInstanceName.Buffer,
			    AdapterInstanceName.Length );
	}
	NdisFreeMemory ( AdapterInstanceName.Buffer, 0, 0 );
    } else {
	Error ( "ProtocolBindAdapter NdisQueryAdapterInstanceName", Status );
    }

    Context->DeviceName = NULL;
    if ( DeviceName->Length > 0 ) {
	if ( ( Context->DeviceName =
	       ( PWCHAR ) ExAllocatePool ( NonPagedPool,
					   DeviceName->Length +
					   sizeof ( WCHAR ) ) ) == NULL ) {
	    DBG ( "ExAllocatePool DeviceName\n" );
	} else {
	    RtlZeroMemory ( Context->DeviceName,
			    DeviceName->Length + sizeof ( WCHAR ) );
	    RtlCopyMemory ( Context->DeviceName, DeviceName->Buffer,
			    DeviceName->Length );
	}
    }

    if ( Context->AdapterName != NULL )
	DBG ( "Adapter: %S\n", Context->AdapterName );
    if ( Context->DeviceName != NULL )
	DBG ( "Device Name: %S\n", Context->DeviceName );
    if ( ( Context->AdapterName == NULL )
	 && ( Context->DeviceName == NULL ) )
	DBG ( "Unnamed Adapter...\n" );

    Request.RequestType = NdisRequestQueryInformation;
    Request.DATA.QUERY_INFORMATION.Oid = OID_802_3_CURRENT_ADDRESS;
    Request.DATA.QUERY_INFORMATION.InformationBuffer = Mac;
    Request.DATA.QUERY_INFORMATION.InformationBufferLength = sizeof ( Mac );

    KeResetEvent ( &Context->Event );
    NdisRequest ( &Status, Context->BindingHandle, &Request );
    if ( Status == NDIS_STATUS_PENDING ) {
	KeWaitForSingleObject ( &Context->Event, Executive, KernelMode,
				FALSE, NULL );
	Status = Context->Status;
    }
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolBindAdapter NdisRequest (Mac)", Status );
    } else {
	RtlCopyMemory ( Context->Mac, Mac, 6 );
	DBG ( "Mac: %02x:%02x:%02x:%02x:%02x:%02x\n", Mac[0],
	      Mac[1], Mac[2], Mac[3], Mac[4], Mac[5] );
    }

    Request.RequestType = NdisRequestQueryInformation;
    Request.DATA.QUERY_INFORMATION.Oid = OID_GEN_MAXIMUM_FRAME_SIZE;
    Request.DATA.QUERY_INFORMATION.InformationBuffer = &MTU;
    Request.DATA.QUERY_INFORMATION.InformationBufferLength = sizeof ( MTU );

    KeResetEvent ( &Context->Event );
    NdisRequest ( &Status, Context->BindingHandle, &Request );
    if ( Status == NDIS_STATUS_PENDING ) {
	KeWaitForSingleObject ( &Context->Event, Executive, KernelMode,
				FALSE, NULL );
	Status = Context->Status;
    }
    if ( !NT_SUCCESS ( Status ) ) {
	Error ( "ProtocolBindAdapter NdisRequest (MTU)", Status );
    } else {
	Context->MTU = MTU;
	DBG ( "MTU: %d\n", MTU );
    }

    Request.RequestType = NdisRequestSetInformation;
    Request.DATA.SET_INFORMATION.Oid = OID_GEN_CURRENT_PACKET_FILTER;
    Request.DATA.SET_INFORMATION.InformationBuffer = &InformationBuffer;
    Request.DATA.SET_INFORMATION.InformationBufferLength =
	sizeof ( InformationBuffer );

    KeResetEvent ( &Context->Event );
    NdisRequest ( &Status, Context->BindingHandle, &Request );
    if ( Status == NDIS_STATUS_PENDING ) {
	KeWaitForSingleObject ( &Context->Event, Executive, KernelMode,
				FALSE, NULL );
	Status = Context->Status;
    }
    if ( !NT_SUCCESS ( Status ) )
	Error ( "ProtocolBindAdapter NdisRequest (filter)", Status );

    KeAcquireSpinLock ( &SpinLock, &Irql );
    if ( BindingContextList == NULL ) {
	BindingContextList = Context;
    } else {
	for ( Walker = BindingContextList; Walker->Next != NULL;
	      Walker = Walker->Next );
	Walker->Next = Context;
    }
    KeReleaseSpinLock ( &SpinLock, Irql );

    AoEResetProbe (  );
    *StatusOut = NDIS_STATUS_SUCCESS;
}

static VOID STDCALL ProtocolUnbindAdapter ( OUT PNDIS_STATUS StatusOut,
				     IN NDIS_HANDLE ProtocolBindingContext,
				     IN NDIS_HANDLE UnbindContext )
{
    PBINDINGCONTEXT Context = ( PBINDINGCONTEXT ) ProtocolBindingContext;
    PBINDINGCONTEXT Walker, PreviousContext;
    NDIS_STATUS Status;
    KIRQL Irql;
#if defined(DEBUGMOSTPROTOCOLCALLS) || defined(DEBUGALLPROTOCOLCALLS)
    DBG ( "Entry\n" );
#endif

    PreviousContext = NULL;
    KeAcquireSpinLock ( &SpinLock, &Irql );
    for ( Walker = BindingContextList; Walker != Context && Walker != NULL;
	  Walker = Walker->Next )
	PreviousContext = Walker;
    if ( Walker == NULL ) {
	DBG ( "Context not found in BindingContextList!!\n" );
	KeReleaseSpinLock ( &SpinLock, Irql );
	return;
    }
    if ( PreviousContext == NULL ) {
	BindingContextList = Walker->Next;
    } else {
	PreviousContext->Next = Walker->Next;
    }
    KeReleaseSpinLock ( &SpinLock, Irql );

    NdisCloseAdapter ( &Status, Context->BindingHandle );
    if ( !NT_SUCCESS ( Status ) )
	Error ( "ProtocolUnbindAdapter NdisCloseAdapter", Status );
    NdisFreePacketPool ( Context->PacketPoolHandle );
    NdisFreeBufferPool ( Context->BufferPoolHandle );
    ExFreePool ( Context );
    if ( BindingContextList == NULL )
	KeSetEvent ( &ProtocolStopEvent, 0, FALSE );
    *StatusOut = NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS STDCALL ProtocolPnPEvent ( IN NDIS_HANDLE ProtocolBindingContext,
				       IN PNET_PNP_EVENT NetPnPEvent )
{
#if defined(DEBUGMOSTPROTOCOLCALLS) || defined(DEBUGALLPROTOCOLCALLS)
    DBG ( "%s\n", NetEventString ( NetPnPEvent->NetEvent ) );
#endif
    if ( ProtocolBindingContext == NULL
	 && NetPnPEvent->NetEvent == NetEventReconfigure ) {
#ifdef _MSC_VER
	NdisReEnumerateProtocolBindings ( ProtocolHandle );
#else
	DBG ( "No vector to NdisReEnumerateProtocolBindings\n" );
#endif
    }
    if ( NetPnPEvent->NetEvent == NetEventQueryRemoveDevice ) {
	return NDIS_STATUS_FAILURE;
    } else {
	return NDIS_STATUS_SUCCESS;
    }
}

static PCHAR STDCALL NetEventString ( IN NET_PNP_EVENT_CODE NetEvent )
{
    switch ( NetEvent ) {
    case NetEventSetPower:
	return "NetEventSetPower";
    case NetEventQueryPower:
	return "NetEventQueryPower";
    case NetEventQueryRemoveDevice:
	return "NetEventQueryRemoveDevice";
    case NetEventCancelRemoveDevice:
	return "NetEventCancelRemoveDevice";
    case NetEventReconfigure:
	return "NetEventReconfigure";
    case NetEventBindList:
	return "NetEventBindList";
    case NetEventBindsComplete:
	return "NetEventBindsComplete";
    case NetEventPnPCapabilities:
	return "NetEventPnPCapabilities";
    default:
	return "NetEventUnknown";
    }
}
