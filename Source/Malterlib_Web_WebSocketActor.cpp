// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Container/PagedByteVector>
#include <Mib/Container/BitArray>
#include <Mib/Stream/BinaryStorage>
#include <Mib/Concurrency/IoCompletionOpTracker>
#include <Mib/Core/IoSubSystem>

#include <Mib/Web/HTTP/Request>
#include <Mib/Web/HTTP/Response>

#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Cryptography/RandomData>
#include <Mib/Cryptography/Exception>
#include <Mib/Encoding/Base64>

#include <Mib/Encoding/Json>

#include <deque>

#include "Malterlib_Web_WebSocket.h"

#if defined(DCompiler_clang) && !defined(DPlatformFamily_Emscripten)
#	define DEnableVector
#endif

#ifdef DEnableVector
using vec4uint32 = uint32 __attribute__((ext_vector_type(4)));
#endif

namespace NMib::NWeb
{
	static ch8 const gs_PingMessageData[] = "WdI6Q6-HvOxlK5Vc";

	using CBinaryStreamPagedByteVector = NContainer::TCBinaryStreamPagedByteVector<NStream::CBinaryStreamBigEndian>;

	namespace
	{
		enum EState
		{
			EState_None
			, EState_HeaderReceived
			, EState_Connected
			, EState_Disconnecting
			, EState_Disconnected
		};

		enum EOpcode : uint8
		{
			EOpcode_ContinuationFrame = 0
			, EOpcode_TextFrame = 1
			, EOpcode_BinaryFrame = 2
			, EOpcode_ConnectionClose = 8
			, EOpcode_Ping = 9
			, EOpcode_Pong = 10
		};

		struct CHeader
		{
			uint8 m_bFinalFragment:1;
			uint8 m_bReserver0:1;
			uint8 m_bReserver1:1;
			uint8 m_bReserver2:1;
			uint8 m_Opcode:4;
			uint8 m_bMask:1;
			uint8 m_PayloadLength:7;
		};

		struct CMessage
		{
			CMessage()
			{
				NMemory::fg_MemClear(m_Mask); // MSVC does not support inline initializing of array
			}

			// Moves whatever contiguous assembly has accumulated onto the end of the message
			// storage, so view appends that follow land after it in arrival order. Only legal
			// between frames — a masked frame being direct read owes its mask pass positions
			// inside m_Data
			void f_FlushDataToStorage()
			{
				if (m_Data.f_IsEmpty())
					return;

				// As a shared segment, not an adopted vector: consumers slice payloads out of
				// the message as views, and only shared segments can hand out sub views
				m_Storage.f_AppendShared(NContainer::CSharedByteVector(fg_Move(m_Data)));
				m_Data = NContainer::CIOByteVector();
			}

			uint64 m_Length = 0;
			NContainer::CIOByteVector m_Data;
			// Binary payload as it is delivered: views of the receive buffers interleaved
			// with flushed contiguous assembly; empty until a completion stream appends the
			// first view
			NStream::CBinaryStorage m_Storage;
			umint m_Position = 0;
			uint8 m_Mask[4];
			CHeader m_Header;
			bool m_bHeaderFinished = false;
		};

		// Keeps the owner of a viewed payload alive until every frame referencing it has been
		// sent. The owner type does not matter, so a message can reference bytes belonging to
		// a shared buffer, a segmented storage, a text buffer set or a string in place
		struct CPayloadOwner
		{
			virtual ~CPayloadOwner() = default;
		};

		template <typename t_COwner>
		struct TCPayloadOwner final : public CPayloadOwner
		{
			TCPayloadOwner(t_COwner &&_Owner)
				: m_Owner(fg_Move(_Owner))
			{
			}

			t_COwner m_Owner;
		};

		struct COutgoingMessage
		{
			~COutgoingMessage()
			{
				if (m_Promise)
					m_Promise->f_SetException(DMibErrorInstance("Outgoing message abandoned"));
			}

			NStorage::TCSharedPointer<NContainer::CIOByteVector const> m_pData;
			NStorage::TCOptionalClearOnMove<NConcurrency::TCPromise<void>> m_Promise;

			// View payload: the message references its payload spans in place and frames are
			// emitted lazily, one per write pass, instead of being copied into fragments up
			// front. Unmasked connections send the spans by reference; masking clients copy
			// and mask each frame into the arena at emit so the shared payload stays untouched
			NStorage::TCSharedPointer<CPayloadOwner> m_pOwner;
			NContainer::TCVector<NSys::CIoSpan> m_Spans;
			umint m_nTotalBytes = 0;
			umint m_iPayloadSent = 0;
			umint m_iSpan = 0;
			umint m_iSpanOffset = 0;

			EOpcode m_Opcode;
			bool m_bFinished = false;
			bool m_bView = false;
		};

		// One entry of the outgoing byte stream: either the next bytes of the copy arena
		// (m_OutgoingData) or a view into a queued message's shared payload
		struct COutgoingSegment
		{
			enum class EKind : uint8
			{
				mc_Arena
				, mc_View
			};

			EKind m_Kind = EKind::mc_Arena;
			umint m_nBytes = 0;
			umint m_iSent = 0; // partial send progress; only ever nonzero on a view at the head
			uint8 const *m_pData = nullptr; // EKind::mc_View only

			// Keep the viewed payload alive until the segment is fully sent
			NStorage::TCSharedPointer<CPayloadOwner> m_pOwnerKeepAlive;
		};

		struct COutgoingDataPromise
		{
			COutgoingDataPromise() = default;
			COutgoingDataPromise(COutgoingDataPromise &&) = default;

			~COutgoingDataPromise()
			{
				if (m_Promise)
					m_Promise->f_SetException(DMibErrorInstance("Outgoing message abandoned"));
			}

			uint64 m_Position = 0;
			NStorage::TCOptionalClearOnMove<NConcurrency::TCPromise<void>> m_Promise;
		};

		constexpr static umint gc_OutgoingPageSize = 2048;
		constexpr static umint gc_IncomingPageSize = 2048;

		// Bytes requested per receive call; receives land in the page tail when it has
		// room for at least half of this, otherwise through a stack bounce buffer. Kept above
		// one whole TLS record with its framing, so a transport that decrypts into this buffer
		// always has room for the next record instead of falling back for every one of them
		constexpr static umint gc_ReceiveChunkSize = 24576;

		// Below this size the copy into the arena is cheaper than per view segment
		// bookkeeping and scattered framing
		constexpr static umint gc_CopySmallMessageThreshold = 1024;

		// Data frame payloads at least this large are received straight into the message
		// buffer instead of passing through the incoming pages. Below this the buffered
		// prefix dominates the frame and the direct read machinery does not pay off
		constexpr static umint gc_DirectReadThreshold = 4 * gc_ReceiveChunkSize;

		// A one entry span vector allocated to exactly one entry: the vector minimum size
		// would otherwise reserve sixteen
		NContainer::TCVector<NSys::CIoSpan> fg_MakeSpanVector(uint8 const *_pData, umint _nBytes)
		{
			NContainer::TCVector<NSys::CIoSpan> Spans;
			Spans.f_SetLen(1, true);
			Spans.f_GetArray()[0] = NSys::CIoSpan{.m_pData = _pData, .m_nBytes = _nBytes};

			return Spans;
		}

		// Sec-WebSocket-Extensions is a comma separated list, each entry a token with optional
		// parameters after a semicolon. Only the token is compared, so an entry carrying
		// parameters still matches the one it names
		bool fg_ContainsExtension(NStr::CStr const &_Header, NStr::CStr const &_Token)
		{
			NStr::CStr ToParse = _Header;

			while (!ToParse.f_IsEmpty())
			{
				NStr::CStr Entry = fg_GetStrSep(ToParse, ",");
				NStr::CStr Name = fg_GetStrSep(Entry, ";");
				Name.f_Trim();

				if (Name == _Token)
					return true;
			}

			return false;
		}
	}

	template <typename t_CCallback>
	struct TCDeferredCallback
	{
		using CReturn = typename NTraits::TCFunctionTraits<t_CCallback>::CReturn;
		using CFunction = NFunction::TCFunctionMovable<t_CCallback>;
		using CStripedReturn = typename NConcurrency::NPrivate::TCIsFuture<CReturn>::CType;

		template <typename ...tfp_CParams>
		NConcurrency::TCFuture<CStripedReturn> operator() (tfp_CParams && ...p_Params)
		{
			if (!mp_fCallback.f_IsEmpty())
				return mp_fCallback(fg_Forward<tfp_CParams>(p_Params)...);
			else if (!mp_bDoDefer)
				return DMibErrorInstance("Invalid call to non-defered").f_ExceptionPointer();

			NConcurrency::TCPromiseFuturePair<CStripedReturn> Promise;
			[&]<typename ...tfp_CFunctionParams>(NMeta::TCTypeList<tfp_CFunctionParams...> &&) mutable
			{
				mp_DeferredCalls.f_Insert
					(
						[Promise = fg_Move(Promise.m_Promise), ...p_Params2 = NTraits::TCDecay<tfp_CFunctionParams>(fg_Forward<tfp_CParams>(p_Params))]
						(NConcurrency::TCActorFunctorWeak<t_CCallback> const &_fCallback) mutable
						{
							return [&]<typename ...tfp_CParams2>(tfp_CParams2 && ...p_Params) mutable
								{
									_fCallback(fg_Move(p_Params)...) > fg_Move(Promise);
								}
								(fg_Move(p_Params2)...)
							;
						}
					)
				;
			}
			(typename NTraits::TCFunctionTraits<t_CCallback>::CParams());

			return fg_Move(Promise.m_Future);
		}

		template <typename ...tfp_CParams>
		void f_CallDiscard(tfp_CParams && ...p_Params)
		{
			if (!mp_fCallback.f_IsEmpty())
				return mp_fCallback.f_CallDiscard(fg_Forward<tfp_CParams>(p_Params)...);
			else if (!mp_bDoDefer)
				return;

			[&]<typename ...tfp_CFunctionParams>(NMeta::TCTypeList<tfp_CFunctionParams...> &&) mutable
			{
				mp_DeferredCalls.f_Insert
					(
						[...p_Params2 = NTraits::TCDecay<tfp_CFunctionParams>(fg_Forward<tfp_CParams>(p_Params))]
						(NConcurrency::TCActorFunctorWeak<t_CCallback> const &_fCallback) mutable
						{
							return [&]<typename ...tfp_CParams2>(tfp_CParams2 && ...p_Params) mutable
								{
									_fCallback.f_CallDiscard(fg_Move(p_Params)...);
								}
								(fg_Move(p_Params2)...)
							;
						}
					)
				;
			}
			(typename NTraits::TCFunctionTraits<t_CCallback>::CParams());
		}

		void f_SetCallback(NConcurrency::TCActorFunctorWeak<t_CCallback> &&_fCallback)
		{
			mp_fCallback = fg_Move(_fCallback);

			for (auto &fDeferredCall : mp_DeferredCalls)
				fDeferredCall(mp_fCallback);

			f_StopDeferring();
		}

		void f_StopDeferring()
		{
			mp_DeferredCalls.f_Clear();
			mp_bDoDefer = false;
		}

		bool f_ShouldCall() const
		{
			return !mp_fCallback.f_IsEmpty() || mp_bDoDefer;
		}

		bool f_IsEmpty() const
		{
			return mp_fCallback.f_IsEmpty();
		}

		TCDeferredCallback(bool _bDoDefer)
			: mp_bDoDefer(_bDoDefer)
		{
		}

		NConcurrency::TCFuture<void> f_Destroy() &&
		{
			f_StopDeferring();
			return fg_Move(mp_fCallback).f_Destroy();
		}

	public:
		NContainer::TCVector<NFunction::TCFunctionMovable<void (NConcurrency::TCActorFunctorWeak<t_CCallback> const &_Callback)>> mp_DeferredCalls;
		bool mp_bDoDefer = false;
		NConcurrency::TCActorFunctorWeak<t_CCallback> mp_fCallback;
	};


	struct CWebSocketActor::CInternal : public NConcurrency::CActorInternal
	{
		// The io subsystem, cached since each access through the getter is an atomic operation
		NMib::NSys::CIoSubSystem *mp_pIo = &NMib::NSys::fg_IoSubSystem();

		struct CClientConnectionInput
		{
			NStr::CStr m_EncodedKey;
			NContainer::TCSet<NStr::CStr> m_Protocols;
		};

		// What each operation in flight took. Addressed rather than ordered, because
		// operations do not always report in submission order
		struct CSendReservation
		{
			umint m_nBytes:sizeof(umint) * 8 - 1 = 0;
			umint m_bInUse:1 = false;
		};

		CInternal(CWebSocketActor *_pThis, bool _bClient, CWebsocketSettings const &_Settings)
			: m_pThis(_pThis)
			, m_fOnReceiveBinaryMessage(true)
			, m_fOnReceiveTextMessage(true)
			, m_fOnReceivePing(true)
			, m_fOnReceivePong(true)
			, m_fOnClose(true)
			, m_fOnFinishConnection(!_bClient)
			, m_fOnFinishClientConnection(_bClient)
			, m_IncomingData(gc_IncomingPageSize)
			, m_OutgoingData(gc_OutgoingPageSize)
			, m_bClient(_bClient)
			, m_Settings(_Settings)
			, m_pLastPendingMessagesList(nullptr)
		{
			// Masking is a wire-format contract both peers agree on via their settings, not a
			// property either side derives from the socket
			m_bMaskFrames = !_Settings.m_bAllowUnmaskedFrames;

			// Bounded so every gather size derived from it fits the reservation bit field,
			// on 32 bit platforms included
			m_Settings.m_FragmentationSize = fg_Min(m_Settings.m_FragmentationSize, umint(1) << 30);

			if (_bClient)
				m_ConnectionInfo.f_Set<2>();
			else
				m_ConnectionInfo.f_Set<1>();
		}

		~CInternal()
		{
			DMibFastCheck(!m_bDestroyed || m_OutgoingDataPromises.empty());
			DMibFastCheck(!m_bDestroyed || m_PendingMessages.f_IsEmpty());

			if (m_ClosePromise)
				m_ClosePromise->f_SetException(DMibErrorInstance("Abandoned close"));
		}

		void f_OnReceivedData();
		void f_OnSentData();

		void f_UpdateTimeout();
		void f_SetupTimeout();
		void f_StopTimeout();
		void f_OnTimeoutPongReceived();

		void f_ShutdownDone(NStr::CStr const &_Error);

		void f_HandleControlMessage(CMessage &_Message);
		void f_HandleDataMessage(CMessage &_Message);
		void f_SendMessage(EOpcode _Opcode, uint8 const *_pData, umint _nBytes, bool _bFinished) noexcept;
		void f_SendMessageFrameSegmented(COutgoingMessage &_Message, umint _nFrameBytes) noexcept;

		NContainer::TCLinkedList<COutgoingMessage> &f_PickMessageQueue(uint32 _Priority);
		COutgoingMessage &f_QueueMessage(EOpcode _Opcode, NStorage::TCSharedPointer<NContainer::CIOByteVector const> const &_pData, uint32 _Priority);

		COutgoingMessage &f_QueueViewMessage
			(
				EOpcode _Opcode
				, NContainer::TCVector<NSys::CIoSpan> &&_Spans
				, umint _nTotalBytes
				, NStorage::TCSharedPointer<CPayloadOwner> &&_pOwner
				, uint32 _Priority
			)
		;

		umint f_GetCopyThreshold() const;

		void f_WriteQueuedMessages(bool _bFlushAll);
		void f_WriteCloseFrameWhenDrained();
		static void fs_ApplyMask(uint8 *_pData, umint _iDataStart, umint _nBytes, uint8 const *_pMask);

		void f_TrackArenaBytes(umint _nBytes) noexcept;

		NConcurrency::CActorSubscription f_SetCallbacks(CCallbacks &&_Callbacks);

		void f_FinishDirectReadFrame();

		NNetwork::ICSocketCompletionIo *f_GetCompletionIo();
		NNetwork::ICSocketCompletionIo *f_GetCompletionIoSend();
		NNetwork::ICSocketCompletionIo *f_GetCompletionIoReceive();

		umint f_GatherSendSpans(NSys::CIoSpan *o_pSpans, umint &o_nSpans, NContainer::TCVector<NStorage::TCSharedPointer<CPayloadOwner>> &o_KeepAlives, NStorage::TCSharedPointer<NContainer::CIOByteVector> &o_pArenaCopy);
		void f_ConsumeSentBytes(umint _nSentBytes);

		void f_ReleaseReceiveState();
		void f_ReleaseOutgoingState();
		void f_TryReleaseDeferredReceiveState();

		void f_FinishClientConnection(EFinishConnectionResult _Result, CClientConnectionInfo &&_ConnectionInfo);
		void f_FinishConnection(EFinishConnectionResult _Result, CConnectionInfo &&_ConnectionInfo);

		CWebSocketActor *m_pThis = nullptr;
		NStorage::TCUniquePointer<NNetwork::ICSocket> m_pSocket;
		NMib::NNetwork::CNetAddress m_PeerAddress;

		EState m_State = EState_None;
		// Close-class states reported while a receive operation was in flight; they wait for
		// its data, which can hold the peer's close frame
		NNetwork::ENetTCPState m_DeferredCloseStates = NNetwork::ENetTCPState_None;

		NContainer::CPagedByteVector m_IncomingData{4096};
		NContainer::CPagedByteVector m_OutgoingData{4096};
		NContainer::TCLinkedList<COutgoingSegment> m_OutgoingSegments;
		COutgoingSegment *m_pLastOutgoingSegment = nullptr;
		// Unsent bytes across all outgoing segments. This counts the logical bytes a view segment
		// refers to rather than bytes this connection allocated, so a shared payload queued many
		// times can push it past what a 32 bit umint would hold
		uint64 m_nOutgoingQueuedBytes = 0;
		std::deque<COutgoingDataPromise> m_OutgoingDataPromises;

		NStorage::TCVariant<void, CConnectionInfo, CClientConnectionInfo> m_ConnectionInfo;
		CClientConnectionInput m_ClientConnectionInput;
		NStr::CStr m_Key;
		NStr::CStr m_Version;

		CMessage m_NextMessage;
		CMessage m_PendingMessage;
		CWebSocketActor::CCloseInfo m_CloseInfo;

		// Direct payload read: once a large data frame's header is parsed, the remaining
		// payload bytes are received straight into the destination message buffer. The
		// receive loop never reads past the payload, so the next frame's header still lands
		// in the incoming pages
		uint64 m_nDirectReadRemaining = 0;
		NContainer::CIOByteVector *m_pDirectReadData = nullptr;
		umint m_DirectReadFrameStart = 0;
		// Stable receive target for submitted operations: the kernel writes into it after the
		// submitting call has returned, which the readiness path's stack buffer cannot survive
		NStorage::TCSharedPointer<NConcurrency::CIoCompletionOpTracker> m_pOpTracker;
		NContainer::CByteVector m_ReceiveChunk;

		NContainer::TCMap<uint32, NContainer::TCLinkedList<COutgoingMessage>> m_PendingMessages;
		NContainer::TCLinkedList<COutgoingMessage> *m_pLastPendingMessagesList;

		// A close frame parked behind the pending queue (m_bCloseFramePending): framing the
		// whole backlog at once would copy every masked payload into the arena in one step, so
		// the close is emitted when the queue runs dry. The drain is not complete while it is
		// parked
		NContainer::CByteVector m_CloseFramePayload;

		// Messages accepted at or after the close transition: letting them into the stream
		// would postpone the close frame indefinitely under sustained traffic, so they park
		// here and settle at teardown (their destructors reject the promises). Control frames
		// queued before the transition still drain ahead of the close frame
		NContainer::TCLinkedList<COutgoingMessage> m_PostCloseMessages;

		NStorage::TCOptionalClearOnMove<NConcurrency::TCPromise<CWebSocketActor::CCloseInfo>> m_ClosePromise;
		NContainer::TCLinkedList<NFunction::TCFunctionMovable<void (NStr::CStr const &_Error)>> m_OnShutdown;

		TCDeferredCallback<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<NStream::CBinaryStorage const> _pMessage)> m_fOnReceiveBinaryMessage;
		TCDeferredCallback<NConcurrency::TCFuture<void> (NStr::CStr _Message)> m_fOnReceiveTextMessage;
		TCDeferredCallback<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<NContainer::CIOByteVector const> _ApplicationData)> m_fOnReceivePing;
		TCDeferredCallback<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<NContainer::CIOByteVector const> _ApplicationData)> m_fOnReceivePong;
		TCDeferredCallback<NConcurrency::TCFuture<void> (EWebSocketStatus _Status, NStr::CStr _Message, EWebSocketCloseOrigin _Origin)> m_fOnClose;
		TCDeferredCallback<NConcurrency::TCFuture<void> (EFinishConnectionResult _Result, CConnectionInfo _ConnectionInfo)> m_fOnFinishConnection;
		TCDeferredCallback<NConcurrency::TCFuture<void> (EFinishConnectionResult _Result, CClientConnectionInfo _ConnectionInfo)> m_fOnFinishClientConnection;

		NConcurrency::CActorSubscription m_TimeoutTimerSubscription;
		NTime::CStopwatch m_TimeoutReceivedData;
		NTime::CStopwatch m_TimeoutSentData;
		NStorage::TCSharedPointer<NContainer::CIOByteVector const> m_pTimeoutPingMessage;
		CWebsocketSettings m_Settings;
		umint m_TimeoutTimerSubscriptionSequence = 0;
		uint64 m_nSentBytes = 0;
		uint64 m_nReceivedBytes = 0;

		NNetwork::ICSocketCompletionIo *m_pCompletionIo = nullptr;

		// The stream's flow-control accounting, shared with every buffer it has delivered
		NStorage::TCSharedPointer<NSys::CIoStreamBackpressure> m_pReceiveBackpressure;

		// Sends with the socket right now; one at most, which is what keeps the stream in
		// order with no ordering protocol between operations
		umint m_nSendOpsInFlight = 0;

		// Payload handed to operations that have not reported yet, so a second operation
		// cannot gather bytes the first is already carrying
		umint m_nOutgoingSubmitted = 0;

		// A continuation carries no reservation of its own
		static constexpr umint mc_iNoReservation = umint(-1);

		// Grown one at a time as the send window actually admits more, never scanned per entry:
		// free slots wait on their own list. A staging socket bounds its own pipeline and keeps
		// the historical eight; a plain socket’s pipeline is bounded by the send window in bytes,
		// so the slots only need to never be the binding constraint
		NContainer::TCVector<CSendReservation> m_SendReservations;
		NContainer::TCVector<uint32> m_iFreeSendReservations;
		umint m_nSendReservationsInUse = 0;

		// Bytes of accepted sends whose release functors have not run; what the send window
		// asks are measured against
		umint m_nSendBytesUnreleased = 0;

		// The window a connection begins at, from the settings: eight frames
		umint fp_SendWindowStartBytes() const
		{
			return m_Settings.f_GetSendWindowStartBytes();
		}

		umint fp_MaxSendReservations(NNetwork::ICSocketCompletionIo *_pCompletionIo) const
		{
			if (_pCompletionIo->f_SupportsSendStaging())
				return 8;

			umint nFrameBytes = fp_SendWindowStartBytes() / 8;
			return fg_Max(umint(8), m_Settings.f_GetSendWindowBytes() / nFrameBytes + 2);
		}

		// Tearing the connection down gives every reservation back at once; operations still in
		// flight then find their own already accounted for
		void fp_ResetSendReservations()
		{
			m_iFreeSendReservations.f_Clear();
			for (umint iSlot = 0; iSlot < m_SendReservations.f_GetLen(); ++iSlot)
			{
				m_SendReservations[iSlot].m_bInUse = false;
				m_iFreeSendReservations.f_InsertLast(uint32(iSlot));
			}

			m_nSendReservationsInUse = 0;
			m_nSendBytesUnreleased = 0;
		}

		umint m_bPendingPing:1 = false;
		umint m_bSentPing:1 = false;

		umint m_bPendingMessage:1 = false;
		umint m_bClient:1 = false;
		umint m_bMaskFrames:1 = true;
		umint m_bPeerOfferedUnmasked:1 = false;

		umint m_bOnCloseCalled:1 = false;
		umint m_bOnFinishDone:1 = false;
		umint m_bWantStopDefer:1 = false;
		umint m_bShutdownCalled:1 = false;

		umint m_bFinishCalled:1 = false;

		umint m_bCompletionIo:1 = false;

		// The direct read appends receive buffer views to the message storage instead of
		// filling m_Data, so an unmasked binary frame on a completion stream is never copied
		// in user space
		umint m_bDirectReadToStorage:1 = false;

		// Stream segments resolve as shared views (plain sockets do; TLS decrypts into caller
		// buffers), which the storage direct read requires
		umint m_bReceiveStreamShared:1 = false;

		// A close frame is parked in m_CloseFramePayload
		umint m_bCloseFramePending:1 = false;

		// The standing receive: started once, ended by exactly one terminal segment. While
		// active and unended, close-class poll states wait for it — it still holds the bytes
		// that precede the close, and it always terminates once the peer is gone
		umint m_bReceiveStreamActive:1 = false;
		umint m_bReceiveStreamEnded:1 = false;

		umint m_bDeferredShutdownCleanup:1 = false;
		umint m_bFlushSendScheduled:1 = false;

#if DMibConfig_Tests_Enable
		umint m_bDebugNoProcessing:1 = false;
		umint m_bDebugNoProcessingReceive:1 = false;
		umint m_bDebugNoProcessingSend:1 = false;
		umint m_bDebugNoWriteQueuedMessages:1 = false;
		umint m_bDebugFailSends:1 = false;
		umint m_bDebugDelayClose:1 = false;

		aint m_nDebugRemainingWriteOps = -1; // -1 = unlimited, >=0 = remaining write ops allowed (decrements each op)
#endif

#if DMibEnableSafeCheck > 0
		umint m_bDestroyed = false;
#endif
	};

	CWebSocketActor::CWebSocketActor(bool _bClient, CWebsocketSettings const &_Settings)
		: mp_pInternal(fg_Construct(this, _bClient, _Settings))
	{
		auto &Internal = *mp_pInternal;
		Internal.f_SetupTimeout();
	}

	CWebSocketActor::~CWebSocketActor()
	{
	}

	// The socket's completion transfer interface; null when the connection runs on readiness.
	// Cached at activation — the socket cannot be replaced after that
	NNetwork::ICSocketCompletionIo *CWebSocketActor::CInternal::f_GetCompletionIo()
	{
		if (!m_bCompletionIo || !m_pSocket)
			return nullptr;

		return m_pCompletionIo;
	}

	// Per direction; null when that direction does not accept submits. For new submits only —
	// resolve in-flight completions through f_GetCompletionIo
	NNetwork::ICSocketCompletionIo *CWebSocketActor::CInternal::f_GetCompletionIoSend()
	{
		auto *pCompletionIo = f_GetCompletionIo();

		return pCompletionIo && pCompletionIo->f_SupportsCompletionSend() ? pCompletionIo : nullptr;
	}

	NNetwork::ICSocketCompletionIo *CWebSocketActor::CInternal::f_GetCompletionIoReceive()
	{
		auto *pCompletionIo = f_GetCompletionIo();

		return pCompletionIo && pCompletionIo->f_SupportsCompletionReceive() ? pCompletionIo : nullptr;
	}

	NContainer::TCLinkedList<COutgoingMessage> &CWebSocketActor::CInternal::f_PickMessageQueue(uint32 _Priority)
	{
		// Nothing accepted after the close transition may enter the stream; see
		// m_PostCloseMessages
		if (m_State >= EState_Disconnecting)
			return m_PostCloseMessages;

		return m_PendingMessages[_Priority];
	}

	COutgoingMessage &CWebSocketActor::CInternal::f_QueueMessage
		(
			EOpcode _Opcode
			, NStorage::TCSharedPointer<NContainer::CIOByteVector const> const &_pData
			, uint32 _Priority
		)
	{
		DMibFastCheck(!m_pThis->f_IsDestroyed());

		auto &NewMessage = f_PickMessageQueue(_Priority).f_Insert();
		NewMessage.m_pData = _pData;
		NewMessage.m_Opcode = _Opcode;
		NewMessage.m_bFinished = true;

		return NewMessage;

	}

	// Queues a message that references its payload in place. The frames are cut out of the
	// spans lazily as they are written, so a fragmented message costs no copies: only the
	// frame headers are built, and masking clients mask into the arena at emit time
	COutgoingMessage &CWebSocketActor::CInternal::f_QueueViewMessage
		(
			EOpcode _Opcode
			, NContainer::TCVector<NSys::CIoSpan> &&_Spans
			, umint _nTotalBytes
			, NStorage::TCSharedPointer<CPayloadOwner> &&_pOwner
			, uint32 _Priority
		)
	{
		DMibFastCheck(!m_pThis->f_IsDestroyed());

		auto &NewMessage = f_PickMessageQueue(_Priority).f_Insert();
		NewMessage.m_Opcode = _Opcode;
		NewMessage.m_bFinished = true;
		NewMessage.m_bView = true;
		NewMessage.m_pOwner = fg_Move(_pOwner);
		NewMessage.m_Spans = fg_Move(_Spans);
		NewMessage.m_nTotalBytes = _nTotalBytes;

		return NewMessage;
	}

	// Largest payload that may be copied instead of referenced: copying is only cheaper
	// below the small message threshold, and a copied message is never fragmented, so a
	// payload that does not fit one frame has to take the view path whatever its size
	umint CWebSocketActor::CInternal::f_GetCopyThreshold() const
	{
		return fg_Min(umint(gc_CopySmallMessageThreshold), m_Settings.m_FragmentationSize);
	}

	// Mirrors bytes appended to the copy arena into the outgoing segment queue
	void CWebSocketActor::CInternal::f_TrackArenaBytes(umint _nBytes) noexcept
	{
		if (!_nBytes)
			return;

		if (m_pLastOutgoingSegment && m_pLastOutgoingSegment->m_Kind == COutgoingSegment::EKind::mc_Arena)
		{
			m_pLastOutgoingSegment->m_nBytes += _nBytes;
			m_nOutgoingQueuedBytes += _nBytes;
			return;
		}

		auto &Segment = m_OutgoingSegments.f_Insert();
		Segment.m_Kind = COutgoingSegment::EKind::mc_Arena;
		Segment.m_nBytes = _nBytes;
		m_pLastOutgoingSegment = &Segment;
		m_nOutgoingQueuedBytes += _nBytes;
	}

	void CWebSocketActor::CInternal::f_WriteQueuedMessages(bool _bFlushAll)
	{
		if (m_PendingMessages.f_IsEmpty())
			return;

		uint64 OutgoingData = m_nOutgoingQueuedBytes;
		uint64 TargetData = 0;

		if (!_bFlushAll)
		{
			// Keep roughly one frame of data queued ahead so the drain loop can issue large
			// vectored writes while control frames still interleave promptly
			TargetData = fg_Max(uint64(2 * gc_OutgoingPageSize), uint64(m_Settings.m_FragmentationSize) + NNetwork::gc_SocketFramingMargin);
#if DMibConfig_IoDebug_Enable
			// MalterlibWebSocketFrameAhead=N frames N of those ahead, to measure how deep a socket whose
			// completions come at the packet keeps its pipeline
			if (umint nFrameAhead = mp_pIo->f_WebSocketFrameAhead(); nFrameAhead > 1)
				TargetData *= nFrameAhead;
#endif

			if (OutgoingData >= TargetData)
				return;
		}

		auto pList = m_pLastPendingMessagesList;
		auto pFragmentingList = m_pLastPendingMessagesList;  // Save the fragmenting list

		if (!pList)
		{
			pList = m_PendingMessages.f_FindLargest();
			DMibCheck(pList);
		}
		else
		{
			// Fragmentation is in progress - must continue with current list
			// EXCEPTION: Ping/Pong can interleave per RFC 6455 (they use max uint32 priority)
			auto *pHighestPrioList = m_PendingMessages.f_FindLargest();
			if (pHighestPrioList != pList && m_PendingMessages.fs_GetKey(*pHighestPrioList) == TCLimitsInt<uint32>::mc_Max)
			{
				// Ping/Pong queued - allow them to interleave
				pList = pHighestPrioList;
			}
			// Otherwise: stay on fragmenting list (data frames cannot interleave)
		}

		DMibCheck(!pList->f_IsEmpty());

		auto *pPending = &pList->f_GetFirst();

		bool bFinished = false;

		while (OutgoingData < TargetData || _bFlushAll)
		{
#if DMibConfig_Tests_Enable
			if (m_nDebugRemainingWriteOps == 0)
				break;
#endif

			bool bMessageDone;
			if (pPending->m_bView)
			{
				// View messages emit one frame per iteration; the message stays at the head of
				// its list while payload remains, which pins the fragmenting list exactly like
				// pre-fragmented messages do
				umint nRemaining = pPending->m_nTotalBytes - pPending->m_iPayloadSent;
				umint nFrameBytes = fg_Min(nRemaining, m_Settings.m_FragmentationSize);
				bMessageDone = nFrameBytes == nRemaining;
				bFinished = bMessageDone && pPending->m_bFinished;

				f_SendMessageFrameSegmented(*pPending, nFrameBytes);
			}
			else
			{
				bFinished = pPending->m_bFinished;
				bMessageDone = true;

				f_SendMessage(pPending->m_Opcode, pPending->m_pData->f_GetArray(), pPending->m_pData->f_GetLen(), bFinished);
			}

#if DMibConfig_Tests_Enable
			if (m_nDebugRemainingWriteOps > 0)
				--m_nDebugRemainingWriteOps;
#endif

			OutgoingData = m_nOutgoingQueuedBytes;

			if (!bMessageDone)
				continue;

			if (pPending->m_Promise)
			{
				COutgoingDataPromise Promise;
				Promise.m_Position = m_nSentBytes + OutgoingData;
				Promise.m_Promise = fg_Move(pPending->m_Promise);
				m_OutgoingDataPromises.push_back(fg_Move(Promise));
			}

			pList->f_Remove(*pPending);
			if (pList->f_IsEmpty())
			{
				if (pList == pFragmentingList)
					pFragmentingList = nullptr;

				m_PendingMessages.f_Remove(pList);

				if (pFragmentingList)
					pList = pFragmentingList;
				else
				{
					pList = m_PendingMessages.f_FindLargest();
					if (!pList)
					{
						pPending = nullptr;
						break;
					}
				}
			}
			pPending = &pList->f_GetFirst();
		}

		if (pFragmentingList)
			m_pLastPendingMessagesList = pFragmentingList;
		else if (bFinished)
			m_pLastPendingMessagesList = nullptr;
		else
			m_pLastPendingMessagesList = pList;
	}

	void CWebSocketActor::CInternal::f_WriteCloseFrameWhenDrained()
	{
		if (!m_bCloseFramePending || !m_PendingMessages.f_IsEmpty())
			return;

		m_bCloseFramePending = false;
		NContainer::CByteVector Payload = fg_Move(m_CloseFramePayload);
		f_SendMessage(EOpcode_ConnectionClose, Payload.f_GetArray(), Payload.f_GetLen(), true);
	}

	// Gathers the head segments as spans (no copy): arena segments contribute their pages,
	// view segments their referenced payload bytes. Returns the bytes gathered
	umint CWebSocketActor::CInternal::f_GatherSendSpans(NSys::CIoSpan *o_pSpans, umint &o_nSpans, NContainer::TCVector<NStorage::TCSharedPointer<CPayloadOwner>> &o_KeepAlives, NStorage::TCSharedPointer<NContainer::CIOByteVector> &o_pArenaCopy)
	{
		umint nSpans = 0;
		umint nGatheredBytes = 0;
		umint nArenaBytes = 0;
		NContainer::TCBitArray<NNetwork::ICSocket::mc_MaxSendSpans> ArenaSpans;

		// One gather is one transport write, so a connection that frames at more than this would
		// otherwise have its frames split across writes for no reason. The floor keeps small
		// framing from making the gather too small to be worth the call
		umint nMaxGatherBytes = fg_Max(umint(256 * 1024), m_Settings.m_FragmentationSize);

		// What operations already in flight took. They report before this gather's bytes go out, so
		// the stream stays in order; what must not happen is offering their bytes a second time.
		// The arena is consumed from its front, so its offset walks every segment whether or not
		// this gather takes anything from it
		umint nSkip = m_nOutgoingSubmitted;

		umint ArenaOffset = 0;
		bool bFull = false;
		for (auto &Segment : m_OutgoingSegments)
		{
			if (bFull)
				break;

			bool bArena = Segment.m_Kind == COutgoingSegment::EKind::mc_Arena;
			umint nAvailable = bArena ? Segment.m_nBytes : Segment.m_nBytes - Segment.m_iSent;

			if (nSkip >= nAvailable)
			{
				nSkip -= nAvailable;

				if (bArena)
					ArenaOffset += Segment.m_nBytes;

				continue;
			}

			umint nSkipWithin = nSkip;
			nSkip = 0;

			if (bArena)
			{
				m_OutgoingData.f_Read
					(
						ArenaOffset + nSkipWithin
						, Segment.m_nBytes - nSkipWithin
						, [&](umint _iStart, uint8 const* _pPtr, umint _nBytes) -> bool
						{
							o_pSpans[nSpans].m_pData = _pPtr;
							o_pSpans[nSpans].m_nBytes = _nBytes;
							ArenaSpans.f_SetBit(nSpans, 1);
							++nSpans;
							nGatheredBytes += _nBytes;
							nArenaBytes += _nBytes;
							bFull = nSpans >= NNetwork::ICSocket::mc_MaxSendSpans || nGatheredBytes >= nMaxGatherBytes;
							return !bFull;
						}
					)
				;
				ArenaOffset += Segment.m_nBytes;
			}
			else
			{
				// Clamped to what is left of the gather budget: sockets without a vectored
				// implementation fall back to one f_Send per span, and the SSL one takes an
				// int length
				umint iStart = Segment.m_iSent + nSkipWithin;
				umint nSegmentBytes = fg_Min(Segment.m_nBytes - iStart, nMaxGatherBytes - nGatheredBytes);

				o_pSpans[nSpans].m_pData = Segment.m_pData + iStart;
				o_pSpans[nSpans].m_nBytes = nSegmentBytes;
				nGatheredBytes += nSegmentBytes;
				++nSpans;
				bFull = nSpans >= NNetwork::ICSocket::mc_MaxSendSpans || nGatheredBytes >= nMaxGatherBytes;

				// The viewed payload has to outlive the kernel's use of these pages, which for a
				// zero copy send ends only at the buffer-released notification — after the
				// transfer was reported and the segment retired. The released functor carries
				// these, so consuming the queue never frees pages the kernel still references
				if (Segment.m_pOwnerKeepAlive)
					o_KeepAlives.f_Insert(Segment.m_pOwnerKeepAlive);
			}
		}

		// Arena bytes live in pages the consume path removes from the front as soon as the
		// transfer is reported — before a zero copy release. They are copied into storage the
		// released functor owns, and the spans retargeted, so the kernel never reads a page the
		// arena has already recycled. A socket whose releases are prompt has no such window:
		// the kernel is done with the pages by the time the transfer is reported
		if (nArenaBytes && !(m_pCompletionIo && m_pCompletionIo->f_SendReleaseIsPrompt()))
		{
			o_pArenaCopy = fg_Construct();
			o_pArenaCopy->f_SetLen(nArenaBytes, false);

			umint nCopied = 0;
			ArenaSpans.f_EnumSetBits
				(
					[&](umint _iSpan) -> bool
					{
						NMemory::fg_MemCopy(o_pArenaCopy->f_GetArray() + nCopied, o_pSpans[_iSpan].m_pData, o_pSpans[_iSpan].m_nBytes);
						o_pSpans[_iSpan].m_pData = o_pArenaCopy->f_GetArray() + nCopied;
						nCopied += o_pSpans[_iSpan].m_nBytes;

						return true;
					}
				)
			;
		}

		o_nSpans = nSpans;

		return nGatheredBytes;
	}

	// Advances the outgoing stream past sent bytes: resolves due write promises, consumes
	// arena bytes and view cursors, and releases fully sent segments' keep alives
	void CWebSocketActor::CInternal::f_ConsumeSentBytes(umint _nSentBytes)
	{
		uint64 PrevSent = m_nSentBytes;
		m_nSentBytes += _nSentBytes;

		while (!m_OutgoingDataPromises.empty())
		{
			auto &Promise = m_OutgoingDataPromises.front();
			uint64 Diff = Promise.m_Position - PrevSent;
			if (Diff <= _nSentBytes)
			{
				Promise.m_Promise->f_SetResult();
				Promise.m_Promise.f_Clear();
				m_OutgoingDataPromises.pop_front();
				continue;
			}
			break;
		}

		// Consume the sent bytes across head segments: arena bytes leave the arena, view
		// segments advance their cursor and release their keep alives when fully sent
		umint nConsumed = _nSentBytes;
		while (nConsumed)
		{
			auto &Head = m_OutgoingSegments.f_GetFirst();
			umint nHeadRemaining = Head.m_nBytes - Head.m_iSent;
			if (Head.m_Kind == COutgoingSegment::EKind::mc_Arena)
			{
				umint nThis = fg_Min(nConsumed, Head.m_nBytes);
				m_OutgoingData.f_RemoveFront(nThis);
				Head.m_nBytes -= nThis;
				nHeadRemaining = Head.m_nBytes;
				nConsumed -= nThis;
				m_nOutgoingQueuedBytes -= nThis;
			}
			else
			{
				umint nThis = fg_Min(nConsumed, nHeadRemaining);
				Head.m_iSent += nThis;
				nHeadRemaining -= nThis;
				nConsumed -= nThis;
				m_nOutgoingQueuedBytes -= nThis;
			}

			if (!nHeadRemaining)
			{
				if (&Head == m_pLastOutgoingSegment)
					m_pLastOutgoingSegment = nullptr;
				m_OutgoingSegments.f_Remove(Head);
			}
		}
	}

	// Drops everything the receive side holds; nothing can be received once the socket is gone
	void CWebSocketActor::CInternal::f_ReleaseReceiveState()
	{
		m_ReceiveChunk.f_Clear();
		m_IncomingData.f_RemoveFront(m_IncomingData.f_GetLen());
		m_NextMessage = CMessage();
		m_PendingMessage = CMessage();
		m_bPendingMessage = false;
		m_nDirectReadRemaining = 0;
		m_pDirectReadData = nullptr;
		m_DirectReadFrameStart = 0;
		m_bDirectReadToStorage = false;
		m_bReceiveStreamShared = false;
	}

	// Segments and arena reference shared payloads and pages a closed actor kept alive by the
	// application would otherwise pin until destruction; nothing can send them once the socket is
	// gone. Only safe with no operation in flight — an in-flight send still reads these buffers
	void CWebSocketActor::CInternal::f_ReleaseOutgoingState()
	{
		m_OutgoingSegments.f_Clear();
		m_pLastOutgoingSegment = nullptr;
		m_nOutgoingQueuedBytes = 0;
		m_nOutgoingSubmitted = 0;
		for (auto &Reservation : m_SendReservations)
			Reservation.m_bInUse = false;
		m_OutgoingData.f_RemoveFront(m_OutgoingData.f_GetLen());
	}

	// Runs the receive state release a close deferred while completion transfers were still in
	// flight and could land in these buffers
	void CWebSocketActor::CInternal::f_TryReleaseDeferredReceiveState()
	{
		if (m_nSendOpsInFlight)
			return;

		// Close states parked behind an operation are that operation's to hand back. Nothing else
		// reports them a second time, so a path that reaches here without resolving them would
		// lose the close entirely. Taken before they are acted on, so a disconnect coming back
		// through here finds nothing left to do
		NNetwork::ENetTCPState DeferredStates = m_DeferredCloseStates;
		m_DeferredCloseStates = NNetwork::ENetTCPState_None;

		if (m_bDeferredShutdownCleanup)
		{
			m_bDeferredShutdownCleanup = false;

			f_ReleaseReceiveState();
			f_ReleaseOutgoingState();
		}

		// Last, because it can disconnect and leave nothing here worth touching
		if (DeferredStates)
			m_pThis->fp_ProcessState(DeferredStates);
	}

#if DMibConfig_Tests_Enable
	NConcurrency::TCFuture<void> CWebSocketActor::f_DebugSetFlags(fp64 _Timeout, NNetwork::ESocketDebugFlag _DebugFlags)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying socket");

		auto &Internal = *mp_pInternal;

		Internal.m_bDebugNoProcessing = (_DebugFlags & NNetwork::ESocketDebugFlag_StopProcessing) != NNetwork::ESocketDebugFlag_None;
		Internal.m_bDebugNoProcessingReceive = (_DebugFlags & NNetwork::ESocketDebugFlag_StopProcessingReceive) != NNetwork::ESocketDebugFlag_None;
		Internal.m_bDebugNoProcessingSend = (_DebugFlags & NNetwork::ESocketDebugFlag_StopProcessingSend) != NNetwork::ESocketDebugFlag_None;
		Internal.m_bDebugNoWriteQueuedMessages = (_DebugFlags & NNetwork::ESocketDebugFlag_StopWriteQueuedMessages) != NNetwork::ESocketDebugFlag_None;
		Internal.m_bDebugFailSends = (_DebugFlags & NNetwork::ESocketDebugFlag_FailSends) != NNetwork::ESocketDebugFlag_None;
		Internal.m_bDebugDelayClose = (_DebugFlags & NNetwork::ESocketDebugFlag_DelayClose) != NNetwork::ESocketDebugFlag_None;

		if (_Timeout != fp64::fs_Inf())
		{
			Internal.m_Settings.m_Timeout = _Timeout;
			Internal.f_SetupTimeout();
		}

		fp_UpdateSend();

		// Socket state edges dropped while a no-processing flag was set are not re-signaled by the
		// socket, so re-drive processing now that the flags have changed. Always include Read: a
		// previously consumed edge can leave data pending without any accumulated state
		if (Internal.m_pSocket && Internal.m_pSocket->f_IsValid())
			fp_ProcessState(Internal.m_pSocket->f_GetState() | NNetwork::ENetTCPState_Read);

		co_return {};
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_DebugSetMaxWriteOps(aint _nMaxWriteOps)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying socket");

		auto &Internal = *mp_pInternal;
		Internal.m_nDebugRemainingWriteOps = _nMaxWriteOps; // -1 = unlimited, >=0 = remaining ops

		fp_UpdateSend();

		co_return {};
	}
#endif

	auto CWebSocketActor::f_DebugGetStats() -> NConcurrency::TCFuture<CDebugStats>
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying socket");

		auto &Internal = *mp_pInternal;

		CDebugStats DebugStats;
		DebugStats.m_nSentBytes = Internal.m_nSentBytes;
		DebugStats.m_nReceivedBytes = Internal.m_nReceivedBytes;
		DebugStats.m_SecondsSinceLastSend = Internal.m_TimeoutSentData.f_GetTime();
		DebugStats.m_SecondsSinceLastReceive = Internal.m_TimeoutReceivedData.f_GetTime();
		DebugStats.m_State = Internal.m_State;
		DebugStats.m_bMaskFrames = Internal.m_bMaskFrames;
		DebugStats.m_IncomingDataBufferBytes = Internal.m_IncomingData.f_GetLen();
		DebugStats.m_OutgoingDataBufferBytes = Internal.m_nOutgoingQueuedBytes;

		co_return fg_Move(DebugStats);
	}

	NConcurrency::TCFuture<CWebSocketActor::CCloseInfo> CWebSocketActor::f_Close(EWebSocketStatus _Status, NStr::CStr _Reason)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;
		if (Internal.m_ClosePromise)
		{
			DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::f_Close 1", fg_ThisActor(this), !Internal.m_bClient);
			co_return DMibErrorInstance("Socket close already initiated");
		}

		if (!Internal.m_pSocket || Internal.m_State == EState_Disconnected)
		{
			DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::f_Close 2", fg_ThisActor(this), !Internal.m_bClient );
			CWebSocketActor::CCloseInfo CloseInfo;
			CloseInfo.m_Status = EWebSocketStatus_AlreadyClosed;
			CloseInfo.m_Reason = "Already fully closed";
			co_return fg_Move(CloseInfo);
		}

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		auto CloseFuture = Internal.m_ClosePromise.f_CreateNew().f_Future();

		DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::f_Close 3", fg_ThisActor(this), !Internal.m_bClient);

		fp_Disconnect(_Status, _Reason, false, EWebSocketCloseOrigin_Local);

		auto Value = co_await fg_Move(CloseFuture);

		DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::f_Close 4", fg_ThisActor(this), !Internal.m_bClient);

		co_return fg_Move(Value);
	}

	void CWebSocketActor::CInternal::f_ShutdownDone(NStr::CStr const &_Error)
	{
		// The socket is gone by the time this runs, so nothing can be received into the incoming
		// buffers any more. A direct read reserves the whole advertised frame up front, so a peer
		// that sends a header and then disconnects would otherwise leave a full frame worth of
		// memory held for as long as the application keeps the closed actor alive. A completion
		// transfer still in flight pins these buffers on the kernel side, so its cancellation
		// completion runs the release instead
		if (m_nSendOpsInFlight)
			m_bDeferredShutdownCleanup = true;
		else
		{
			f_ReleaseReceiveState();
			f_ReleaseOutgoingState();
		}

		// Sends defer their flush onto the run queue, so the teardown can arrive with queued
		// messages the flush will never write: it no-ops without a socket. Dropping them here
		// settles their promises (the destructors reject unresolved ones) instead of leaving
		// senders hanging until the actor is destroyed. A close frame still parked behind them
		// can never be written either. With an operation still in flight the outgoing segments
		// and arena stay untouched until its completion runs the deferred release — the kernel
		// may still be reading them
		m_PendingMessages.f_Clear();
		m_pLastPendingMessagesList = nullptr;
		m_PostCloseMessages.f_Clear();
		m_OutgoingDataPromises.clear();
		m_bCloseFramePending = false;
		m_CloseFramePayload.f_Clear();

		// The terminal settle: any progress timeout still armed to watch a disconnected-state
		// drain has nothing left to watch
		f_StopTimeout();

		for (auto &fOnShutdown : m_OnShutdown)
			fOnShutdown(_Error);
		m_OnShutdown.f_Clear();
	}

	NConcurrency::TCFuture<void> CWebSocketActor::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

#if DMibEnableSafeCheck > 0
		Internal.m_bDestroyed = true;
#endif

		DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Destroy", fg_ThisActor(this), !Internal.m_bClient);

		if (Internal.m_pOpTracker)
		{
			// In-flight completion transfers still hold kernel references into the buffers this
			// destroy releases below. Closing the socket cancels them, and the fence resolves once
			// the last completion functor has released its hold; only then may the queues,
			// the arena and the view keep alives be dropped. The actor outlives fp_Destroy, so
			// everything the operations point into stays alive across the await
			Internal.m_pSocket.f_Clear();

			auto &Tracker = *Internal.m_pOpTracker;
			auto DrainFuture = Tracker.m_DrainPromise.f_CreateNew().f_Future();
			uint32 Previous = Tracker.m_State.f_FetchOr(NConcurrency::CIoCompletionOpTracker::mc_DrainFlag, NAtomic::gc_MemoryOrder_SequentiallyConsistent);
			if (!Previous)
				(*Tracker.m_DrainPromise).f_SetResult();

			co_await fg_Move(DrainFuture);
		}

		Internal.m_PendingMessages.f_Clear();
		Internal.m_PostCloseMessages.f_Clear();
		Internal.m_OutgoingDataPromises.clear();
		Internal.m_pLastPendingMessagesList = nullptr;
		Internal.m_OutgoingSegments.f_Clear();
		Internal.m_pLastOutgoingSegment = nullptr;
		Internal.m_nOutgoingQueuedBytes = 0;
		Internal.m_nOutgoingSubmitted = 0;
		for (auto &Reservation : Internal.m_SendReservations)
			Reservation.m_bInUse = false;
		if (Internal.m_ClosePromise)
		{
			Internal.m_ClosePromise->f_SetException(DMibErrorInstance("Abandoned close"));
			Internal.m_ClosePromise.f_Clear();
		}

		co_return {};
	}

	NConcurrency::TCFuture<CWebSocketActor::CCloseInfo> CWebSocketActor::f_CloseWithLinger(EWebSocketStatus _Status, NStr::CStr _Reason, fp64 _MaxLingerTime)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		{
			auto &Internal = *mp_pInternal;
			if (!Internal.m_pSocket || Internal.m_State == EState_Disconnected)
			{
				DMibLog(DebugVerbose3, " ++++ {} {} EWebSocketStatus_AlreadyClosed", fg_ThisActor(this), !Internal.m_bClient);

				CWebSocketActor::CCloseInfo CloseInfo;
				CloseInfo.m_Status = EWebSocketStatus_AlreadyClosed;
				CloseInfo.m_Reason = "Already fully closed";

				fg_ThisActor(this).f_Destroy().f_DiscardResult();

				co_return fg_Move(CloseInfo);
			}

#if DMibConfig_Tests_Enable
			if (Internal.m_bDebugDelayClose)
				co_await NConcurrency::fg_Timeout(0.05);
#endif
		}

		auto ProcessingActor = NConcurrency::fg_ThisConcurrentActor();

		DMibLog(DebugVerbose3, " ++++ {} {} f_CloseWithLinger", fg_ThisActor(this), !mp_pInternal->m_bClient);

		NConcurrency::TCPromiseFuturePair<CWebSocketActor::CCloseInfo> Promise;
		{
			auto &Internal = *mp_pInternal;

			struct CState
			{
				~CState()
				{
					if (!m_bHandled)
						f_Finish();
				}

				void f_Finish()
				{
					fg_Move(m_WebSocketActor).f_Destroy().f_DiscardResult();
				}

				NConcurrency::TCActor<CWebSocketActor> m_WebSocketActor;
				NAtomic::TCAtomic<bool> m_bHandled;
			};

			NStorage::TCSharedPointer<CState> pState = fg_Construct();
			pState->m_WebSocketActor = fg_ThisActor(this);

			auto Cleanup = NConcurrency::g_OnScopeExitActor(ProcessingActor) / [pState, Promise = Promise.m_Promise]
				{
					if (pState->m_bHandled.f_Exchange(true))
						return;

					Promise.f_SetException(DMibErrorInstance("Websocket destroyed"));
					pState->f_Finish();
				}
			;

			Internal.m_OnShutdown.f_Insert
				(
					[Cleanup, pState, Promise = Promise.m_Promise, this](NStr::CStr const &_Error)
					{
						if (pState->m_bHandled.f_Exchange(true))
							return;

						auto &Internal = *mp_pInternal;
						if (!_Error.f_IsEmpty())
							Promise.f_SetException(DMibErrorInstance(fg_Format("Unclean websocket shutdown: {}", _Error)));
						else
							Promise.f_SetResult(fg_Move(Internal.m_CloseInfo));
						pState->f_Finish();
					}
				)
			;

			f_Close(_Status, _Reason) > ProcessingActor / [pState, Promise = Promise.m_Promise](NConcurrency::TCAsyncResult<NWeb::CWebSocketActor::CCloseInfo> &&_Result)
				{
					if (!_Result)
					{
						if (pState->m_bHandled.f_Exchange(true))
							return;

						Promise.f_SetException(fg_Move(_Result));
						pState->f_Finish();
					}
				}
			;

			NConcurrency::fg_Timeout(_MaxLingerTime, false)(ProcessingActor) > [Promise = fg_Move(Promise.m_Promise), pState]() -> NConcurrency::TCFuture<void>
				{
					if (pState->m_bHandled.f_Exchange(true))
						co_return {};

					Promise.f_SetException(DMibErrorInstance("Timed out waiting for websocket to close gracefully"));
					pState->f_Finish();

					co_return {};
				}
			;
		}

		co_await fg_ContinueRunningOnActor(ProcessingActor);

		co_return co_await fg_Move(Promise.m_Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendBinary(NStorage::TCSharedPointer<NContainer::CIOByteVector const> _pMessage, uint32 _Priority)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugFailSends)
			co_return DMibErrorInstance("Debug fail send");
#endif

		DMibLog(DebugVerbose3, " ++++ {} {} f_SendBinary", fg_ThisActor(this), !Internal.m_bClient);

		auto &Massage = *_pMessage;
		umint nBytes = Massage.f_GetLen();

		if (nBytes > Internal.m_Settings.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		COutgoingMessage *pNewMessage;
		if (nBytes > Internal.f_GetCopyThreshold())
		{
			pNewMessage = &Internal.f_QueueViewMessage
				(
					EOpcode_BinaryFrame
					, fg_MakeSpanVector(Massage.f_GetArray(), nBytes)
					, nBytes
					, fg_Construct<TCPayloadOwner<NStorage::TCSharedPointer<NContainer::CIOByteVector const>>>(fg_Move(_pMessage))
					, _Priority
				)
			;
		}
		else
			pNewMessage = &Internal.f_QueueMessage(EOpcode_BinaryFrame, _pMessage, _Priority);

		auto Future = pNewMessage->m_Promise.f_CreateNew().f_Future();
		DMibLog(DebugVerbose3, " ++++ {} {} Queue binary", fg_ThisActor(this), !Internal.m_bClient);
		fp_ScheduleUpdateSend();

		co_return co_await fg_Move(Future);
	}

	// Sends a segmented binary message by reference: the payload spans are handed to the
	// socket in place, so the storage must stay frozen until the future resolves
	NConcurrency::TCFuture<void> CWebSocketActor::f_SendBinaryStorage(NStorage::TCSharedPointer<NStream::CBinaryStorage const> _pMessage, uint32 _Priority)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugFailSends)
			co_return DMibErrorInstance("Debug fail send");
#endif

		DMibLog(DebugVerbose3, " ++++ {} {} f_SendBinaryStorage", fg_ThisActor(this), !Internal.m_bClient);

		umint nBytes = _pMessage->f_GetTotalLength();

		if (nBytes > Internal.m_Settings.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		// Storage messages always take the view path: unmasked connections reference the
		// payload in place and masking clients mask into the arena at frame emit, so no
		// flattening or fragment copies happen in either case
		NContainer::TCVector<NSys::CIoSpan> Spans;
		Spans.f_Reserve(_pMessage->f_GetSpanCount());
		_pMessage->f_VisitSpans
			(
				[&](uint8 const *_pData, umint _nSpanBytes)
				{
					Spans.f_InsertLast(NSys::CIoSpan{.m_pData = _pData, .m_nBytes = _nSpanBytes});
				}
			)
		;

		auto &NewMessage = Internal.f_QueueViewMessage
			(
				EOpcode_BinaryFrame
				, fg_Move(Spans)
				, nBytes
				, fg_Construct<TCPayloadOwner<NStorage::TCSharedPointer<NStream::CBinaryStorage const>>>(fg_Move(_pMessage))
				, _Priority
			)
		;

		auto Future = NewMessage.m_Promise.f_CreateNew().f_Future();
		DMibLog(DebugVerbose3, " ++++ {} {} Queue storage view", fg_ThisActor(this), !Internal.m_bClient);
		fp_ScheduleUpdateSend();
		co_return co_await fg_Move(Future);
	}

	// Queues each storage as its own binary message in one actor call, so a burst of packets
	// pays one call and one send flush instead of one each; the future resolves when the last
	// message is sent and reports any failure of the batch
	NConcurrency::TCFuture<void> CWebSocketActor::f_SendBinaryStorages(NContainer::TCVector<NStorage::TCSharedPointer<NStream::CBinaryStorage const>> _Messages, uint32 _Priority)
	{
		// Deliberately no shutdown check beyond destruction, matching the single-message sends: a
		// batch queued after the socket has shut down settles when the actor is destroyed, where
		// the pending message and promise teardown rejects it as abandoned
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugFailSends)
			co_return DMibErrorInstance("Debug fail send");
#endif

		DMibLog(DebugVerbose3, " ++++ {} {} f_SendBinaryStorages {}", fg_ThisActor(this), !Internal.m_bClient, _Messages.f_GetLen());

		if (_Messages.f_IsEmpty())
			co_return {};

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		// Validate the whole batch before queueing anything so a failure never leaves a partial
		// batch on the wire ahead of the error
		for (auto const &pMessage : _Messages)
		{
			if (pMessage->f_GetTotalLength() > Internal.m_Settings.m_MaxMessageSize)
				co_return DMibErrorInstance("Message is bigger than max message size");
		}

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		// Each storage becomes its own binary message on the wire, exactly as if sent through
		// f_SendBinaryStorage one by one; the batch only shares the actor call and the flush.
		// One promise on the last message observes the whole batch: messages resolve in queue
		// order and a torn down connection rejects every still queued promise, so any earlier
		// failure reaches it
		NConcurrency::TCFuture<void> Future;
		umint nMessages = _Messages.f_GetLen();
		for (umint iMessage = 0; iMessage < nMessages; ++iMessage)
		{
			auto &pMessage = _Messages[iMessage];
			umint nBytes = pMessage->f_GetTotalLength();

			NContainer::TCVector<NSys::CIoSpan> Spans;
			Spans.f_Reserve(pMessage->f_GetSpanCount());
			pMessage->f_VisitSpans
				(
					[&](uint8 const *_pData, umint _nSpanBytes)
					{
						Spans.f_InsertLast(NSys::CIoSpan{.m_pData = _pData, .m_nBytes = _nSpanBytes});
					}
				)
			;

			auto &NewMessage = Internal.f_QueueViewMessage
				(
					EOpcode_BinaryFrame
					, fg_Move(Spans)
					, nBytes
					, fg_Construct<TCPayloadOwner<NStorage::TCSharedPointer<NStream::CBinaryStorage const>>>(fg_Move(pMessage))
					, _Priority
				)
			;

			if (iMessage + 1 == nMessages)
				Future = NewMessage.m_Promise.f_CreateNew().f_Future();
		}

		fp_ScheduleUpdateSend();
		co_return co_await fg_Move(Future);
	}

	void CWebSocketActor::fp_StateAdded(NNetwork::ENetTCPState _StateAdded)
	{
		fp_ProcessState(_StateAdded);
	}

	void CWebSocketActor::fp_ScheduleUpdateSend()
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bFlushSendScheduled)
			return;
		Internal.m_bFlushSendScheduled = true;

		// Rides the actor's own run queue, so every send call already queued behind the current
		// one runs before it: a burst of small messages queued in one batch is gathered into a
		// single vectored send instead of paying one send per message
		fg_ThisActor(this).f_Bind<&CWebSocketActor::fp_FlushSend>().f_DiscardResult();
	}

	void CWebSocketActor::fp_FlushSend()
	{
		auto &Internal = *mp_pInternal;
		Internal.m_bFlushSendScheduled = false;

		if (f_IsDestroyed())
			return;

		fp_UpdateSend();
	}

	void CWebSocketActor::fp_TryActivateCompletionIo(bool _bSubmitReceive)
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bCompletionIo)
			return;

		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
			return;

		// Decided once per connection, at establishment: by now any transport handshake beneath
		// has completed, so a null here is a socket that will never support completion transfers
		auto *pCompletionIo = Internal.m_pSocket->f_GetCompletionIo();
		if (!pCompletionIo)
			return;

		Internal.m_pCompletionIo = pCompletionIo;
		Internal.m_bCompletionIo = true;
		Internal.m_pOpTracker = fg_Construct();

		// Said before the first operation, so the socket's synchronous entry points start refusing
		// now rather than whenever one of them first happens to be refused by the kernel
		pCompletionIo->f_OnCompletionActivated();

		DMibLog(DebugVerbose3, " ++++ {} {} Completion transfers active", fg_ThisActor(this), !Internal.m_bClient);

		if (_bSubmitReceive)
			fp_StartReceiveStream();
	}

	void CWebSocketActor::fp_StartReceiveStream()
	{
		auto &Internal = *mp_pInternal;

		// A stream is started at most once per socket, and once its terminal has been delivered
		// nothing arms again — a readiness edge arriving after the end of the stream would
		// otherwise ask for a second stream on a registration that already had its one
		if (Internal.m_bReceiveStreamActive || Internal.m_bReceiveStreamEnded)
			return;

		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
			return;

		auto *pCompletionIo = Internal.f_GetCompletionIoReceive();
		if (!pCompletionIo)
			return;

		// The stream's flow control: buffer capacity outstanding across the whole pipeline,
		// counted down by each buffer's own destructor. When the limit parks the kernel side,
		// the release that crosses the resume threshold reschedules through this actor
		umint nBufferBytes = pCompletionIo->f_GetReceiveBufferBytes();
		auto pBackpressure = NStorage::TCSharedPointer<NSys::CIoStreamBackpressure>(fg_Construct());
		pBackpressure->m_nLimitBytes = NNetwork::fg_GetReceiveWindowBytes(*Internal.mp_pIo, nBufferBytes);
		pBackpressure->m_nResumeBytes = pBackpressure->m_nLimitBytes / 2;
		pBackpressure->m_fResume = [WeakThis = fg_ThisActor(this).f_Weak()]() mutable
			{
				if (auto This = WeakThis.f_Lock())
					This.f_Bind<&CWebSocketActor::fp_ReceiveWindowResume>().f_DiscardResult();
			}
		;
		Internal.m_pReceiveBackpressure = pBackpressure;

		bool bStarted = pCompletionIo->f_StartReceiveStream
			(
				fg_Move(pBackpressure)
				, [WeakThis = fg_ThisActor(this).f_Weak()](NSys::CIoStreamSegment &&_Segment) mutable
				{
					// Loop thread: hand the segment to the actor. The segment owns its buffer's
					// reference, so a job dropped mid-teardown frees it from the destructor
					if (auto This = WeakThis.f_Lock())
						This.f_Bind<&CWebSocketActor::fp_ReceiveSegment>(fg_Move(_Segment)).f_DiscardResult();
				}
			)
		;

		if (bStarted)
			Internal.m_bReceiveStreamActive = true;

		// A refusal means the socket is closing; the close paths take over
	}

	// A backpressure release crossing the resume threshold: the kernel side parked its ring, and
	// this is where it is rescheduled
	void CWebSocketActor::fp_ReceiveWindowResume()
	{
		auto &Internal = *mp_pInternal;

		if (f_IsDestroyed())
			return;

		if (Internal.m_pSocket && Internal.m_pSocket->f_IsValid() && Internal.m_pCompletionIo)
			Internal.m_pCompletionIo->f_ResumeReceiveStream();
	}

	// The transport can hold records of its own making — a key update acknowledgement, a session
	// ticket, the tail of a send it could not finish — that no frame of ours will carry out.
	// Completion transfers report no write readiness, so this is where they are noticed
	void CWebSocketActor::fp_DrainSocketOutput()
	{
		auto &Internal = *mp_pInternal;

		auto *pCompletionIo = Internal.f_GetCompletionIoSend();
		if (!pCompletionIo)
			return;

		if (!pCompletionIo->f_HasPendingOutput())
			return;

		fp_SubmitSendOp(true);
	}

	// _bContinue carries on a transfer the socket has not finished with: it may have nothing new to
	// offer, and the operation exists to move what the socket still holds rather than what is queued
	void CWebSocketActor::fp_SubmitSendOp(bool _bContinue, umint _iInheritedReservation)
	{
		auto &Internal = *mp_pInternal;

		// A continuation carries on a transfer an earlier call reserved for, so it takes that
		// reservation rather than one of its own. Held from here, above every way out of this
		// function: a continuation that cannot be submitted has to give the reservation back, or
		// nothing ever will and the queue stays permanently spoken for
		umint iReservation = _bContinue ? _iInheritedReservation : Internal.mc_iNoReservation;

		auto fReleaseOnFailure = NMib::g_OnScopeExit / [&]
			{
				if (iReservation == Internal.mc_iNoReservation)
					return;

				auto &Reservation = Internal.m_SendReservations[iReservation];

				// Tearing the connection down gives every reservation back at once, so an
				// operation still in flight then finds its own already accounted for
				if (!Reservation.m_bInUse)
					return;

				DMibFastCheck(Internal.m_nOutgoingSubmitted >= Reservation.m_nBytes);

				Internal.m_nOutgoingSubmitted -= Reservation.m_nBytes;
				Reservation.m_bInUse = false;
				--Internal.m_nSendReservationsInUse;
				Internal.m_iFreeSendReservations.f_InsertLast(uint32(iReservation));
			}
		;

		if (!Internal.m_nOutgoingQueuedBytes && !_bContinue)
			return;

		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
			return;

		auto *pCompletionIo = Internal.f_GetCompletionIoSend();
		if (!pCompletionIo)
			return;

		// The send window: the path already holds as much unreleased as it has earned, so the
		// batch stays in the queue — the release that shrinks the count re-drives this. New
		// batches only, like the staging gate below
		if (!_bContinue && pCompletionIo->f_IsSendWindowFull(Internal.m_nSendBytesUnreleased, Internal.fp_SendWindowStartBytes()))
		{
#if DMibConfig_IoDebug_Enable
			if (auto *pStats = NNetwork::fg_NetIoStats())
				pStats->m_nSendBlocked.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
#endif

			return;
		}

		// Nothing new may be sealed: the transport cannot take another batch, and the release
		// upcall is what re-drives this. The gate is for new plaintext only — a continuation
		// seals nothing, it moves ciphertext the transport already holds, and refusing it when
		// the send buffer is full is a deadlock: only sending makes the buffer not-full again
		if (!_bContinue && !pCompletionIo->f_CanSubmitSend())
		{
#if DMibConfig_IoDebug_Enable
			if (auto *pStats = NNetwork::fg_NetIoStats())
			{
				pStats->m_nSendBlocked.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
				pStats->m_LastPumpPending.f_Store(Internal.m_nOutgoingQueuedBytes, NAtomic::gc_MemoryOrder_Relaxed);
				pStats->m_LastPumpPinned.f_Store(Internal.m_nOutgoingSubmitted, NAtomic::gc_MemoryOrder_Relaxed);
				pStats->m_LastPumpOpsInUse.f_Store(pCompletionIo->f_HasSendOperationInFlight() ? 1 : 0, NAtomic::gc_MemoryOrder_Relaxed);
				pStats->m_LastPumpOpsUnresolved.f_Store(pCompletionIo->f_HasPendingOutput() ? 1 : 0, NAtomic::gc_MemoryOrder_Relaxed);
			}
#endif

			return;
		}

		if (!_bContinue && Internal.m_nOutgoingQueuedBytes <= Internal.m_nOutgoingSubmitted)
			return;


		// A new batch needs a reservation slot for its bytes, and the slots outlive the socket's
		// own operation records by one drain — the chain carrier's reservation is inherited by
		// its continuation and only settles when the chain reports. When every slot is still
		// spoken for the batch waits; the completion that frees one re-drives this
		if (!_bContinue)
		{
			if (Internal.m_nSendReservationsInUse >= Internal.fp_MaxSendReservations(pCompletionIo))
				return;

#if DMibConfig_IoDebug_Enable
			if (auto *pStats = NNetwork::fg_NetIoStats())
			{
				uint64 nOutstanding = Internal.m_nSendReservationsInUse + 1;
				uint64 nMax = pStats->m_nSendMaxOutstanding.f_Load(NAtomic::gc_MemoryOrder_Relaxed);
				while (nMax < nOutstanding && !pStats->m_nSendMaxOutstanding.f_CompareExchangeWeak(nMax, nOutstanding, NAtomic::gc_MemoryOrder_Relaxed))
				{
				}
			}
#endif
		}

		// A continuation offers nothing: the queue still holds the plaintext of the transfer the
		// socket is carrying, because that is only consumed once it reports, and gathering from it
		// again would hand the same bytes over twice
		NSys::CIoSpan Spans[NNetwork::ICSocket::mc_MaxSendSpans];
		umint nSpans = 0;
		umint nGatheredBytes = 0;
		NContainer::TCVector<NStorage::TCSharedPointer<CPayloadOwner>> KeepAlives;
		NStorage::TCSharedPointer<NContainer::CIOByteVector> pArenaCopy;
		if (!_bContinue)
		{
			nGatheredBytes = Internal.f_GatherSendSpans(Spans, nSpans, KeepAlives, pArenaCopy);
			if (!nGatheredBytes)
				return;
		}

		DMibLog(DebugVerbose3, " ++++ {} {} Submitting send of {}", fg_ThisActor(this), !Internal.m_bClient, nGatheredBytes);

		// Reserved rather than consumed: the queue still holds these bytes until the operation
		// reports, because a short write leaves part of them still to send
		if (!_bContinue)
		{
			if (!Internal.m_iFreeSendReservations.f_IsEmpty())
				iReservation = Internal.m_iFreeSendReservations.f_Pop();
			else
			{
				iReservation = Internal.m_SendReservations.f_GetLen();
				Internal.m_SendReservations.f_InsertLast(CInternal::CSendReservation());
			}

			++Internal.m_nSendReservationsInUse;

			DMibFastCheck(nGatheredBytes < (umint(1) << (sizeof(umint) * 8 - 1)));

			Internal.m_SendReservations[iReservation].m_nBytes = nGatheredBytes;
			Internal.m_SendReservations[iReservation].m_bInUse = true;
			Internal.m_nOutgoingSubmitted += nGatheredBytes;
		}

		// Each functor holds the tracker until it is destroyed — the completion and the buffer
		// release both — so the destroy fence waits until the kernel has let go of the pages,
		// not just reported. A functor the submit refuses or a throw drops releases the same way
		bool bSubmitted = pCompletionIo->f_SubmitSendVectored
			(
				Spans
				, nSpans
				, [Hold = NConcurrency::CIoCompletionOpHold(Internal.m_pOpTracker), iReservation, WeakThis = fg_ThisActor(this).f_Weak()](NSys::CIoCompletion _Result) mutable
				{
					if (auto This = WeakThis.f_Lock())
						This.f_Bind<&CWebSocketActor::fp_SendCompleted>(_Result, iReservation).f_DiscardResult();
				}
				,
				[Hold = NConcurrency::CIoCompletionOpHold(Internal.m_pOpTracker), KeepAlives = fg_Move(KeepAlives), pArenaCopy = fg_Move(pArenaCopy), WeakThis = fg_ThisActor(this).f_Weak(), nGatheredBytes](umint _iTransfer) mutable
				{
					// The kernel is done with the gathered pages; the keep alives this functor
					// carried can finally go, and the socket gets its buffer back on the
					// actor's thread
					KeepAlives.f_Clear();
					pArenaCopy.f_Clear();

					if (auto This = WeakThis.f_Lock())
						This.f_Bind<&CWebSocketActor::fp_SendBufferReleased>(_iTransfer, nGatheredBytes).f_DiscardResult();
				}
			)
		;

		if (bSubmitted)
		{
			fReleaseOnFailure.f_Clear();
			Internal.m_nSendBytesUnreleased += nGatheredBytes;
		}
		else
		{
			// Terminal by contract, and the queue is still holding the plaintext this was meant to
			// carry. Left alone it would sit there with nothing to drive it and the send futures
			// would never resolve, so the connection ends here rather than going quiet
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, "Socket refused a send", true, EWebSocketCloseOrigin_Remote);
			return;
		}

#if DMibConfig_IoDebug_Enable
		if (auto *pStats = NNetwork::fg_NetIoStats())
		{
			pStats->m_nSendSubmits.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
			if (_bContinue)
				pStats->m_nSendContinuations.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
		}
#endif

		++Internal.m_nSendOpsInFlight;
	}

	void CWebSocketActor::fp_ReceiveSegment(NSys::CIoStreamSegment &&_Segment)
	{
		auto &Internal = *mp_pInternal;

		if (f_IsDestroyed())
			return;

		auto &Segment = _Segment;
		bool bTerminal = Segment.m_Status != NSys::EIoCompletionStatus::mc_Done || !Segment.m_nBytes;

		if (bTerminal)
		{
			Internal.m_bReceiveStreamActive = false;
			Internal.m_bReceiveStreamEnded = true;
		}

		bool bSocketUsable = Internal.m_pSocket && Internal.m_pSocket->f_IsValid();
		if (!bSocketUsable || !Internal.m_pCompletionIo)
		{
			Internal.f_TryReleaseDeferredReceiveState();
			return;
		}

		if (Segment.m_Status == NSys::EIoCompletionStatus::mc_Cancelled)
		{
			// Resolved so the socket hears the stream is over, with nothing to deliver
			NSys::CIoCompletion Result;
			Internal.m_pCompletionIo->f_ResolveReceiveSegment(Segment, nullptr, 0, Result);
			Internal.f_TryReleaseDeferredReceiveState();
			return;
		}

#if DMibConfig_Tests_Enable
		// Mirror the readiness path's receive stop: the bytes are banked so nothing is lost,
		// but they are not delivered and the timeout stopwatch is deliberately not restarted.
		// The debug re-drive flushes when the flag clears
		bool bDebugBank = Internal.m_bDebugNoProcessing || Internal.m_bDebugNoProcessingReceive;
#else
		constexpr bool bDebugBank = false;
#endif

		// A socket whose segments are the payload as delivered hands them back as a shared view
		// of the very buffer the kernel filled: the frame parser consumes the view where it
		// lies — payload into the message buffer for a direct read, the rest into the incoming
		// pages — one copy from kernel memory to its destination, exactly like the readiness
		// path read
		if (!bTerminal)
		{
			NSys::CIoCompletion SharedResult;
			NContainer::CSharedByteVector SharedData;
			if (Internal.m_pCompletionIo->f_ResolveReceiveSegmentShared(Segment, SharedData, SharedResult))
			{
#if DMibConfig_IoDebug_Enable
				if (auto *pStats = NNetwork::fg_NetIoStats())
				{
					pStats->m_nRecvSharedDeliveries.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
					pStats->m_nRecvSharedBytes.f_FetchAdd(SharedData.f_GetLen(), NAtomic::gc_MemoryOrder_Relaxed);
				}
#endif
				try
				{
					// Segments resolve as shared views on this connection, so unmasked binary
					// frames may take the storage direct read and reference these buffers
					Internal.m_bReceiveStreamShared = true;

					Internal.m_nReceivedBytes += SharedData.f_GetLen();

					umint iOffset = 0;
					while (iOffset < SharedData.f_GetLen())
					{
						// Readiness parity: bytes arriving after the disconnect are dropped —
						// the close callback has already run. Disconnecting still processes:
						// that state is waiting for the peer's close frame
						if (Internal.m_State == EState_Disconnected)
							break;

						if (Internal.m_nDirectReadRemaining)
						{
							umint nRemaining = (umint)Internal.m_nDirectReadRemaining;
							umint nCopy = fg_Min(nRemaining, SharedData.f_GetLen() - iOffset);

							if (Internal.m_bDirectReadToStorage)
							{
								// The payload piece stays where the kernel put it: the message
								// references the receive buffer, and the buffer goes home when
								// the consumer lets go of the message
								auto &Target = Internal.m_bPendingMessage ? Internal.m_PendingMessage : Internal.m_NextMessage;
								Target.m_Storage.f_AppendShared(NContainer::CSharedByteVector(SharedData, iOffset, nCopy));
							}
							else
							{
								auto &Dest = *Internal.m_pDirectReadData;
								umint FrameLength = (umint)Internal.m_NextMessage.m_Length;
								umint FillOffset = Internal.m_DirectReadFrameStart + FrameLength - nRemaining;
								NMemory::fg_MemCopy(Dest.f_GetArray() + FillOffset, SharedData.f_GetArray() + iOffset, nCopy);
							}

							Internal.m_nDirectReadRemaining -= nCopy;
							iOffset += nCopy;

							if (!Internal.m_nDirectReadRemaining && !bDebugBank)
							{
								Internal.f_FinishDirectReadFrame();
								if (Internal.m_pSocket && Internal.m_pSocket->f_IsValid())
									fp_UpdateSend();
							}
						}
						else
						{
							// Fed to the parser a small gulp at a time: as soon as it has a
							// data frame's header it flips to the direct read, and the loop
							// hands the payload over as views of this very segment instead of
							// bouncing it through the pages and back out. Only headers, small
							// frames and a page-size prefix ever take the copy route
							umint nInsert = fg_Min(SharedData.f_GetLen() - iOffset, umint(4096));
							Internal.m_IncomingData.f_InsertBack(SharedData.f_GetArray() + iOffset, nInsert);
							iOffset += nInsert;

							if (!bDebugBank)
							{
								fp_ProcessIncoming();
								if (Internal.m_pSocket && Internal.m_pSocket->f_IsValid())
									fp_UpdateSend();
							}
						}

						if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
						{
							Internal.f_TryReleaseDeferredReceiveState();
							return;
						}
					}
				}
				catch (NCryptography::CExceptionCryptography const &_Exception)
				{
					fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket error: {}", _Exception.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
					return;
				}
				catch (NNetwork::CExceptionNet const &_Exception)
				{
					fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket error: {}", _Exception.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
					return;
				}

				if (!bDebugBank)
				{
					Internal.f_OnReceivedData();
					fp_DrainSocketOutput();
				}

				return;
			}
		}

		// What the kernel delivered becomes what this asked for here, on the actor's thread. The
		// target is chosen per round — a large data frame's payload lands straight in the message
		// buffer at its fill offset, everything else goes through the chunk into the incoming
		// pages — and one segment can span both
		bool bResolvedSegment = false;
		bool bError = false;

		try
		{
			for (;;)
			{
				void *pTarget;
				umint nTarget;
				bool bDirect = Internal.m_nDirectReadRemaining && Internal.m_State != EState_Disconnected;
				// Shared connections consume in the loop above; a storage direct read can
				// never be pending on a connection whose segments resolve into buffers
				DMibFastCheck(!Internal.m_bDirectReadToStorage);
				if (bDirect)
				{
					auto &Dest = *Internal.m_pDirectReadData;
					umint FrameLength = (umint)Internal.m_NextMessage.m_Length;
					umint nRemaining = (umint)Internal.m_nDirectReadRemaining;
					pTarget = Dest.f_GetArray() + Internal.m_DirectReadFrameStart + FrameLength - nRemaining;
					nTarget = nRemaining;
				}
				else
				{
					if (Internal.m_ReceiveChunk.f_GetLen() != gc_ReceiveChunkSize)
						Internal.m_ReceiveChunk.f_SetLen(gc_ReceiveChunkSize, false);
					pTarget = Internal.m_ReceiveChunk.f_GetArray();
					nTarget = gc_ReceiveChunkSize;
				}

				NSys::CIoCompletion Result;
				bool bProduced;
				if (!bResolvedSegment)
				{
					bProduced = Internal.m_pCompletionIo->f_ResolveReceiveSegment(Segment, pTarget, nTarget, Result);
					bResolvedSegment = true;

					if (bProduced && Result.m_Status == NSys::EIoCompletionStatus::mc_Error)
					{
						bError = true;
						break;
					}
				}
				else
					bProduced = Internal.m_pCompletionIo->f_ResolveHeld(pTarget, nTarget, Result);

				if (!bProduced || !Result.m_nBytes)
					break;

				DMibLog(DebugVerbose3, " ++++ {} {} Received stream bytes {}", fg_ThisActor(this), !Internal.m_bClient, Result.m_nBytes);
				Internal.m_nReceivedBytes += Result.m_nBytes;

				// Readiness parity: bytes arriving after the disconnect are dropped, exactly like
				// fp_ProcessIncoming's disconnected case — the close callback has already run, so
				// delivering now would hand the consumer data after its close. The stream keeps
				// draining, which is what keeps the peer's receive window from wedging.
				// Disconnecting is deliberately not included — that state is still waiting for
				// the peer's close frame
				if (Internal.m_State == EState_Disconnected)
					continue;

				if (bDirect)
				{
					DMibFastCheck(Result.m_nBytes <= Internal.m_nDirectReadRemaining);
					Internal.m_nDirectReadRemaining -= Result.m_nBytes;

					if (!Internal.m_nDirectReadRemaining && !bDebugBank)
					{
						Internal.f_FinishDirectReadFrame();
						if (Internal.m_pSocket && Internal.m_pSocket->f_IsValid())
							fp_UpdateSend();
					}
				}
				else
				{
					Internal.m_IncomingData.f_InsertBack(Internal.m_ReceiveChunk.f_GetArray(), Result.m_nBytes);

					if (!bDebugBank)
					{
						fp_ProcessIncoming();
						if (Internal.m_pSocket && Internal.m_pSocket->f_IsValid())
							fp_UpdateSend();
					}
				}

				if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
				{
					Internal.f_TryReleaseDeferredReceiveState();
					return;
				}
			}
		}
		catch (NCryptography::CExceptionCryptography const &_Exception)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket error: {}", _Exception.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
			return;
		}
		catch (NNetwork::CExceptionNet const &_Exception)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket error: {}", _Exception.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
			return;
		}

		if (Segment.m_Status == NSys::EIoCompletionStatus::mc_Error)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket receive error: {}", NNetwork::fg_FormatSocketIoError(Segment.m_Error)), true, EWebSocketCloseOrigin_Remote);
			return;
		}

		if (bError)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, "Socket receive failed", true, EWebSocketCloseOrigin_Remote);
			return;
		}

		if (bTerminal)
		{
			// End of stream. The close-class states that waited for the stream to run dry are
			// honored now that everything it held has been delivered
			NNetwork::ENetTCPState DeferredStates = Internal.m_DeferredCloseStates;
			Internal.m_DeferredCloseStates = NNetwork::ENetTCPState_None;
			if (DeferredStates)
				fp_ProcessState(DeferredStates);

			Internal.f_TryReleaseDeferredReceiveState();
			return;
		}

		if (!bDebugBank)
		{
			Internal.f_OnReceivedData();
			fp_DrainSocketOutput();
		}
	}

	void CWebSocketActor::fp_SendCompleted(NSys::CIoCompletion _Result, umint _iReservation)
	{
		auto &Internal = *mp_pInternal;
		DMibFastCheck(Internal.m_nSendOpsInFlight);
		--Internal.m_nSendOpsInFlight;

		auto fReleaseReservation = [&]()
			{
				// Only once the transfer has actually been reported. A socket that says it is not
				// done with these bytes yet is carrying them still, and releasing here would let
				// the next gather offer them a second time. A continuation reserved nothing of its
				// own, and says so with no reservation at all
				if (_iReservation == Internal.mc_iNoReservation)
					return;

				auto &Reservation = Internal.m_SendReservations[_iReservation];

				// Tearing the connection down gives every reservation back at once, so an
				// operation still in flight then finds its own already accounted for
				if (!Reservation.m_bInUse)
					return;

				DMibFastCheck(Internal.m_nOutgoingSubmitted >= Reservation.m_nBytes);

				Internal.m_nOutgoingSubmitted -= Reservation.m_nBytes;
				Reservation.m_bInUse = false;
				--Internal.m_nSendReservationsInUse;
				Internal.m_iFreeSendReservations.f_InsertLast(uint32(_iReservation));
			}
		;

		if (f_IsDestroyed())
			return;

		// As on the receive side: a transport that frames what it sends reports the bytes that
		// left the machine, and turns them into the caller's here. Records it has produced but
		// not yet placed are why this can say the transfer is not over
		bool bSocketUsable = Internal.m_pSocket && Internal.m_pSocket->f_IsValid();
		bool bResolved = true;
		if (bSocketUsable && Internal.m_pCompletionIo)
			bResolved = Internal.m_pCompletionIo->f_ResolveSend(_Result);

		if (_Result.m_Status == NSys::EIoCompletionStatus::mc_Cancelled || !bSocketUsable)
		{
			Internal.m_nOutgoingSubmitted = 0;
			Internal.fp_ResetSendReservations();
			Internal.f_TryReleaseDeferredReceiveState();
			return;
		}

		if (!bResolved)
		{
			// The socket still holds these bytes, so the reservation travels to the operation that
			// carries on with them
			fp_SubmitSendOp(true, _iReservation);
			return;
		}

		fReleaseReservation();

		if (_Result.m_Status == NSys::EIoCompletionStatus::mc_Error)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket send error: {}", NNetwork::fg_FormatSocketIoError(_Result.m_Error)), true, EWebSocketCloseOrigin_Remote);
			return;
		}

		DMibLog(DebugVerbose3, " ++++ {} {} Send completion {}", fg_ThisActor(this), !Internal.m_bClient, _Result.m_nBytes);

		if (_Result.m_nBytes)
		{
			Internal.f_ConsumeSentBytes(_Result.m_nBytes);
			Internal.f_OnSentData();
		}

		// Frames may have queued while the operation was in flight, and the disconnect path wants
		// its shutdown once the queue runs dry; fp_UpdateSend's completion branch covers both
		fp_UpdateSend();
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendText(NStr::CStr _Data, uint32 _Priority)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugFailSends)
			co_return DMibErrorInstance("Debug fail send");
#endif

		NStr::CStr Data = _Data;

		umint nBytes = Data.f_GetLen();

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		if (nBytes > Internal.m_Settings.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		// The string is moved into the keep alive first, so the span references the bytes
		// where they will stay for as long as the message is queued
		NStorage::TCSharedPointer<TCPayloadOwner<NStr::CStr>> pStringOwner = fg_Construct(fg_Move(Data));

		// The spans are taken before the owner is handed over: argument evaluation order is
		// unspecified, so reading through the pointer in one argument while moving it in
		// another would be a race between the two
		auto Spans = fg_MakeSpanVector((uint8 const *)pStringOwner->m_Owner.f_GetStr(), nBytes);

		auto &NewMessage = Internal.f_QueueViewMessage
			(
				EOpcode_TextFrame
				, fg_Move(Spans)
				, nBytes
				, NStorage::TCSharedPointer<CPayloadOwner>(fg_Move(pStringOwner))
				, _Priority
			)
		;

		auto Future = NewMessage.m_Promise.f_CreateNew().f_Future();

		fp_ScheduleUpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	// The payload spans are handed to the socket in place, so the buffers must stay frozen
	// until the future resolves; the const pointee is what enforces that
	NConcurrency::TCFuture<void> CWebSocketActor::f_SendTextBuffer(NStorage::TCSharedPointer<CMaybeSecureByteVector const> _pMessage, uint32 _Priority)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugFailSends)
			co_return DMibErrorInstance("Debug fail send");
#endif

		auto &Message = *_pMessage;

		umint nBytes = Message.f_GetLen();

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		if (nBytes > Internal.m_Settings.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		auto &NewMessage = Internal.f_QueueViewMessage
			(
				EOpcode_TextFrame
				, fg_MakeSpanVector(Message.f_GetArray(), nBytes)
				, nBytes
				, fg_Construct<TCPayloadOwner<NStorage::TCSharedPointer<CMaybeSecureByteVector const>>>(fg_Move(_pMessage))
				, _Priority
			)
		;

		auto Future = NewMessage.m_Promise.f_CreateNew().f_Future();

		fp_ScheduleUpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendTextBuffers(NStorage::TCSharedPointer<CMessageBuffers const> _pMessageBuffers, uint32 _Priority)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugFailSends)
			co_return DMibErrorInstance("Debug fail send");
#endif

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		auto &MessageBuffers = *_pMessageBuffers;

		auto &MessageMarkers = MessageBuffers.m_Markers;
		auto &Message = MessageBuffers.m_Data;
		umint const *pMessageMarkersArray = MessageMarkers.f_GetArray();
		umint nMessages = MessageMarkers.f_GetLen();
		if (!nMessages)
			co_return {};

		umint MessageLength = Message.f_GetLen();
		uint8 const *pMessageArray = Message.f_GetArray();

		for (umint iMessage = 0; iMessage < nMessages; ++iMessage)
		{
			umint iStart = pMessageMarkersArray[iMessage];
			umint iEnd = iMessage == (nMessages - 1) ? MessageLength : pMessageMarkersArray[iMessage + 1];
			umint nBytes = iEnd - iStart;

			if (nBytes > Internal.m_Settings.m_MaxMessageSize)
				co_return DMibErrorInstance("Message is bigger than max message size");
		}

		NConcurrency::TCFuture<void> Future;

		// Every message is a slice of the same buffer set, so they all reference it through
		// one owner instead of one per message
		NStorage::TCSharedPointer<CPayloadOwner> pBuffersOwner = fg_Construct<TCPayloadOwner<NStorage::TCSharedPointer<CMessageBuffers const>>>(fg_Move(_pMessageBuffers));

		for (umint iMessage = 0; iMessage < nMessages; ++iMessage)
		{
			bool bIsLastMessage = iMessage == (nMessages - 1);

			umint iStart = pMessageMarkersArray[iMessage];

			umint nBytes;
			if (bIsLastMessage)
				nBytes = MessageLength - iStart;
			else
				nBytes = pMessageMarkersArray[iMessage + 1] - iStart;

			auto &OutMsg = Internal.f_QueueViewMessage
				(
					EOpcode_TextFrame
					, fg_MakeSpanVector(pMessageArray + iStart, nBytes)
					, nBytes
					, fg_TempCopy(pBuffersOwner)
					, _Priority
				)
			;

			if (bIsLastMessage)
			{
				// OK, assuming messages are sent in the order they are queued attaching the promise to the last message should
				// behave as assumed.
				Future = OutMsg.m_Promise.f_CreateNew().f_Future();
			}
		}

		fp_ScheduleUpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendPing(NStorage::TCSharedPointer<NContainer::CIOByteVector const> _ApplicationData)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;
		umint nBytes = _ApplicationData->f_GetLen();
		if (nBytes > Internal.m_Settings.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		auto &NewMessage = Internal.f_QueueMessage(EOpcode_Ping, _ApplicationData, TCLimitsInt<uint32>::mc_Max);
		auto Future = NewMessage.m_Promise.f_CreateNew().f_Future();

		fp_ScheduleUpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendPong(NStorage::TCSharedPointer<NContainer::CIOByteVector const> _ApplicationData)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;
		umint nBytes = _ApplicationData->f_GetLen();
		if (nBytes > Internal.m_Settings.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		auto &NewMessage = Internal.f_QueueMessage(EOpcode_Pong, _ApplicationData, TCLimitsInt<uint32>::mc_Max);
		auto Future = NewMessage.m_Promise.f_CreateNew().f_Future();

		fp_ScheduleUpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	// The kernel released a send's buffers. For a socket that stages what it carries this is
	// what lets the next generation be filled, so anything parked behind the cap moves now
	void CWebSocketActor::fp_SendBufferReleased(umint _iTransfer, umint _nBytes)
	{
		auto &Internal = *mp_pInternal;

		// The window asks measure against this; a teardown may have zeroed the count already
		Internal.m_nSendBytesUnreleased -= fg_Min(_nBytes, Internal.m_nSendBytesUnreleased);

		if (f_IsDestroyed())
			return;

		bool bSocketUsable = Internal.m_pSocket && Internal.m_pSocket->f_IsValid();
		if (bSocketUsable && Internal.m_pCompletionIo)
			Internal.m_pCompletionIo->f_ResolveSendRelease(_iTransfer);

		if (!bSocketUsable)
			return;

		// A generation freeing is what un-parks staged ciphertext and queued payload alike
		fp_UpdateSend();
		fp_DrainSocketOutput();
	}


	// Frames are written into the shared arena in pieces, so a failure partway would leave a
	// header with no mask or payload on the wire. That is not recoverable, so this and the
	// segmented form terminate rather than unwinding out of a half written frame
	void CWebSocketActor::CInternal::f_SendMessage(EOpcode _Opcode, uint8 const *_pData, umint _nBytes, bool _bFinished) noexcept
	{
		umint LenBefore = m_OutgoingData.f_GetLen();
		auto TrackBytes = g_OnScopeExit / [&]
			{
				f_TrackArenaBytes(m_OutgoingData.f_GetLen() - LenBefore);
			}
		;

		CBinaryStreamPagedByteVector Stream(m_OutgoingData);

		bool bMask = m_bClient && m_bMaskFrames;

		uint8 Header0 = 0;
		if (_bFinished)
			Header0 |= uint8(0x01) << 7;

		Header0 |= ((uint8)_Opcode);

		uint8 Header1 = 0;

		if (bMask)
			Header1 |= uint8(0x01) << 7;

		if (_nBytes >= 65536)
			Header1 |= uint8(127);
		else if (_nBytes >= 126)
			Header1 |= uint8(126);
		else
			Header1 |= uint8(_nBytes);

		Stream << Header0;
		Stream << Header1;

		if (_nBytes >= 65536)
			Stream << uint64(_nBytes);
		else if (_nBytes >= 126)
			Stream << uint16(_nBytes);

		uint8 Mask[4] = {0};
		if (bMask)
		{
			NCryptography::fg_GenerateRandomData(Mask, sizeof(Mask));
			m_OutgoingData.f_InsertBack(Mask, sizeof(Mask));
		}

		if (_nBytes == 0)
			return;

		umint StartPos = m_OutgoingData.f_GetLen();
		m_OutgoingData.f_InsertBack(_pData, _nBytes);

		if (bMask)
		{
			m_OutgoingData.f_Mutate
				(
					StartPos
					, _nBytes
					, [&](umint _iStart, uint8 * _pPtr, umint _nBytes) -> bool
					{
						fs_ApplyMask(_pPtr, _iStart - StartPos, _nBytes, Mask);
						return true;
					}
				)
			;
		}
	}

	// Emits one frame of a view message, advancing the message cursors. The header goes
	// into the copy arena; on unmasked connections the payload is queued as views into the
	// message's shared spans, while masking clients copy and mask the frame's bytes into
	// the arena so the shared payload is never mutated
	void CWebSocketActor::CInternal::f_SendMessageFrameSegmented(COutgoingMessage &_Message, umint _nFrameBytes) noexcept
	{
		umint nRemaining = _Message.m_nTotalBytes - _Message.m_iPayloadSent;
		DMibFastCheck(_nFrameBytes <= nRemaining);

		bool bLastFrame = _nFrameBytes == nRemaining;
		EOpcode Opcode = _Message.m_iPayloadSent == 0 ? _Message.m_Opcode : EOpcode_ContinuationFrame;
		bool bFinished = bLastFrame && _Message.m_bFinished;
		bool bMask = m_bClient && m_bMaskFrames;

		// A short frame is copied into the arena next to its header instead of becoming its
		// own send span: a span costs a gather slot, a segment and a keep alive, which is
		// more than copying a few hundred bytes. Masked frames always copy, since the mask
		// is applied to the copy and the shared source bytes must stay untouched
		bool bCopyToArena = bMask || _nFrameBytes <= gc_CopySmallMessageThreshold;

		uint8 Mask[4] = {0};
		{
			umint LenBefore = m_OutgoingData.f_GetLen();
			CBinaryStreamPagedByteVector Stream(m_OutgoingData);

			uint8 Header0 = 0;
			if (bFinished)
				Header0 |= uint8(0x01) << 7;
			Header0 |= ((uint8)Opcode);

			uint8 Header1 = 0;
			if (bMask)
				Header1 |= uint8(0x01) << 7;

			if (_nFrameBytes >= 65536)
				Header1 |= uint8(127);
			else if (_nFrameBytes >= 126)
				Header1 |= uint8(126);
			else
				Header1 |= uint8(_nFrameBytes);

			Stream << Header0;
			Stream << Header1;

			if (_nFrameBytes >= 65536)
				Stream << uint64(_nFrameBytes);
			else if (_nFrameBytes >= 126)
				Stream << uint16(_nFrameBytes);

			if (bMask)
			{
				NCryptography::fg_GenerateRandomData(Mask, sizeof(Mask));
				m_OutgoingData.f_InsertBack(Mask, sizeof(Mask));
			}

			f_TrackArenaBytes(m_OutgoingData.f_GetLen() - LenBefore);
		}

		umint FrameOffset = 0;
		while (FrameOffset < _nFrameBytes)
		{
			NSys::CIoSpan const &Span = _Message.m_Spans[_Message.m_iSpan];
			umint nSpanRemaining = Span.m_nBytes - _Message.m_iSpanOffset;
			umint nThis = fg_Min(_nFrameBytes - FrameOffset, nSpanRemaining);
			uint8 const *pSpanData = (uint8 const *)Span.m_pData + _Message.m_iSpanOffset;

			if (bCopyToArena)
			{
				umint StartPos = m_OutgoingData.f_GetLen();
				m_OutgoingData.f_InsertBack(pSpanData, nThis);
				if (bMask)
				{
					umint MaskOffset = FrameOffset;
					m_OutgoingData.f_Mutate
						(
							StartPos
							, nThis
							, [&](umint _iStart, uint8 *_pPtr, umint _nBytes) -> bool
							{
								fs_ApplyMask(_pPtr, MaskOffset + (_iStart - StartPos), _nBytes, Mask);
								return true;
							}
						)
					;
				}
				f_TrackArenaBytes(nThis);
			}
			else
			{
				auto &Segment = m_OutgoingSegments.f_Insert();
				Segment.m_Kind = COutgoingSegment::EKind::mc_View;
				Segment.m_pData = pSpanData;
				Segment.m_nBytes = nThis;
				Segment.m_pOwnerKeepAlive = _Message.m_pOwner;
				m_pLastOutgoingSegment = &Segment;
				m_nOutgoingQueuedBytes += nThis;
			}

			_Message.m_iSpanOffset += nThis;
			if (_Message.m_iSpanOffset == Span.m_nBytes)
			{
				++_Message.m_iSpan;
				_Message.m_iSpanOffset = 0;
			}
			FrameOffset += nThis;
		}

		_Message.m_iPayloadSent += _nFrameBytes;
	}

	void CWebSocketActor::fp_Disconnect(EWebSocketStatus _Status, NStr::CStr const &_Reason, bool _bFatal, EWebSocketCloseOrigin _Origin, bool _bRemoteTransportClosed)
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_State == EState_Disconnected)
		{
			if (_bFatal)
			{
				Internal.m_pSocket.f_Clear();
				Internal.f_ShutdownDone(_Reason);
			}
			DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 1 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
			return; // Already disconnected
		}

		if (Internal.m_State == EState_Connected || Internal.m_State == EState_Disconnecting)
		{
			auto WasState = Internal.m_State;
			if (!_bFatal && Internal.m_State == EState_Connected)
			{
				// Send packet to other side
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 2 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);

				// A close can arrive with unawaited sends still queued. Local close: everything
				// queued first enters the stream ahead of the close frame, which parks and is
				// emitted by the bounded incremental framing when the queue runs dry (framing
				// the whole backlog at once would copy every payload into the arena in one
				// step).
				//
				// Remote close: RFC 6455 §5.5.1 wants the reply sent as soon as practical,
				// delayed only for the message in transit. A started fragment sequence
				// completes; unstarted messages are dropped (their destructors settle the
				// promises, as at shutdown). Queued pongs stay — tiny, legal between
				// fragments, and owed. Queued pings are dropped — they solicit replies
				// nothing will wait for
				if (_Origin == EWebSocketCloseOrigin_Remote && !Internal.m_PendingMessages.f_IsEmpty())
				{
					auto *pFragmentingList = Internal.m_pLastPendingMessagesList;

					NContainer::TCVector<NContainer::TCLinkedList<COutgoingMessage> *> DropLists;
					for (auto &List : Internal.m_PendingMessages)
					{
						if (&List == pFragmentingList)
							continue;

						if (Internal.m_PendingMessages.fs_GetKey(List) == TCLimitsInt<uint32>::mc_Max)
							continue;

						DropLists.f_InsertLast(&List);
					}
					for (auto *pList : DropLists)
						Internal.m_PendingMessages.f_Remove(pList);

					if (auto *pControlList = Internal.m_PendingMessages.f_FindEqual(TCLimitsInt<uint32>::mc_Max))
					{
						NContainer::TCVector<COutgoingMessage *> DropPings;
						for (auto &Message : *pControlList)
						{
							if (Message.m_Opcode == EOpcode_Ping)
								DropPings.f_InsertLast(&Message);
						}
						for (auto *pMessage : DropPings)
							pControlList->f_Remove(*pMessage);

						if (pControlList->f_IsEmpty())
							Internal.m_PendingMessages.f_Remove(pControlList);
					}

					if (pFragmentingList)
					{
						NContainer::TCVector<COutgoingMessage *> DropMessages;
						bool bProtected = true;
						for (auto &Message : *pFragmentingList)
						{
							if (bProtected)
							{
								bProtected = !Message.m_bFinished;
								continue;
							}

							DropMessages.f_InsertLast(&Message);
						}
						for (auto *pMessage : DropMessages)
							pFragmentingList->f_Remove(*pMessage);
					}
				}

				if (_Status != EWebSocketStatus_NoStatusReceived)
				{
					DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 3 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
					NStream::CBinaryStreamMemory<NStream::CBinaryStreamBigEndian> Stream;
					Stream << uint16(_Status);

					NStr::CStr Reason = _Reason;
					if (Reason.f_GetLen() > mc_MaxCloseMessageLength)
					{
						Reason = Reason.f_Left(mc_MaxCloseMessageLength);
						DMibLog(Warning, "Cut off Websocket close reason:\n   {}\n   {}", _Reason, Reason);
					}

					umint ReasonLen = Reason.f_GetLen();
					if (ReasonLen != 0)
						Stream.f_FeedBytes(Reason.f_GetStr(), Reason.f_GetLen());

					Internal.m_CloseFramePayload = Stream.f_MoveVector();
				}
				else
				{
					DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 4 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
					Internal.m_CloseFramePayload.f_Clear();
				}

				Internal.m_bCloseFramePending = true;
				Internal.f_WriteCloseFrameWhenDrained();

				Internal.m_State = EState_Disconnecting;
				fp_UpdateSend();
			}
			if (_Origin == EWebSocketCloseOrigin_Remote)
			{
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 5 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
				Internal.m_CloseInfo.m_Status = _Status;
				Internal.m_CloseInfo.m_Reason = _Reason;
				if (Internal.m_ClosePromise)
				{
					DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 6 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
					Internal.m_ClosePromise->f_SetResult(Internal.m_CloseInfo);
					Internal.m_ClosePromise.f_Clear();
				}
				if (!Internal.m_bOnCloseCalled)
				{
					DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 7 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
					Internal.m_bOnCloseCalled = true;
					if (Internal.m_fOnClose.f_ShouldCall())
						Internal.m_fOnClose.f_CallDiscard(_Status, _Reason, _Origin);
				}

				if (!_bFatal)
				{
					DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 8 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
					if (WasState == EState_Connected)
					{
						DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 9 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
						Internal.m_State = EState_Disconnected;

						// A transport closure without a close frame has failed the connection: nothing
						// queued can reach the peer, and no further socket events drive the
						// incremental framing. The undeliverable messages are dropped (destructors
						// settle the promises) and the parked close frame goes out best-effort, like
						// the crossing-close path below. A close frame from the peer stays on the
						// incremental path instead — the peer keeps reading until it has our reply
						if (_bRemoteTransportClosed)
						{
							Internal.m_PendingMessages.f_Clear();
							Internal.m_pLastPendingMessagesList = nullptr;

							if (Internal.m_bCloseFramePending)
							{
								Internal.m_bCloseFramePending = false;
								NContainer::CByteVector Payload = fg_Move(Internal.m_CloseFramePayload);
								Internal.f_SendMessage(EOpcode_ConnectionClose, Payload.f_GetArray(), Payload.f_GetLen(), true);
								fp_UpdateSend();
							}

							// The shutdown is a write-side cut, so it waits for the close frame to leave
							// the queue; the drain-complete conditions fire it once the queue empties,
							// and the progress timeout bounds the attempt
							if (!Internal.m_nOutgoingQueuedBytes)
							{
								Internal.f_StopTimeout();
								DMibLog(DebugVerbose3, " ++++ {} {} fp_Shutdown 1b {}", fg_ThisActor(this), !Internal.m_bClient);
								fp_Shutdown();
							}
						}
						else if (Internal.m_PendingMessages.f_IsEmpty() && !Internal.m_nOutgoingQueuedBytes && !Internal.m_bCloseFramePending)
						{
							Internal.f_StopTimeout();
							DMibLog(DebugVerbose3, " ++++ {} {} fp_Shutdown 1 {}", fg_ThisActor(this), !Internal.m_bClient);
							fp_Shutdown();
						}
						else
						{
							// The drain depends on the peer keeping its receive window open, so the
							// progress timeout stays armed — a peer that stops reading after its close
							// would otherwise wedge the backlog forever. f_ShutdownDone stops the timer
						}
					}
					else if (WasState == EState_Disconnecting)
					{
						DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 10 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
						Internal.m_State = EState_Disconnected;

						// The peer is owed a close reply, so the parked close frame jumps the queue.
						// The undeliverable messages are dropped first (destructors settle the
						// promises) — the disconnected refill would otherwise frame them behind the
						// close frame, putting data after close on the wire. A close frame between
						// fragments is legal, so a fragmenting message needs no completion
						if (Internal.m_bCloseFramePending)
						{
							Internal.m_PendingMessages.f_Clear();
							Internal.m_pLastPendingMessagesList = nullptr;

							Internal.m_bCloseFramePending = false;
							NContainer::CByteVector Payload = fg_Move(Internal.m_CloseFramePayload);
							Internal.f_SendMessage(EOpcode_ConnectionClose, Payload.f_GetArray(), Payload.f_GetLen(), true);
							fp_UpdateSend();
						}

						// The shutdown is a write-side cut, so it waits for the close reply to leave
						// the queue; the drain-complete conditions fire it once the queue empties,
						// and the progress timeout bounds the attempt
						if (!Internal.m_nOutgoingQueuedBytes)
						{
							Internal.f_StopTimeout();
							DMibLog(DebugVerbose3, " ++++ {} {} fp_Shutdown 2 {}", fg_ThisActor(this), !Internal.m_bClient);
							fp_Shutdown();
						}
					}
					return;
				}
			}
			else if (!_bFatal)
			{
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 11 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
				return;
			}
		}
		else
		{
			if (Internal.m_bClient)
			{
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 12 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
				auto &ConnectionInfo = Internal.m_ConnectionInfo.f_GetAsType<CClientConnectionInfo>();
				if (Internal.m_pSocket)
					ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
				ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;
				ConnectionInfo.m_ErrorStatus = _Status;
				ConnectionInfo.m_Error = _Reason;
				Internal.f_FinishClientConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo));
			}
			else
			{
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 13 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
				auto &ConnectionInfo = Internal.m_ConnectionInfo.f_GetAsType<CConnectionInfo>();
				if (Internal.m_pSocket)
					ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
				ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;
				ConnectionInfo.m_ErrorStatus = _Status;
				ConnectionInfo.m_Error = _Reason;
				Internal.f_FinishConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo));
			}
		}

		if (_bFatal)
		{
			DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 14 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
			Internal.m_CloseInfo.m_Status = _Status;
			Internal.m_CloseInfo.m_Reason = fg_Format("Abnormal closure: {}", _Reason);
			if (Internal.m_ClosePromise)
			{
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 15 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
				Internal.m_ClosePromise->f_SetResult(Internal.m_CloseInfo);
				Internal.m_ClosePromise.f_Clear();
			}
			if (!Internal.m_bOnCloseCalled)
			{
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 16 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
				Internal.m_bOnCloseCalled = true;
				Internal.m_fOnClose.f_CallDiscard(_Status, _Reason, _Origin);
			}

			Internal.m_pSocket.f_Clear();
			Internal.f_ShutdownDone(_Reason);
		}

		DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 17 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
		Internal.m_State = EState_Disconnected;
		Internal.f_StopTimeout();
	}

	void CWebSocketActor::fp_Shutdown()
	{
		try
		{
			auto &Internal = *mp_pInternal;
			if (Internal.m_pSocket && !Internal.m_bShutdownCalled)
			{
				Internal.m_pSocket->f_Shutdown();
				Internal.m_bShutdownCalled = true;
			}
		}
		catch (NCryptography::CExceptionCryptography const &_Error)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket exception: {}", _Error.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
		}
		catch (NNetwork::CExceptionNet const &_Error)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket exception: {}", _Error.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
		}
	}

	void CWebSocketActor::fp_UpdateSend()
	{
		auto &Internal = *mp_pInternal;
		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
			return;

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugNoWriteQueuedMessages)
			return;
#endif

		if (Internal.m_State == EState_Connected)
			Internal.f_WriteQueuedMessages(false);
		else if ((Internal.m_State == EState_Disconnecting || Internal.m_State == EState_Disconnected) && Internal.m_bCloseFramePending)
		{
			// Bounded framing during teardown too — flush-all would materialize the whole
			// backlog into the arena at once. The parked close frame follows the drained
			// queue and fences the stream: data after a close frame is a protocol
			// violation, so later sends stay pending and settle at destroy
			Internal.f_WriteQueuedMessages(false);
			Internal.f_WriteCloseFrameWhenDrained();
		}

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugNoProcessing || Internal.m_bDebugNoProcessingSend)
			return;
#endif

		// Asked per direction: a connection whose receives are submitted but whose sends are not
		// takes the readiness path below, which is the one that leaves write readiness armed to
		// come back for the rest of a short write. The completion branch has no such edge, and
		// with no send operation to submit a stalled write would never be finished
		if (auto *pCompletionIoSend = Internal.f_GetCompletionIoSend())
		{
			// Offer new batches while the socket's gate allows — with an operation in flight
			// included, which is what runs the seal ahead of the wire. Submission order is
			// the ordering story; continuations still go through fp_SendCompleted
			while (Internal.m_pSocket->f_IsValid() && pCompletionIoSend->f_CanSubmitSend() && Internal.m_nOutgoingQueuedBytes > Internal.m_nOutgoingSubmitted)
			{
				umint nBefore = Internal.m_nOutgoingSubmitted;
				fp_SubmitSendOp();
				if (Internal.m_nOutgoingSubmitted == nBefore)
					break;

				if (Internal.m_State == EState_Connected)
					Internal.f_WriteQueuedMessages(false);
				else if ((Internal.m_State == EState_Disconnecting || Internal.m_State == EState_Disconnected) && Internal.m_bCloseFramePending)
				{
					// Same bounded refill as the entry framing; the parked close is the fence
					Internal.f_WriteQueuedMessages(false);
					Internal.f_WriteCloseFrameWhenDrained();
				}
			}

			// The close handshake and the final drain are still this function's to finish: a
			// connection with nothing new queued would otherwise never push its close frame
			// out or let go of the socket
			if (Internal.m_State == EState_Disconnected && !Internal.m_nOutgoingQueuedBytes && !Internal.m_bCloseFramePending)
				fp_Shutdown();

			fp_DrainSocketOutput();

			return;
		}

		bool bDidSend = false;
		while (Internal.m_nOutgoingQueuedBytes && Internal.m_pSocket->f_IsValid())
		{
			NSys::CIoSpan Spans[NNetwork::ICSocket::mc_MaxSendSpans];
			umint nSpans = 0;
			NContainer::TCVector<NStorage::TCSharedPointer<CPayloadOwner>> KeepAlives;
			NStorage::TCSharedPointer<NContainer::CIOByteVector> pArenaCopy;
			umint nGatheredBytes = Internal.f_GatherSendSpans(Spans, nSpans, KeepAlives, pArenaCopy);
			if (!nGatheredBytes)
				break;

			umint SentBytes = 0;
			bool bStuffed = false;
			bool bDisconnected = false;
			NNetwork::CSocketOperationResult CombinedResults;
			try
			{
				bDidSend = true;
				NNetwork::CSocketOperationResult Result = Internal.m_pSocket->f_SendVectored(Spans, nSpans);
				DMibLog(DebugVerbose3, " ++++ {} {} Sending {} resulted in {} sent", fg_ThisActor(this), !Internal.m_bClient, nGatheredBytes, Result.m_nBytes);
#if DMibConfig_IoDebug_Enable
				if (auto *pStats = NNetwork::fg_NetIoStats())
				{
					pStats->m_nSendReadinessCalls.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
					pStats->m_nSendReadinessBytes.f_FetchAdd(Result.m_nBytes, NAtomic::gc_MemoryOrder_Relaxed);
				}
#endif

				CombinedResults += Result;

				SentBytes = Result.m_nBytes;
				if (SentBytes != nGatheredBytes)
					bStuffed = true;
			}
			catch (NCryptography::CExceptionCryptography const &_Error)
			{
				fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket exception: {}", _Error.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
				bDisconnected = true;
			}
			catch (NNetwork::CExceptionNet const &_Error)
			{
				fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket exception: {}", _Error.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
				bDisconnected = true;
			}

			if (CombinedResults.m_bSentNetwork)
				Internal.f_OnSentData();
			if (CombinedResults.m_bReceivedNetwork)
				Internal.f_OnReceivedData();

			Internal.f_ConsumeSentBytes(SentBytes);

			if (bDisconnected)
				break;
			if (bStuffed)
				break;
			if (Internal.m_State == EState_Connected)
				Internal.f_WriteQueuedMessages(false);
			else if ((Internal.m_State == EState_Disconnecting || Internal.m_State == EState_Disconnected) && Internal.m_bCloseFramePending)
			{
				// Same bounded refill as the entry framing; the parked close is the fence —
				// without the emit here, a backlog that drains fully inside this loop would leave
				// the close parked with no writable edge left to deliver it
				Internal.f_WriteQueuedMessages(false);
				Internal.f_WriteCloseFrameWhenDrained();
			}
		}

		if (!bDidSend && Internal.m_pSocket && Internal.m_pSocket->f_IsValid())
		{
			NNetwork::CSocketOperationResult SendResult = Internal.m_pSocket->f_Send(nullptr, 0);
			if (SendResult.m_bSentNetwork)
				Internal.f_OnSentData();
			if (SendResult.m_bReceivedNetwork)
				Internal.f_OnReceivedData();
		}

		if (Internal.m_State == EState_Disconnected && !Internal.m_nOutgoingQueuedBytes && !Internal.m_bCloseFramePending)
		{
			DMibLog(DebugVerbose3, " ++++ {} {} fp_Shutdown 3 {}", fg_ThisActor(this), !Internal.m_bClient);
			fp_Shutdown();
		}
	}

/*
	 0                   1                   2                   3
	 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	+-+-+-+-+-------+-+-------------+-------------------------------+
	|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
	|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
	|N|V|V|V|       |S|             |   (if payload len==126/127)   |
	| |1|2|3|       |K|             |                               |
	+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
	|     Extended payload length continued, if payload len == 127  |
	+ - - - - - - - - - - - - - - - +-------------------------------+
	|                               |Masking-key, if MASK set to 1  |
	+-------------------------------+-------------------------------+
	| Masking-key (continued)       |          Payload Data         |
	+-------------------------------- - - - - - - - - - - - - - - - +
	:                     Payload Data continued ...                :
	+ - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
	|                     Payload Data continued ...                |
	+---------------------------------------------------------------+
 */

	void CWebSocketActor::CInternal::fs_ApplyMask(uint8 *_pData, umint _iDataStart, umint _nBytes, uint8 const *_pMask)
	{
		uint8 DoubleMask[8];
		NMemory::fg_MemCopy(DoubleMask, _pMask, 4);
		NMemory::fg_MemCopy(DoubleMask + 4, _pMask, 4);

		uint8 *pCurrent = _pData;
		uint8 *pEnd = _pData + _nBytes;
		uint8 *pAlignedEnd = fg_AlignDown(pEnd, 4);
		uint8 *pAlignedStart = fg_AlignUp(_pData, 4);
		umint MaskOffset = (_iDataStart + (pAlignedStart - _pData)) % 4;
		pAlignedStart = fg_Min(pAlignedStart, pAlignedEnd);

		for (umint i = _iDataStart % 4; pCurrent < pAlignedStart; pCurrent += 1)
		{
			*pCurrent ^= DoubleMask[i];
			++i;
		}

		uint32 AlignedMask = 0;
		NMemory::fg_MemCopy(&AlignedMask, DoubleMask + MaskOffset, sizeof(uint32));

#ifdef DEnableVector
		uint8 *pAlignedVectorEnd = fg_AlignDown(pEnd, 16);
		uint8 *pAlignedVectorStart = fg_Min(fg_AlignUp(_pData, 16), pAlignedVectorEnd);

		for (; pCurrent < pAlignedVectorStart; pCurrent += 4)
			*((uint32 *)pCurrent) ^= AlignedMask;

		vec4uint32 VectorMask = {AlignedMask, AlignedMask, AlignedMask, AlignedMask};
		for (; pCurrent < pAlignedVectorEnd; pCurrent += 16)
			*((vec4uint32 *)pCurrent) ^= VectorMask;
#endif

		for (; pCurrent < pAlignedEnd; pCurrent += 4)
			*((uint32 *)pCurrent) ^= AlignedMask;

		MaskOffset = (_iDataStart + (pCurrent - _pData)) % 4;
		for (umint i = 0; pCurrent < pEnd; pCurrent += 1)
		{
			*pCurrent ^= DoubleMask[MaskOffset + i];
			++i;
		}
	}

	bool CWebSocketActor::fp_ProcessIncomingMessage()
	{
		auto &Internal = *mp_pInternal;
		DMibLog(DebugVerbose3, " ++++ {} {} fp_ProcessIncomingMessage", fg_ThisActor(this), !Internal.m_bClient);

		auto &Message = Internal.m_NextMessage;
		CHeader &Header = Message.m_Header;
		uint64 &Length = Message.m_Length;
		uint8 *Mask = Message.m_Mask;

		if (!Message.m_bHeaderFinished)
		{
			umint ThisPosition = 0;
			umint &Position = Message.m_Position;
			{
				ThisPosition += 2;
				if (Position < ThisPosition)
				{
					uint8 Data[2];
					if
					(
						!Internal.m_IncomingData.f_Read
						(
							Position
							, 2
							, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
							{
								NMemory::fg_MemCopy((uint8 *)Data + (_iStart - Position), _pData, _nBytes);

								return (_iStart + _nBytes) < (Position + 2);
							}
						)
					)
					{
						return false;
					}

					Header.m_bFinalFragment = (Data[0] >> 7) & uint8(1);
					Header.m_bReserver0 = (Data[0] >> 6) & uint8(1);
					Header.m_bReserver1 = (Data[0] >> 5) & uint8(1);
					Header.m_bReserver2 = (Data[0] >> 4) & uint8(1);
					Header.m_Opcode = Data[0] & uint8(0xF);
					Header.m_bMask = Data[1] >> 7;
					Header.m_PayloadLength = Data[1] & uint8(0x7f);

					Position += 2;
				}
			}

			if (Header.m_bReserver0 || Header.m_bReserver1 || Header.m_bReserver2)
			{
				fp_Disconnect(EWebSocketStatus_ProtocolError, "Reserved bit cannot be set", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			bool bControlMessage = Header.m_Opcode >= EOpcode_ConnectionClose && Header.m_Opcode <= EOpcode_Pong;
			if (bControlMessage && Header.m_PayloadLength >= 126)
			{
				fp_Disconnect(EWebSocketStatus_ProtocolError, "Control frame too big", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			if (Header.m_PayloadLength == 126)
			{
				ThisPosition += 2;
				if (Position < ThisPosition)
				{
					uint16 Data;
					if
					(
						!Internal.m_IncomingData.f_Read
						(
							Position
							, 2
							, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
							{
								NMemory::fg_MemCopy((uint8 *)&Data + (_iStart - Position), _pData, _nBytes);
								return (_iStart + _nBytes) < (Position + 2);
							}
						)
					)
					{
						return false;
					}

					Length = fg_ByteSwapBE(Data);
					Position += 2;
				}
			}
			else if (Header.m_PayloadLength == 127)
			{
				ThisPosition += 8;
				if (Position < ThisPosition)
				{
					uint64 Data;
					if
					(
						!Internal.m_IncomingData.f_Read
						(
							Position
							, 8
							, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
							{
								NMemory::fg_MemCopy((uint8 *)&Data + (_iStart - Position), _pData, _nBytes);
								return (_iStart + _nBytes) < (Position + 8);
							}
						)
					)
					{
						return false;
					}

					Length = fg_ByteSwapBE(Data);
					Position += 8;
				}
			}
			else
				Length = Header.m_PayloadLength;

			if (Length > uint64(Internal.m_Settings.m_MaxMessageSize))
			{
				fp_Disconnect(EWebSocketStatus_MessageTooBig, "Unsupported message length", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			if (Header.m_bMask)
			{
				if (Internal.m_bClient)
				{
					fp_Disconnect(EWebSocketStatus_ProtocolError, "Masked frame received from server", false, EWebSocketCloseOrigin_Local);
					return false;
				}
				ThisPosition += 4;
				if (Position < ThisPosition)
				{
					if
					(
						!Internal.m_IncomingData.f_Read
						(
							Position
							, 4
							, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
							{
								NMemory::fg_MemCopy(Mask + (_iStart - Position), _pData, _nBytes);
								return (_iStart + _nBytes) < (Position + 4);
							}
						)
					)
					{
						return false;
					}

					Position += 4;
				}
			}
			else if (!Internal.m_bClient && Internal.m_bMaskFrames)
			{
				fp_Disconnect(EWebSocketStatus_ProtocolError, "Client sent unmasked frame", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			Internal.m_IncomingData.f_RemoveFront(Position);
			Message.m_bHeaderFinished = true;
		}

		bool bControlMessage = false;
		switch (Header.m_Opcode)
		{
		case EOpcode_ContinuationFrame:
			{
				if (!Internal.m_bPendingMessage)
				{
					fp_Disconnect(EWebSocketStatus_ProtocolError, "Promise frame without start frame", false, EWebSocketCloseOrigin_Local);
					return false;
				}
			}
			break;
		case EOpcode_TextFrame:
		case EOpcode_BinaryFrame:
			{
				if (Internal.m_bPendingMessage)
				{
					fp_Disconnect(EWebSocketStatus_ProtocolError, "Data frame without finishing fragment", false, EWebSocketCloseOrigin_Local);
					return false;
				}
			}
			break;
		case EOpcode_ConnectionClose:
		case EOpcode_Ping:
		case EOpcode_Pong:
			{
				bControlMessage = true;
				if (!Header.m_bFinalFragment)
				{
					fp_Disconnect(EWebSocketStatus_ProtocolError, "Fragmented control frame", false, EWebSocketCloseOrigin_Local);
					return false;
				}
			}
			break;
		default:
			{
				uint8 Opcode = Header.m_Opcode;
				fp_Disconnect(EWebSocketStatus_ProtocolError, NStr::fg_Format("Invalid opcode: {}", Opcode), false, EWebSocketCloseOrigin_Local);
			}
			return false;
		}

		// Control frames carry their own size constraints; data frames are bounded so that an
		// advertised length can never make the direct read path below grow the message buffer
		// by more than one fragment worth of memory
		if (!bControlMessage && Length > uint64(Internal.m_Settings.m_MaxFragmentSize))
		{
			fp_Disconnect(EWebSocketStatus_MessageTooBig, "Frame is bigger than the maximum fragment size", false, EWebSocketCloseOrigin_Local);
			return false;
		}

		umint nBytesAvailable = Internal.m_IncomingData.f_GetLen();

		if (!bControlMessage && Length >= gc_DirectReadThreshold && nBytesAvailable < Length)
		{
			auto &Target = Internal.m_bPendingMessage ? Internal.m_PendingMessage : Message;

			// An unmasked binary frame on a completion stream needs no contiguous landing
			// zone: the receive loop appends the buffer views to the message storage as they
			// arrive, and nothing is copied in user space. Masked frames keep the contiguous
			// path — the mask pass is a write over the whole frame anyway
			bool bBinaryMessage =
				(Internal.m_bPendingMessage ? Target.m_Header.m_Opcode : Header.m_Opcode) == EOpcode_BinaryFrame
			;
			if (bBinaryMessage && !Header.m_bMask && Internal.m_bReceiveStreamShared)
			{
				umint iStart = Target.m_Data.f_GetLen() + umint(Target.m_Storage.f_GetTotalLength());
				if (iStart > Internal.m_Settings.m_MaxMessageSize || Length > uint64(Internal.m_Settings.m_MaxMessageSize - iStart))
				{
					fp_Disconnect(EWebSocketStatus_MessageTooBig, "Unsupported message length", false, EWebSocketCloseOrigin_Local);
					return false;
				}

				// Earlier fragments assembled contiguously go first, then the prefix of this
				// frame that is already buffered in the pages; the views follow in order
				Target.f_FlushDataToStorage();

				if (nBytesAvailable)
				{
					// One copy out of the pages, shared so consumers can still slice views
					// across the prefix; an exact allocation so the block carries no header
					uint8 *pPrefix;
					NContainer::CSharedByteVector Prefix = NContainer::CSharedByteVector::fs_AllocateExact(nBytesAvailable, pPrefix);
					Internal.m_IncomingData.f_ReadFront
						(
							nBytesAvailable
							, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
							{
								NMemory::fg_MemCopy(pPrefix + _iStart, _pData, _nBytes);
								return _iStart + _nBytes < nBytesAvailable;
							}
						)
					;
					Internal.m_IncomingData.f_RemoveFront(nBytesAvailable);
					Target.m_Storage.f_AppendShared(fg_Move(Prefix));
				}

				Internal.m_pDirectReadData = nullptr;
				Internal.m_DirectReadFrameStart = 0;
				Internal.m_nDirectReadRemaining = Length - nBytesAvailable;
				Internal.m_bDirectReadToStorage = true;

				return false; // The receive loop completes the payload
			}

			// Large data frame: copy the payload prefix already buffered in the pages and
			// receive the remainder straight into the message buffer. The limit bounds the
			// whole message, so continuation fragments that landed zero copy in the storage
			// count beside what the data buffer holds; iStart stays the buffer's own fill
			// offset for the copy arithmetic below
			auto &Dest = Target.m_Data;
			umint iStart = Dest.f_GetLen();
			umint nMessageBytes = iStart + umint(Target.m_Storage.f_GetTotalLength());
			if (nMessageBytes > Internal.m_Settings.m_MaxMessageSize || Length > uint64(Internal.m_Settings.m_MaxMessageSize - nMessageBytes))
			{
				fp_Disconnect(EWebSocketStatus_MessageTooBig, "Unsupported message length", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			// Extend to the full frame once so the receive loop can land bytes at the fill
			// offset without touching the length. f_SetLen grows the allocation geometrically
			// on its own, so fragmented reassembly stays amortized without an explicit
			// reserve (f_Reserve allocates exactly, which would recopy on every fragment).
			// A single frame is bounded by m_MaxFragmentSize above, so an advertised length
			// only ever buys one fragment worth of memory ahead of the payload; Windows
			// commits the whole allocation up front, so the frame bound is what keeps a peer
			// that stalls after the header from charging the commit limit
			umint NeededLen = umint(iStart + Length);
			Dest.f_SetLen(NeededLen, false);

			if (nBytesAvailable)
			{
				uint8 *pFrame = Dest.f_GetArray() + iStart;
				Internal.m_IncomingData.f_ReadFront
					(
						nBytesAvailable
						, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
						{
							NMemory::fg_MemCopy(pFrame + _iStart, _pData, _nBytes);
							return _iStart + _nBytes < nBytesAvailable;
						}
					)
				;
				Internal.m_IncomingData.f_RemoveFront(nBytesAvailable);
			}

			Internal.m_pDirectReadData = &Dest;
			Internal.m_DirectReadFrameStart = iStart;
			Internal.m_nDirectReadRemaining = Length - nBytesAvailable;

			return false; // The receive loop completes the payload
		}

		if (nBytesAvailable < Length)
			return false; // Message not finished

		uint8 *pMaskStart;
		if (Internal.m_bPendingMessage && !bControlMessage)
		{
			// The limit bounds the whole message: storage-backed fragments of the pending
			// message count beside its data buffer, while iStart stays the buffer's own
			// length for the mask arithmetic below
			umint iStart = Internal.m_PendingMessage.m_Data.f_GetLen();
			umint nMessageBytes = iStart + umint(Internal.m_PendingMessage.m_Storage.f_GetTotalLength());
			if (nMessageBytes > Internal.m_Settings.m_MaxMessageSize || Length > uint64(Internal.m_Settings.m_MaxMessageSize - nMessageBytes))
			{
				fp_Disconnect(EWebSocketStatus_MessageTooBig, "Unsupported message length", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			Internal.m_IncomingData.f_ReadFront
				(
					Length
					, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
					{
						Internal.m_PendingMessage.m_Data.f_Insert(_pData, _nBytes);
						return _iStart + _nBytes < Length;
					}
				)
			;
			Internal.m_IncomingData.f_RemoveFront(Length);
			pMaskStart = Internal.m_PendingMessage.m_Data.f_GetArray() + iStart;
		}
		else
		{
			Internal.m_IncomingData.f_ReadFront
				(
					Length
					, [&](umint _iStart, uint8 const *_pData, umint _nBytes) -> bool
					{
						Message.m_Data.f_Insert(_pData, _nBytes);
						return _iStart + _nBytes < Length;
					}
				)
			;
			Internal.m_IncomingData.f_RemoveFront(Length);
			pMaskStart = Message.m_Data.f_GetArray();
		}

		if (Header.m_bMask)
			CInternal::fs_ApplyMask(pMaskStart, 0, Length, Mask);

		if (bControlMessage)
		{
			Internal.f_HandleControlMessage(Message);
			Message = CMessage();
			return true;
		}

		if (Header.m_bFinalFragment)
		{
			if (Internal.m_bPendingMessage)
			{
				Internal.f_HandleDataMessage(Internal.m_PendingMessage);
				Internal.m_bPendingMessage = false;
				Internal.m_PendingMessage = CMessage();
			}
			else
				Internal.f_HandleDataMessage(Message);
			Message = CMessage();
		}
		else
		{
			if (!Internal.m_bPendingMessage)
			{
				Internal.m_bPendingMessage = true;
				Internal.m_PendingMessage = fg_Move(Message);
			}
			Message = CMessage();
		}
		return true;
	}

	// Completes a data frame whose payload was received straight into the message buffer:
	// unmasks the frame in place and runs the same dispatch and fragment bookkeeping as the
	// buffered path
	void CWebSocketActor::CInternal::f_FinishDirectReadFrame()
	{
		auto &Message = m_NextMessage;
		CHeader &Header = Message.m_Header;

		// A storage direct read is unmasked by construction, so only the contiguous form
		// owes the mask pass
		if (Header.m_bMask)
			fs_ApplyMask(m_pDirectReadData->f_GetArray() + m_DirectReadFrameStart, 0, Message.m_Length, Message.m_Mask);

		m_pDirectReadData = nullptr;
		m_DirectReadFrameStart = 0;
		m_bDirectReadToStorage = false;

		if (Header.m_bFinalFragment)
		{
			if (m_bPendingMessage)
			{
				f_HandleDataMessage(m_PendingMessage);
				m_bPendingMessage = false;
				m_PendingMessage = CMessage();
			}
			else
				f_HandleDataMessage(Message);
			Message = CMessage();
		}
		else
		{
			if (!m_bPendingMessage)
			{
				m_bPendingMessage = true;
				m_PendingMessage = fg_Move(Message);
			}
			Message = CMessage();
		}
	}

	void CWebSocketActor::CInternal::f_HandleControlMessage(CMessage &_Message)
	{
		// RFC 6455 - 5.5
		switch (_Message.m_Header.m_Opcode)
		{
		case EOpcode_ConnectionClose:
			{
				// RFC 6455 - 5.5.1.
				EWebSocketStatus Status = EWebSocketStatus_NoStatusReceived;
				NStr::CStr Reason;
				if (_Message.m_Data.f_GetLen() >= 2)
				{
					NStream::CBinaryStreamMemoryPtr<NStream::CBinaryStreamBigEndian> Stream;
					Stream.f_OpenRead(_Message.m_Data);

					uint16 ErrorCode;
					Stream >> ErrorCode;
					Status = (EWebSocketStatus)ErrorCode;

					umint Len = Stream.f_GetLength() - 2;
					ch8 *pData = Reason.f_GetStr(Len);
					if (Len != 0)
						Stream.f_ConsumeBytes(pData, Len);
					pData[Len] = 0;
					Reason.f_SetStrLen(Len);

					if (!NStr::fg_IsValidUTF8(Reason.f_GetStr(), Reason.f_GetLen()))
					{
						m_pThis->fp_Disconnect(EWebSocketStatus_InvalidFramePayloadData, NStr::gc_Str<"Invalid UTF-8">, false, EWebSocketCloseOrigin_Local);
						break;
					}

					if (!fs_IsValidCloseStatus(Status))
					{
						m_pThis->fp_Disconnect(EWebSocketStatus_ProtocolError, NStr::gc_Str<"Invalid Close Code">, false, EWebSocketCloseOrigin_Local);
						break;
					}
				}

				// TODO: Send reponse frame
				DMibLog(DebugVerbose3, " ++++ {} {} Handle EOpcode_ConnectionClose {}", fg_ThisActor(m_pThis), !m_bClient, Reason);
				m_pThis->fp_Disconnect(Status, Reason, false, EWebSocketCloseOrigin_Remote);
			}
			break;
		case EOpcode_Ping:
			{
				// RFC 6455 - 5.5.2.
				if (m_fOnReceivePing.f_IsEmpty() || (m_pTimeoutPingMessage && _Message.m_Data == *m_pTimeoutPingMessage))
				{
					// We reply automatically as quickly as possible
					f_SendMessage(EOpcode_Pong, _Message.m_Data.f_GetArray(), _Message.m_Data.f_GetLen(), true);
					m_pThis->fp_UpdateSend();
				}
				else
				{
					// Otherwise we let the application reply
					m_fOnReceivePing.f_CallDiscard(fg_Construct(fg_Move(_Message.m_Data)));
				}
			}
			break;
		case EOpcode_Pong:
			{
				// RFC 6455 - 5.5.3.
				if (m_pTimeoutPingMessage && _Message.m_Data == *m_pTimeoutPingMessage)
					f_OnTimeoutPongReceived();
				else if (!m_fOnReceivePong.f_IsEmpty())
					m_fOnReceivePong.f_CallDiscard(fg_Construct(fg_Move(_Message.m_Data)));
			}
			break;
		default:
			{
				DMibNeverGetHere;
			}
			break;
		}
	}

	void CWebSocketActor::CInternal::f_HandleDataMessage(CMessage &_Message)
	{
		DMibLog(DebugVerbose3, " ++++ {} {} f_HandleDataMessage", fg_ThisActor(m_pThis), !m_bClient);
		switch (_Message.m_Header.m_Opcode)
		{
		case EOpcode_TextFrame:
			{
				if (!NStr::fg_IsValidUTF8((ch8 const *)_Message.m_Data.f_GetArray(), _Message.m_Data.f_GetLen()))
				{
					m_pThis->fp_Disconnect(EWebSocketStatus_InvalidFramePayloadData, NStr::gc_Str<"Invalid UTF-8">, false, EWebSocketCloseOrigin_Local);
					break;
				}

				NStr::CStr Data(NStr::CAllowNUL(), (ch8 const *)_Message.m_Data.f_GetArray(), _Message.m_Data.f_GetLen());
				DMibLog(DebugVerbose3, " ++++ {} {} call m_OnReceiveTextMessage", fg_ThisActor(m_pThis), !m_bClient);

				if (m_fOnReceiveTextMessage.f_ShouldCall())
					m_fOnReceiveTextMessage.f_CallDiscard(fg_Move(Data));
			}
			break;
		case EOpcode_BinaryFrame:
			{
				DMibLog(DebugVerbose3, " ++++ {} {} call m_OnReceiveBinaryMessage", fg_ThisActor(m_pThis), !m_bClient);
				if (m_fOnReceiveBinaryMessage.f_ShouldCall())
				{
					// Contiguous assembly moves in as the trailing segment; a message that
					// never saw a view append becomes a single moved vector segment
					_Message.f_FlushDataToStorage();

					NStorage::TCSharedPointer<NStream::CBinaryStorage> pStorage = fg_Construct(fg_Move(_Message.m_Storage));
					m_fOnReceiveBinaryMessage.f_CallDiscard(pStorage.f_ShareAsConst());
				}
			}
			break;
		default:
			{
				DMibNeverGetHere;
			}
			break;
		}
	}

	void CWebSocketActor::fp_ProcessIncoming()
	{
		auto &Internal = *mp_pInternal;
		bool bMoreWork = true;
		while (bMoreWork && !Internal.m_IncomingData.f_IsEmpty())
		{
			bMoreWork = false;
			switch (Internal.m_State)
			{
			case EState_Connected:
			case EState_Disconnecting:
				{
					if (fp_ProcessIncomingMessage())
						bMoreWork = true;
				}
				break;
			case EState_Disconnected:
				{
					// Just drop everything that comes in
					Internal.m_IncomingData.f_RemoveFront(Internal.m_IncomingData.f_GetLen());
				}
				break;
			case EState_HeaderReceived:
				{
					fp_Disconnect(EWebSocketStatus_ProtocolError, "Data received before handshake response was sent", true, EWebSocketCloseOrigin_Local);
				}
				break;
			case EState_None:
				{
					if (Internal.m_bClient)
					{
						auto &ConnectionInfo = Internal.m_ConnectionInfo.f_GetAsType<CClientConnectionInfo>();
						switch (ConnectionInfo.m_pResponse->f_Parse(Internal.m_IncomingData))
						{
						case NHTTP::EResponseStatus_Complete:
							{

								auto &EntityFields = ConnectionInfo.m_pResponse->f_GetEntityFields();
								auto &GeneralFields = ConnectionInfo.m_pResponse->f_GetGeneralFields();

								auto &StatusLine = ConnectionInfo.m_pResponse->f_GetStatusLine();

								if (StatusLine.f_GetStatus() != NHTTP::EStatus_SwitchingProtocols)
								{
									fp_RejectClientConnection(fg_Format("Status was not set to 101 Switching Protocols, but rather: {} {}", StatusLine.f_GetStatus(), StatusLine.f_GetReasonPhrase()));
									break;
								}

								if (GeneralFields.f_GetUpgrade().f_CmpNoCase("websocket") != 0)
								{
									fp_RejectClientConnection("Upgrade was not set to 'websocket'");
									break;
								}

								if (GeneralFields.f_GetConnection() != NHTTP::EConnectionToken_Upgrade)
								{
									fp_RejectClientConnection("Connection was not set to 'Upgrade'");
									break;
								}

								{
									auto pAccept = EntityFields.f_GetUnknownField("Sec-WebSocket-Accept");
									if (!pAccept)
									{
										fp_RejectClientConnection("Sec-WebSocket-Accept missing");
										break;
									}


									NCryptography::CHash_SHA1 Hash;
									Hash.f_AddData(Internal.m_ClientConnectionInput.m_EncodedKey.f_GetStr(), Internal.m_ClientConnectionInput.m_EncodedKey.f_GetLen());
									Hash.f_AddData("258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);

									NCryptography::CHash_SHA1::CMessageDigest Digest = Hash;

									NContainer::CByteVector DigestData;
									DigestData.f_Insert(Digest.f_GetData(), Digest.mc_Size);

									auto CorrectKey = NEncoding::fg_Base64Encode(DigestData);
									if (CorrectKey != *pAccept)
									{
										fp_RejectClientConnection("Invalid Sec-WebSocket-Accept key");
										break;
									}
								}

								{
									auto pProtocol = EntityFields.f_GetUnknownField("Sec-WebSocket-Protocol");
									if (pProtocol)
									{
										if (!Internal.m_ClientConnectionInput.m_Protocols.f_IsEmpty())
										{
											if (!pProtocol || !Internal.m_ClientConnectionInput.m_Protocols.f_FindEqual(*pProtocol))
											{
												fp_RejectClientConnection("Server didn't return any Sec-WebSocket-Protocol the client asked for");
												break;
											}
										}
										ConnectionInfo.m_Protocol = *pProtocol;
									}
								}

								// Masking stops only when the server answered with the extension
								// this client offered. No answer means it is still on
								if (Internal.m_Settings.m_bNegotiateUnmaskedFrames)
								{
									auto pExtensions = EntityFields.f_GetUnknownField("Sec-WebSocket-Extensions");
									if (pExtensions && fg_ContainsExtension(*pExtensions, gc_pUnmaskedFramesExtension))
										Internal.m_bMaskFrames = false;
								}
								if (Internal.m_pSocket)
									ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
								ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;
								Internal.m_State = EState_Connected;

								// The first receive is armed by the enclosing readiness loop once
								// it has drained what is already buffered, not here in its middle
								fp_TryActivateCompletionIo(false);

								Internal.f_FinishClientConnection(EFinishConnectionResult_Success, fg_Move(ConnectionInfo));
								bMoreWork = true;

							}
							break;
						case NHTTP::EResponseStatus_Invalid:
							{
								fp_Disconnect(EWebSocketStatus_ProtocolError, "Invalid HTTP request header", true, EWebSocketCloseOrigin_Local);
							}
							return;
						case NHTTP::EResponseStatus_InProgress:
							break;
						default:
							{
								bMoreWork = false;
							}
							break;
						}
					}
					else
					{
						auto &ConnectionInfo = Internal.m_ConnectionInfo.f_GetAsType<CConnectionInfo>();
						switch (ConnectionInfo.m_pRequest->f_Parse(Internal.m_IncomingData))
						{
						case NHTTP::ERequestStatus_Complete:
							{
								auto &EntityFields = ConnectionInfo.m_pRequest->f_GetEntityFields();
								auto &RequestLine = ConnectionInfo.m_pRequest->f_GetRequestLine();
								Internal.m_State = EState_HeaderReceived;

								if (RequestLine.f_GetMethod() != NHTTP::EMethod_Get)
								{
									fp_RejectServerConnection(NStr::fg_Format("Unsupported HTTP method: {}. Only GET is supported", fg_HTTP_GetMethodName(RequestLine.f_GetMethod())));
									break;
								}

								NHTTP::CURL const &URI = RequestLine.f_GetURI();
								auto &Paths = URI.f_GetPath();
								if (Paths.f_GetLen() == 2 && Paths[0] == "sockjs" && Paths[1] == "info")
								{
									NEncoding::CJsonSorted Reply;
									Reply["websocket"] = true;
									Reply["origins"].f_Array().f_Insert("*:*");
									Reply["cookie_needed"] = false;
									Reply["entropy"] = NMisc::fg_GetSecureRandomUnsigned();
									NStr::CStr ReplyText = Reply.f_ToString(nullptr);

									NHTTP::CResponseHeader ResponseHeader;

									ResponseHeader.f_SetStatus(NHTTP::EStatus_OK);
									ResponseHeader.f_GetEntityFields().f_SetUnknownField("access-control-allow-origin", "*");
									ResponseHeader.f_GetGeneralFields().f_SetCacheControl("no-store, no-cache, must-revalidate, max-age=0");
									ResponseHeader.f_GetGeneralFields().f_SetConnection(NHTTP::EConnectionToken_KeepAlive);
									ResponseHeader.f_GetEntityFields().f_SetContentType("application/json; charset=UTF-8");
									ResponseHeader.f_GetGeneralFields().f_SetDate(NTime::CTime::fs_NowUTC());
									ResponseHeader.f_GetResponseFields().f_SetVary("Origin");

									fp_RejectServerConnection("Replied to SockJS info request", fg_Move(ResponseHeader), ReplyText);
									break;
								}

								auto *pKey = EntityFields.f_GetUnknownField("Sec-WebSocket-Key");
								if (!pKey)
								{
									fp_RejectServerConnection("Sec-WebSocket-Key missing");
									break;
								}
								Internal.m_Key = *pKey;

								auto *pVersion = EntityFields.f_GetUnknownField("Sec-WebSocket-Version");
								if (!pVersion)
								{
									fp_RejectServerConnection("Sec-WebSocket-Version missing");
									break;
								}
								Internal.m_Version = *pVersion;

								if (Internal.m_Version.f_ToInt(0) != 13)
								{
									NHTTP::CResponseHeader Header;
									Header.f_GetEntityFields().f_SetUnknownField("Sec-WebSocket-Version", "13");
									fp_RejectServerConnection(NStr::fg_Format("Unsupported WebSocket version: {}", Internal.m_Version), fg_Move(Header));
									break;
								}

								{
									auto *pExtensions = EntityFields.f_GetUnknownField("Sec-WebSocket-Extensions");
									Internal.m_bPeerOfferedUnmasked = pExtensions && fg_ContainsExtension(*pExtensions, gc_pUnmaskedFramesExtension);
								}

								auto *pProtocol = EntityFields.f_GetUnknownField("Sec-WebSocket-Protocol");
								if (pProtocol)
								{
									NStr::CStr ToParse = *pProtocol;
									while (!ToParse.f_IsEmpty())
									{
										NStr::CStr Protocol = fg_GetStrSep(ToParse, ",");
										ConnectionInfo.m_Protocols.f_Insert(Protocol);
									}
								}

								ConnectionInfo.m_ID = *pKey;
								ConnectionInfo.m_ProtocolVersion = *pVersion;
								if (Internal.m_pSocket)
									ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
								ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;

								Internal.f_FinishConnection(EFinishConnectionResult_Success, fg_Move(ConnectionInfo));
								bMoreWork = true;
							}
							break;
						case NHTTP::ERequestStatus_InProgress:
							break;
						case NHTTP::ERequestStatus_Invalid:
							{
								fp_Disconnect(EWebSocketStatus_ProtocolError, "Invalid HTTP request header", true, EWebSocketCloseOrigin_Local);
							}
							return;
						default:
							{
								bMoreWork = false;
							}
							break;
						}
					}
				}
				break;
			}
		}
	}

	void CWebSocketActor::CInternal::f_FinishClientConnection(EFinishConnectionResult _Result, CClientConnectionInfo &&_ConnectionInfo)
	{
		if (m_bFinishCalled)
			return;

		m_bFinishCalled = true;

		auto Cleanup = g_OnScopeExit / [this, WeakActor = fg_ThisActor(m_pThis).f_Weak()]
			{
				auto Actor = WeakActor.f_Lock();
				if (Actor)
				{
					NConcurrency::g_Dispatch(Actor) / [this]
						{
							fg_Move(m_fOnFinishClientConnection).f_Destroy().f_DiscardResult();
						}
						> NConcurrency::g_DiscardResult
					;
				}
			}
		;

		m_fOnFinishClientConnection(_Result, fg_Move(_ConnectionInfo)).f_OnResultSet
			(
				[Cleanup = fg_Move(Cleanup)](NConcurrency::TCAsyncResult<void> &&)
				{
				}
			)
		;
	}

	void CWebSocketActor::CInternal::f_FinishConnection(EFinishConnectionResult _Result, CConnectionInfo &&_ConnectionInfo)
	{
		if (m_bFinishCalled)
			return;

		m_bFinishCalled = true;

		auto Cleanup = g_OnScopeExit / [this, WeakActor = fg_ThisActor(m_pThis).f_Weak()]
			{
				auto Actor = WeakActor.f_Lock();
				if (Actor)
				{
					NConcurrency::g_Dispatch(Actor) / [this]
						{
							fg_Move(m_fOnFinishConnection).f_Destroy().f_DiscardResult();
						}
						> NConcurrency::g_DiscardResult
					;
				}
			}
		;

		m_fOnFinishConnection(_Result, fg_Move(_ConnectionInfo)).f_OnResultSet
			(
				[Cleanup = fg_Move(Cleanup)](NConcurrency::TCAsyncResult<void> &&)
				{
				}
			)
		;
	}

	void CWebSocketActor::fp_TryStopDeferring()
	{
		auto &Internal = *mp_pInternal;
		if (!Internal.m_bOnFinishDone)
		{
			Internal.m_bWantStopDefer = true;
			return;
		}
		fp_StopDeferring();
	}

	void CWebSocketActor::fp_StopDeferring()
	{
		auto &Internal = *mp_pInternal;
		Internal.m_fOnReceiveBinaryMessage.f_StopDeferring();
		Internal.m_fOnReceiveTextMessage.f_StopDeferring();
		Internal.m_fOnReceivePing.f_StopDeferring();
		Internal.m_fOnReceivePong.f_StopDeferring();
		Internal.m_fOnClose.f_StopDeferring();
		Internal.m_fOnFinishConnection.f_StopDeferring();
		Internal.m_fOnFinishClientConnection.f_StopDeferring();
	}

	NConcurrency::CActorSubscription CWebSocketActor::fp_AcceptClientConnection(CCallbacks _Callbacks)
	{
		auto &Internal = *mp_pInternal;

		auto Subscription = Internal.f_SetCallbacks(fg_Move(_Callbacks));
		fp_TryStopDeferring();

		return Subscription;
	}

	void CWebSocketActor::fp_RejectClientConnection(NStr::CStr _Error)
	{
		fp_TryStopDeferring();

		fp_Disconnect(EWebSocketStatus_Rejected, NStr::fg_Format("Rejected connection: {}", _Error), false, EWebSocketCloseOrigin_Local);
	}

	NConcurrency::CActorSubscription CWebSocketActor::CInternal::f_SetCallbacks(CCallbacks &&_Callbacks)
	{
		m_fOnReceiveBinaryMessage.f_SetCallback(fg_Move(_Callbacks.m_fOnReceiveBinaryMessage));
		m_fOnReceiveTextMessage.f_SetCallback(fg_Move(_Callbacks.m_fOnReceiveTextMessage));
		m_fOnReceivePing.f_SetCallback(fg_Move(_Callbacks.m_fOnReceivePing));
		m_fOnReceivePong.f_SetCallback(fg_Move(_Callbacks.m_fOnReceivePong));
		m_fOnClose.f_SetCallback(fg_Move(_Callbacks.m_fOnClose));

		return NConcurrency::g_ActorSubscription / [this]() -> NConcurrency::TCFuture<void>
			{
				NConcurrency::TCFutureVector<void> DestroyResults;

				fg_Move(m_fOnReceiveBinaryMessage).f_Destroy() > DestroyResults;
				fg_Move(m_fOnReceiveTextMessage).f_Destroy() > DestroyResults;
				fg_Move(m_fOnReceivePing).f_Destroy() > DestroyResults;
				fg_Move(m_fOnReceivePong).f_Destroy() > DestroyResults;
				fg_Move(m_fOnClose).f_Destroy() > DestroyResults;

				co_await fg_AllDoneWrapped(DestroyResults);

				co_return {};
			}
		;
	}

	NConcurrency::CActorSubscription CWebSocketActor::fp_AcceptServerConnection(NStr::CStr _Protocol, NHTTP::CResponseHeader _ResponseHeader, CCallbacks _Callbacks)
	{
		auto &Internal = *mp_pInternal;
		auto Subscription = Internal.f_SetCallbacks(fg_Move(_Callbacks));
		fp_TryStopDeferring();

		NHTTP::CResponseHeader Response = fg_Move(_ResponseHeader);

		Response.f_SetOutputMethod
			(
				[&](uint8 const *_pData, umint _nBytes) noexcept
				{
					Internal.m_OutgoingData.f_InsertBack(_pData, _nBytes);
					Internal.f_TrackArenaBytes(_nBytes);
				}
			)
		;

		Response.f_SetStatus(NHTTP::EStatus_SwitchingProtocols);

		auto &GeneralFields = Response.f_GetGeneralFields();
		auto &EntityFields = Response.f_GetEntityFields();

		GeneralFields.f_SetUpgrade("websocket");
		GeneralFields.f_SetConnection(NHTTP::EConnectionToken_Upgrade);
		if (!_Protocol.f_IsEmpty())
			EntityFields.f_SetUnknownField("Sec-WebSocket-Protocol", _Protocol);

		// Echoed only when this server was configured for it and the client asked, so both ends
		// have agreed before either stops masking
		if (Internal.m_Settings.m_bNegotiateUnmaskedFrames && Internal.m_bPeerOfferedUnmasked)
		{
			EntityFields.f_SetUnknownField("Sec-WebSocket-Extensions", gc_pUnmaskedFramesExtension);
			Internal.m_bMaskFrames = false;
		}

		NCryptography::CHash_SHA1 Hash;
		Hash.f_AddData(Internal.m_Key.f_GetStr(), Internal.m_Key.f_GetLen());
		Hash.f_AddData("258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
		NCryptography::CHash_SHA1::CMessageDigest Digest = Hash;

		NContainer::CByteVector DigestData;
		DigestData.f_Insert(Digest.f_GetData(), Digest.mc_Size);

		EntityFields.f_SetUnknownField("Sec-WebSocket-Accept", NEncoding::fg_Base64Encode(DigestData));

		Response.f_Complete();

		Internal.m_State = EState_Connected;

		fp_TryActivateCompletionIo(true);

		fp_UpdateSend();

		return Subscription;
	}

	void CWebSocketActor::fp_RejectServerConnection(NStr::CStr _Error, NHTTP::CResponseHeader _ResponseHeader, NStr::CStr _Content)
	{
		auto &Internal = *mp_pInternal;
		fp_TryStopDeferring();

		if (Internal.m_State != EState_HeaderReceived)
		{
			fp_Disconnect(EWebSocketStatus_InternalError, "Reject connection in wrong state", true, EWebSocketCloseOrigin_Local);
			return;
		}

		NHTTP::CResponseHeader Response = fg_Move(_ResponseHeader);

		Response.f_SetOutputMethod
			(
				[&](uint8 const *_pData, umint _nBytes) noexcept
				{
					Internal.m_OutgoingData.f_InsertBack(_pData, _nBytes);
					Internal.f_TrackArenaBytes(_nBytes);
				}
			)
		;

		auto& StatusLine = Response.f_GetStatusLine();

		// RFC 6455 - 4.2.1.
		if (StatusLine.f_GetStatus() == NHTTP::EStatus_Unknown)
			Response.f_SetStatus(NHTTP::EStatus_BadRequest, _Error);

		auto Content = Response.f_Complete();

		if (!_Content.f_IsEmpty())
			Content.f_SendString(_Content);
		else
			Content.f_SendString(_Error);
		fp_UpdateSend();
		fp_Disconnect(EWebSocketStatus_Rejected, NStr::fg_Format("Rejected connection: {}", _Error), false, EWebSocketCloseOrigin_Local);
	}

	void CWebSocketActor::fp_ProcessState(NNetwork::ENetTCPState _StateAdded)
	{
		auto &Internal = *mp_pInternal;

		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid() || f_IsDestroyed())
			return;

		if
		(
			Internal.m_bReceiveStreamActive && !Internal.m_bReceiveStreamEnded
			&& (_StateAdded & (NNetwork::ENetTCPState_Closed | NNetwork::ENetTCPState_RemoteClosed))
		)
		{
			// The stream can still hold buffered data with the peer's close frame in it, and
			// delivery order between it and the poll's close events is not defined. The close
			// states wait for the stream's terminal, the completion mode form of the drain-first
			// rule below — and the stream always terminates once the peer is gone
			Internal.m_DeferredCloseStates = Internal.m_DeferredCloseStates | (_StateAdded & (NNetwork::ENetTCPState_Closed | NNetwork::ENetTCPState_RemoteClosed));
			_StateAdded = _StateAdded & ~(NNetwork::ENetTCPState_Closed | NNetwork::ENetTCPState_RemoteClosed);

			if (!_StateAdded)
				return;
		}

		if (_StateAdded & NNetwork::ENetTCPState_Closed)
		{
			DMibLog(DebugVerbose3, " ++++ {} {} ENetTCPState_Closed", fg_ThisActor(this), !Internal.m_bClient);

			if (Internal.m_State == EState_Disconnecting)
			{
				// The hangup often lands in the same poller batch as the close frame this side is
				// waiting for, and which event is reported first is not ordered. Honoring the close
				// now would drop that frame, so the readable data is drained first; if it holds the
				// close frame, the drain completes the handshake and the close below finds the
				// disconnect already done
				_StateAdded |= NNetwork::ENetTCPState_Read;
			}
			else
			{
				if (Internal.m_State != EState_Disconnected)
					fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason()), true, EWebSocketCloseOrigin_Remote);
				else
				{
					Internal.m_pSocket.f_Clear();
					Internal.f_ShutdownDone(NStr::CStr());
				}
				return;
			}
		}

		if
		(
			(_StateAdded & NNetwork::ENetTCPState_Read)
			&& Internal.f_GetCompletionIoReceive()
#if DMibConfig_Tests_Enable
			&& !(Internal.m_bDebugNoProcessing || Internal.m_bDebugNoProcessingReceive)
#endif
		)
		{
			// Readiness reads must never overlap a submitted operation; a stray readiness
			// edge here at most makes sure one is armed. Banked bytes flush first so the
			// debug re-drive delivers in order, and a banked frame that reached zero
			// remaining has to finish before anything else is parsed. The storage-direct
			// form keeps no destination pointer, so its marker is the flag
			if ((Internal.m_pDirectReadData || Internal.m_bDirectReadToStorage) && !Internal.m_nDirectReadRemaining)
				Internal.f_FinishDirectReadFrame();

			if (!Internal.m_IncomingData.f_IsEmpty())
				fp_ProcessIncoming();

			fp_StartReceiveStream();
		}
		else if
		(
			(_StateAdded & NNetwork::ENetTCPState_Read)
#if DMibConfig_Tests_Enable
			&& !(Internal.m_bDebugNoProcessing || Internal.m_bDebugNoProcessingReceive)
#endif
		)
		{
			DMibLog(DebugVerbose3, " ++++ {} {} ENetTCPState_Read", fg_ThisActor(this), !Internal.m_bClient);

			NNetwork::CSocketOperationResult CombinedResults;
			try
			{
				while (true)
				{
					if (Internal.m_nDirectReadRemaining)
					{
						// A large data frame's payload is received straight into the message
						// buffer at its fill offset; never past the payload, so the next
						// frame's header lands in the incoming pages
						auto &Dest = *Internal.m_pDirectReadData;
						umint FrameLength = (umint)Internal.m_NextMessage.m_Length;
						umint nRemaining = (umint)Internal.m_nDirectReadRemaining;
						umint FillOffset = Internal.m_DirectReadFrameStart + FrameLength - nRemaining;
						NNetwork::CSocketOperationResult Result = Internal.m_pSocket->f_Receive(Dest.f_GetArray() + FillOffset, nRemaining);
						CombinedResults += Result;
#if DMibConfig_IoDebug_Enable
						if (auto *pStats = NNetwork::fg_NetIoStats())
						{
							pStats->m_nRecvReadinessCalls.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
							pStats->m_nRecvReadinessBytes.f_FetchAdd(Result.m_nBytes, NAtomic::gc_MemoryOrder_Relaxed);
						}
#endif
						if (Result.m_nBytes == 0 && !Result.m_bSentNetwork && !Result.m_bReceivedNetwork)
							break;
						DMibLog(DebugVerbose3, " ++++ {} {} Received direct data {}", fg_ThisActor(this), !Internal.m_bClient, Result.m_nBytes);
						Internal.m_nReceivedBytes += Result.m_nBytes;
						Internal.m_nDirectReadRemaining -= Result.m_nBytes;

						if (Internal.m_nDirectReadRemaining)
							continue;

						Internal.f_FinishDirectReadFrame();
						if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
							return;

						fp_UpdateSend();
						if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
							return;

						continue;
					}

					// Bounce through a stack buffer so receive calls stay large: the page tail can
					// never hold more than one page (2 KiB), so receiving directly into it would
					// shrink every receive syscall by 8x
					NNetwork::CSocketOperationResult Result;
					{
						uint8 Bounce[gc_ReceiveChunkSize];
						Result = Internal.m_pSocket->f_Receive(Bounce, gc_ReceiveChunkSize);
						CombinedResults += Result;
#if DMibConfig_IoDebug_Enable
						if (auto *pStats = NNetwork::fg_NetIoStats())
						{
							pStats->m_nRecvReadinessCalls.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
							pStats->m_nRecvReadinessBytes.f_FetchAdd(Result.m_nBytes, NAtomic::gc_MemoryOrder_Relaxed);
						}
#endif
						if (Result.m_nBytes == 0 && !Result.m_bSentNetwork && !Result.m_bReceivedNetwork)
							break;
						Internal.m_IncomingData.f_InsertBack(Bounce, Result.m_nBytes);
					}
					DMibLog(DebugVerbose3, " ++++ {} {} Received data {}", fg_ThisActor(this), !Internal.m_bClient, Result.m_nBytes);
					Internal.m_nReceivedBytes += Result.m_nBytes;

					fp_ProcessIncoming();
					if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
						return;

					fp_UpdateSend();
					if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
						return;

					// The connection can switch to completion transfers while processing (the
					// client upgrade completes inside fp_ProcessIncoming), and readiness receives
					// must not overlap a submitted operation, so the loop hands over here
					if (Internal.f_GetCompletionIoReceive())
					{
						fp_StartReceiveStream();
						break;
					}
				}
			}
			catch (NCryptography::CExceptionCryptography const& _Exception)
			{
				fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket error: {}", _Exception.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
				return;
			}
			catch (NNetwork::CExceptionNet const& _Exception)
			{
				fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket error: {}", _Exception.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
				return;
			}
			if (CombinedResults.m_bReceivedNetwork)
				Internal.f_OnReceivedData();
			if (CombinedResults.m_bSentNetwork)
				Internal.f_OnSentData();
		}

		if (_StateAdded & NNetwork::ENetTCPState_Closed)
		{
			// Only reached on the drain-first path above; a drain that found the close frame has
			// completed the disconnect, so this close is then just the socket going away
			if (Internal.m_State != EState_Disconnected)
				fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason()), true, EWebSocketCloseOrigin_Remote);
			else
			{
				Internal.m_pSocket.f_Clear();
				Internal.f_ShutdownDone(NStr::CStr());
			}
			return;
		}

		if (_StateAdded & NNetwork::ENetTCPState_RemoteClosed)
		{
			if (Internal.m_State <= EState_Connected)
			{
				if (Internal.m_State == EState_Connected)
				{
					DMibLog(DebugVerbose3, " ++++ {} {} ENetTCPState_RemoteClosed 1", fg_ThisActor(this), !Internal.m_bClient);
					fp_Disconnect(Internal.m_CloseInfo.m_Status == EWebSocketStatus_None ? EWebSocketStatus_AbnormalClosure : EWebSocketStatus_NormalClosure, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason()), false, EWebSocketCloseOrigin_Remote, true);
				}
				else
				{
					DMibLog(DebugVerbose3, " ++++ {} {} ENetTCPState_RemoteClosed 2", fg_ThisActor(this), !Internal.m_bClient);
					fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason()), true, EWebSocketCloseOrigin_Remote);
				}
			}
			else if (Internal.m_State == EState_Disconnecting)
			{
				DMibLog(DebugVerbose3, " ++++ {} {} ENetTCPState_RemoteClosed 3 {}", fg_ThisActor(this), !Internal.m_bClient, Internal.m_State);
				fp_Disconnect
					(
						EWebSocketStatus_AbnormalClosure
						, NStr::fg_Format("No close frame received while disconnecting. Socket closed: {}", Internal.m_pSocket->f_GetCloseReason())
						, false
						, EWebSocketCloseOrigin_Remote
						, true
					)
				;
			}
			else
				DMibLog(DebugVerbose3, " ++++ {} {} ENetTCPState_RemoteClosed 4 {}", fg_ThisActor(this), !Internal.m_bClient, Internal.m_State);
		}

		if (_StateAdded & NNetwork::ENetTCPState_Write)
		{
			DMibLog(DebugVerbose3, " ++++ {} {} ENetTCPState_Write", fg_ThisActor(this), !Internal.m_bClient);
			fp_UpdateSend();
		}
	}

	void CWebSocketActor::fp_SetSocket(NStorage::TCUniquePointer<NNetwork::ICSocket> _pSocket)
	{
		auto &Internal = *mp_pInternal;

		DMibFastCheck(!Internal.m_pSocket);
		Internal.m_pSocket = fg_Move(_pSocket);

		// Belongs to the socket that is going away, and completion transfers are decided again
		// for the new one
		Internal.m_pCompletionIo = nullptr;

		// A transport that buffers on the way through sizes that buffering from what one transfer
		// is worth here, which is the fragmentation size the frames are cut to
		Internal.m_pSocket->f_SetTransferSizeHint(fg_Max(Internal.m_Settings.m_FragmentationSize, umint(4096)) + NNetwork::gc_SocketFramingMargin);

		// And how much of it may be in flight at once
		Internal.m_pSocket->f_SetSendWindow(Internal.m_Settings.f_GetSendWindowBytes(), Internal.m_Settings.m_SendWindowBytes != 0);

		NNetwork::ENetTCPState State = NNetwork::ENetTCPState_None;

		if (Internal.m_pSocket->f_IsValid())
		{
			try
			{
				NException::CDisableExceptionTraceScope DisableTrace;
				Internal.m_PeerAddress = Internal.m_pSocket->f_GetPeerAddress();
			}
			catch (NCryptography::CExceptionCryptography const &)
			{
			}
			catch (NNetwork::CExceptionNet const &)
			{
			}

			State = Internal.m_pSocket->f_GetState();
		}

		fp_ProcessState(State);
	}

	NConcurrency::CActorSubscription CWebSocketActor::fp_OnFinishServerConnection
		(
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EFinishConnectionResult _Result, CConnectionInfo _ConnectionInfo)> _fOnFinishConnection
		)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_fOnFinishConnection.f_SetCallback(fg_Move(_fOnFinishConnection));
		Internal.m_bOnFinishDone = true;

		if (Internal.m_bWantStopDefer)
			fp_StopDeferring();

		return NConcurrency::g_ActorSubscription / [this]() -> NConcurrency::TCFuture<void>
			{
				auto &Internal = *mp_pInternal;

				co_await fg_Move(Internal.m_fOnFinishConnection).f_Destroy();

				co_return {};
			}
		;
	}

	NConcurrency::CActorSubscription CWebSocketActor::fp_OnFinishClientConnection
		(
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EFinishConnectionResult _Result, CClientConnectionInfo _ConnectionInfo)> _fOnFinishConnection
			, NHTTP::CRequest _RequestHeader
			, NStr::CStr _ConnectToAddress
			, NStr::CStr _URI
			, NStr::CStr _Origin
			, NContainer::TCVector<NStr::CStr> _Protocols
		)
	{
		auto &Internal = *mp_pInternal;

		Internal.m_fOnFinishClientConnection.f_SetCallback(fg_Move(_fOnFinishConnection));
		Internal.m_bOnFinishDone = true;

		if (Internal.m_bWantStopDefer)
			fp_StopDeferring();

		auto &Line = _RequestHeader.f_GetRequestLine();
		Line.f_Set(NHTTP::EVersion_HTTP_1_1, NHTTP::EMethod_Get, _URI);
		auto &GeneralFields = _RequestHeader.f_GetGeneralFields();
		GeneralFields.f_SetUpgrade("websocket");
		GeneralFields.f_SetConnection(NHTTP::EConnectionToken_Upgrade);
		auto &RequestFields = _RequestHeader.f_GetRequestFields();
		RequestFields.f_SetHost(_ConnectToAddress);
		auto &EntityFields = _RequestHeader.f_GetEntityFields();
		if (!_Origin.f_IsEmpty())
			EntityFields.f_SetUnknownField("Origin", _Origin);
		NStr::CStr Protocols;
		for (auto &Protocol : _Protocols)
			fg_AddStrSep(Protocols, Protocol, ", ");

		NContainer::CByteVector RandomData;
		RandomData.f_SetLen(16);
		NCryptography::fg_GenerateRandomData(RandomData.f_GetArray(), RandomData.f_GetLen());

		NStr::CStr EncodedRandomData = NEncoding::fg_Base64Encode(RandomData);

		if (!Protocols.f_IsEmpty())
			EntityFields.f_SetUnknownField("Sec-WebSocket-Protocol", Protocols);
		EntityFields.f_SetUnknownField("Sec-WebSocket-Version", "13");
		EntityFields.f_SetUnknownField("Sec-WebSocket-Key", EncodedRandomData);

		// Offered only when the caller has said the transport has no intermediary to protect. A
		// server that does not answer with it leaves masking on, which is what makes this safe to
		// send to a peer that predates the extension
		if (Internal.m_Settings.m_bNegotiateUnmaskedFrames)
			EntityFields.f_SetUnknownField("Sec-WebSocket-Extensions", gc_pUnmaskedFramesExtension);

		for (auto &Protocol : _Protocols)
			Internal.m_ClientConnectionInput.m_Protocols[Protocol];
		Internal.m_ClientConnectionInput.m_EncodedKey = EncodedRandomData;

		_RequestHeader.f_WriteHeaders
			(
				[&](uint8 const *_pData, umint _nBytes) noexcept
				{
					Internal.m_OutgoingData.f_InsertBack(_pData, _nBytes);
					Internal.f_TrackArenaBytes(_nBytes);
				}
			)
		;
		fp_UpdateSend();

		return NConcurrency::g_ActorSubscription / [this]() -> NConcurrency::TCFuture<void>
			{
				auto &Internal = *mp_pInternal;

				co_await fg_Move(Internal.m_fOnFinishClientConnection).f_Destroy();

				co_return {};
			}
		;
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SetTimeout(fp64 _Seconds)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Destroying websocket");

		auto &Internal = *mp_pInternal;
		Internal.m_Settings.m_Timeout = _Seconds;
		Internal.f_SetupTimeout();

		co_return {};
	}

	void CWebSocketActor::CInternal::f_StopTimeout()
	{
		m_TimeoutTimerSubscription.f_Clear();
		m_pTimeoutPingMessage.f_Clear();

		if (!m_fOnFinishConnection.f_IsEmpty())
		{
			CConnectionInfo ConnectionInfo;
			ConnectionInfo.m_ErrorStatus = EWebSocketStatus_InternalError;
			ConnectionInfo.m_Error = "Never got a finish connection result";
			f_FinishConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo));
		}
		if (!m_fOnFinishClientConnection.f_IsEmpty())
		{
			CClientConnectionInfo ConnectionInfo;
			ConnectionInfo.m_ErrorStatus = EWebSocketStatus_InternalError;
			ConnectionInfo.m_Error = "Never got a finish client connection result";
			f_FinishClientConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo));
		}
	}

	void CWebSocketActor::CInternal::f_SetupTimeout()
	{
		m_TimeoutTimerSubscription.f_Clear();
		m_pTimeoutPingMessage.f_Clear();

		if (m_Settings.m_Timeout == 0.0)
			return; // Timeout disabled

		m_TimeoutReceivedData.f_Start();
		m_TimeoutSentData.f_Start();

		// Built here and frozen when it is handed to the shared pointer: the ping message is
		// sent repeatedly, so consumers may hold it while the next timeout fires
		umint MessageSize = NStr::fg_StrLen(gs_PingMessageData);
		NContainer::CIOByteVector PingMessage;
		PingMessage.f_SetLen(MessageSize); // TCVector has 16 as min size
		NMemory::fg_MemCopy(PingMessage.f_GetArray(), gs_PingMessageData, MessageSize);
		m_pTimeoutPingMessage = fg_Construct(fg_Move(PingMessage));

		auto Sequence = ++m_TimeoutTimerSubscriptionSequence;
		fg_RegisterTimer
			(
				m_Settings.m_Timeout/2.0
				, [this]() -> NConcurrency::TCFuture<void>
				{
					f_UpdateTimeout();
					co_return {};
				}
				, fg_ThisActor(m_pThis)
			)
			> [this, Sequence](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Subscription)
			{
				if (!_Subscription || m_TimeoutTimerSubscriptionSequence != Sequence)
					return;
				m_TimeoutTimerSubscription = fg_Move(*_Subscription);
			}
		;
	}

	void CWebSocketActor::CInternal::f_OnReceivedData()
	{
		m_TimeoutReceivedData.f_Start();
	}

	void CWebSocketActor::CInternal::f_OnSentData()
	{
		m_TimeoutSentData.f_Start();
	}

	void CWebSocketActor::CInternal::f_OnTimeoutPongReceived()
	{
		m_bPendingPing = false;
		m_bSentPing = false;
	}

	void CWebSocketActor::CInternal::f_UpdateTimeout()
	{
		if (m_State == EState_Connected)
		{
			if (!m_bPendingPing)
			{
				m_bPendingPing = true;
				m_pThis->f_SendPing(m_pTimeoutPingMessage).f_OnResultSet
					(
						[this, ThisWeak = fg_ThisActor(m_pThis).f_Weak()](NConcurrency::TCAsyncResult<void> &&_Result) mutable
						{
							if (!_Result)
								return;
							auto This = ThisWeak.f_Lock();
							if (!This)
								return;
							fg_Dispatch
								(
									This
									,[this]
									{
										if (m_bPendingPing)
											m_bSentPing = true;
									}
								)
								.f_DiscardResult()
							;
						}
					)
				;
			}

			if (m_bSentPing)
			{
				if (m_TimeoutReceivedData.f_GetTime() > m_Settings.m_Timeout)
					m_pThis->fp_Disconnect(EWebSocketStatus_Timeout, NStr::fg_Format("Timeout({}) receiving data", m_Settings.m_Timeout), true, EWebSocketCloseOrigin_Local);
			}

			if (m_nOutgoingQueuedBytes)
			{
				if (m_TimeoutSentData.f_GetTime() > m_Settings.m_Timeout)
					m_pThis->fp_Disconnect(EWebSocketStatus_Timeout, NStr::fg_Format("Timeout({}) sending data", m_Settings.m_Timeout), true, EWebSocketCloseOrigin_Local);
			}
		}
		// A remote close that arrived with sends still queued leaves the disconnected state
		// draining toward the parked close frame; that drain makes progress only while the peer
		// reads, so it stays under the same both-directions-stale watchdog as the other
		// non-connected states — without it a peer that stops reading after its close frame
		// would wedge the backlog and its promises forever
		else if (m_State != EState_Disconnected || m_nOutgoingQueuedBytes || m_bCloseFramePending || !m_PendingMessages.f_IsEmpty())
		{
			NNetwork::ENetTCPState State = NNetwork::ENetTCPState_None;
			if (m_pSocket && m_pSocket->f_IsValid())
				State = m_pSocket->f_GetState();
			if (State)
				m_pThis->fp_ProcessState(State);

			// The disconnected drain stalls on send progress alone: the peer may keep writing
			// — every received byte restarts the receive timer — while never reading, which
			// would hold the both-stale condition below off forever with the backlog and its
			// promises still queued
			if (m_State == EState_Disconnected && m_nOutgoingQueuedBytes && m_TimeoutSentData.f_GetTime() > m_Settings.m_Timeout)
			{
				m_pThis->fp_Disconnect(EWebSocketStatus_Timeout, NStr::fg_Format("Timeout({}) sending data", m_Settings.m_Timeout), true, EWebSocketCloseOrigin_Local);
				return;
			}

			if (m_TimeoutReceivedData.f_GetTime() > m_Settings.m_Timeout && m_TimeoutSentData.f_GetTime() > m_Settings.m_Timeout)
				m_pThis->fp_Disconnect(EWebSocketStatus_Timeout, NStr::fg_Format("Timeout({}) in non-connected state", m_Settings.m_Timeout), true, EWebSocketCloseOrigin_Local);
		}
	}
}
