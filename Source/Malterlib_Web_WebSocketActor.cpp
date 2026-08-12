// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/LogError>
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

			// Flush only between frames; a direct-read masked frame still needs its positions in m_Data.
			void f_FlushDataToStorage()
			{
				if (m_Data.f_IsEmpty())
					return;

				// Use shared segments so payload consumers can take retaining subviews.
				m_Storage.f_AppendShared(NContainer::CSharedByteVector(fg_Move(m_Data)));
				m_Data = NContainer::CIOByteVector();
			}

			uint64 m_Length = 0;
			NContainer::CIOByteVector m_Data;
			NStream::CBinaryStorage m_Storage; // Receive-buffer views interleaved with contiguous assembly in arrival order.
			umint m_Position = 0;
			umint m_nInterleavedBytes = 0; // Nonpayload bytes pin shared buffers too; excessive interleaving switches later payload to copying.
			uint8 m_Mask[4];
			CHeader m_Header;
			bool m_bHeaderFinished = false;
		};

		// Retains payload storage until every referencing frame is sent.
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

			NStorage::TCSharedPointer<CPayloadOwner> m_pOwner; // Emit frames lazily; masking copies into the arena so shared payload remains immutable.
			NContainer::TCVector<NSys::CIoSpan> m_Spans;
			umint m_nTotalBytes = 0;
			umint m_iPayloadSent = 0;
			umint m_iSpan = 0;
			umint m_iSpanOffset = 0;

			EOpcode m_Opcode;
			bool m_bFinished = false;
			bool m_bView = false;
		};

		// Ordered output contains arena ranges and retaining payload views.
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
			NStorage::TCSharedPointer<CPayloadOwner> m_pOwnerKeepAlive; // Retains payload until this segment is fully sent.
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

		constexpr static umint gc_ReceiveChunkSize = 24576; // Must exceed one framed TLS record to avoid per-record holdover copies.

		constexpr static umint gc_CopySmallMessageThreshold = 1024; // Copying below this size costs less than view bookkeeping.

		constexpr static umint gc_DirectReadThreshold = 4 * gc_ReceiveChunkSize; // Below this size buffered prefix cost outweighs direct-read savings.

		constexpr static umint gc_MaxInterleavedBytes = 64 * 1024; // Bound nonpayload gaps so control-frame padding cannot pin a buffer per tiny payload fragment.

		// Avoid the vector's sixteen-entry minimum for a single span.
		NContainer::TCVector<NSys::CIoSpan> fg_MakeSpanVector(uint8 const *_pData, umint _nBytes)
		{
			NContainer::TCVector<NSys::CIoSpan> Spans;
			Spans.f_SetLen(1, true);
			Spans.f_GetArray()[0] = NSys::CIoSpan{.m_pData = _pData, .m_nBytes = _nBytes};

			return Spans;
		}

		// Compare extension tokens before optional semicolon parameters in the comma-separated header.
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
		struct CClientConnectionInput
		{
			NStr::CStr m_EncodedKey;
			NContainer::TCSet<NStr::CStr> m_Protocols;
		};

		// Indexed send reservations permit out-of-order reports. Bounded window/gather sizes fit 32-bit fields; free entries form an index list.
		struct CSendReservation
		{
			static constexpr uint32 mc_iNone = TCLimitsInt<uint32>::mc_Max;

			uint32 m_nBytes = 0;
			uint32 m_iNextFree = mc_iNone;
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
			// Negotiation keeps masking on until both peers agree, even when settings otherwise permit unmasked frames.
			m_bMaskFrames = !_Settings.m_bAllowUnmaskedFrames || _Settings.m_bNegotiateUnmaskedFrames;

			// Bounded so every gather size derived from it stays well inside umint,
			// on 32 bit platforms included
			m_Settings.m_FragmentationSize = fg_Min(m_Settings.m_FragmentationSize, umint(1) << 30);
			f_SizeSendReservations();

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

		auto f_GatherSendSpans
			(
				NSys::CIoSpan *o_pSpans
				, umint &o_nSpans
				, NContainer::TCVector<NStorage::TCSharedPointer<CPayloadOwner>> &o_KeepAlives
				, NStorage::TCSharedPointer<NContainer::CIOByteVector> &o_pArenaCopy
			)
			-> umint
		;
		void f_ConsumeSentBytes(umint _nSentBytes);

		void f_ReleaseReceiveState();
		void f_ReleaseOutgoingState();
		void f_TryReleaseDeferredReceiveState();

		void f_FinishClientConnection(EFinishConnectionResult _Result, CClientConnectionInfo &&_ConnectionInfo);
		void f_FinishConnection(EFinishConnectionResult _Result, CConnectionInfo &&_ConnectionInfo);

		umint f_SendWindowStartBytes() const;
		void f_SizeSendReservations();
		void f_ResetSendReservations();

		CWebSocketActor *m_pThis = nullptr;
		NStorage::TCUniquePointer<NNetwork::ICSocket> m_pSocket;
		NMib::NSys::CIoSubSystem *m_pIo = &NMib::NSys::fg_IoSubSystem();

		NMib::NNetwork::CNetAddress m_PeerAddress;

		EState m_State = EState_None;
		uint32 m_iFreeSendReservation = CSendReservation::mc_iNone;
		NNetwork::ENetTCPState m_DeferredCloseStates = NNetwork::ENetTCPState_None; // Defer close states until earlier stream data, including close frames, is delivered.

		NContainer::CPagedByteVector m_IncomingData{4096};
		NContainer::CPagedByteVector m_OutgoingData{4096};
		NContainer::TCLinkedList<COutgoingSegment> m_OutgoingSegments;
		COutgoingSegment *m_pLastOutgoingSegment = nullptr;
		uint64 m_nOutgoingQueuedBytes = 0; // Logical bytes may exceed 32-bit allocation size when payloads are referenced repeatedly.
		std::deque<COutgoingDataPromise> m_OutgoingDataPromises;

		NStorage::TCVariant<void, CConnectionInfo, CClientConnectionInfo> m_ConnectionInfo;
		CClientConnectionInput m_ClientConnectionInput;
		NStr::CStr m_Key;
		NStr::CStr m_Version;

		CMessage m_NextMessage;
		CMessage m_PendingMessage;
		CWebSocketActor::CCloseInfo m_CloseInfo;

		uint64 m_nDirectReadRemaining = 0; // Read no further than this payload so the next header still enters the framing buffer.
		NContainer::CIOByteVector *m_pDirectReadData = nullptr;
		umint m_DirectReadFrameStart = 0;
		NStorage::TCSharedPointer<NConcurrency::CIoCompletionOpTracker> m_pOpTracker;
		NContainer::CByteVector m_ReceiveChunk;

		NContainer::TCMap<uint32, NContainer::TCLinkedList<COutgoingMessage>> m_PendingMessages;
		NContainer::TCLinkedList<COutgoingMessage> *m_pLastPendingMessagesList;

		NContainer::CByteVector m_CloseFramePayload; // Emit after pending messages drain; framing the whole masked backlog at once would copy it all.
		NContainer::TCLinkedList<COutgoingMessage> m_PostCloseMessages; // Do not admit post-close messages to the stream; teardown rejects their promises.

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

		NStorage::TCSharedPointer<NSys::CIoStreamBackpressure> m_pReceiveBackpressure;

		umint m_nSendOpsInFlight = 0;
		umint m_nOutgoingSubmitted = 0; // Reserved bytes excluded from later gathers until completion.

		// A continuation carries no reservation of its own
		static constexpr umint mc_iNoReservation = umint(-1);

		NContainer::TCVector<CSendReservation> m_SendReservations; // Preallocated for the window; indexed free list avoids per-send scanning or growth.
		umint m_nSendReservationsInUse = 0;
		umint m_nMaxSendReservations = 8;
		umint m_nSendBytesUnreleased = 0; // Accepted bytes whose release callback has not run.

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
		umint m_bDirectReadToStorage:1 = false; // Unmasked binary payload appends retaining receive-buffer views.
		umint m_bReceiveStreamShared:1 = false; // Shared segment resolution permits direct reads into retaining storage views.
		umint m_bCloseFramePending:1 = false;
		umint m_bReceiveStreamActive:1 = false; // Close states wait for the receive stream's one terminal segment.
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

	// Resolve in-flight work through the interface cached at activation, even after new submits stop.
	NNetwork::ICSocketCompletionIo *CWebSocketActor::CInternal::f_GetCompletionIo()
	{
		if (!m_bCompletionIo || !m_pSocket)
			return nullptr;

		return m_pCompletionIo;
	}

	// New submissions only; resolve existing operations through the cached interface.
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

	// Frame views lazily; unmasked payload stays shared while masked payload is copied into the arena.
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

	// Copied messages cannot fragment; use views for payloads larger than one frame even below the copy threshold.
	umint CWebSocketActor::CInternal::f_GetCopyThreshold() const
	{
		return fg_Min(umint(gc_CopySmallMessageThreshold), m_Settings.m_FragmentationSize);
	}

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
			if (umint nFrameAhead = m_pIo->f_WebSocketFrameAhead(); nFrameAhead > 1)
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
				// Retain the message at the queue head until every payload fragment is emitted.
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

	umint CWebSocketActor::CInternal::f_SendWindowStartBytes() const
	{
		return m_Settings.f_GetSendWindowStartBytes();
	}

	// Preallocate the bounded window's reservations so sends never grow the pool.
	void CWebSocketActor::CInternal::f_SizeSendReservations()
	{
		umint nFrameBytes = f_SendWindowStartBytes() / 8;
		m_nMaxSendReservations = fg_Max(umint(8), m_Settings.f_GetSendWindowBytes() / nFrameBytes + 2);
		umint nEntries = m_SendReservations.f_GetLen();
		if (nEntries >= m_nMaxSendReservations)
			return;

		m_SendReservations.f_SetLen(m_nMaxSendReservations);
		CSendReservation *pReservations = m_SendReservations.f_GetArray();
		for (CSendReservation *pReservation = pReservations + nEntries, *pEnd = pReservations + m_nMaxSendReservations; pReservation != pEnd; ++pReservation)
		{
			pReservation->m_iNextFree = m_iFreeSendReservation;
			m_iFreeSendReservation = uint32(pReservation - pReservations);
		}
	}

	// Tearing the connection down gives every reservation back at once; operations still in
	// flight then find their own already accounted for
	void CWebSocketActor::CInternal::f_ResetSendReservations()
	{
		m_iFreeSendReservation = CSendReservation::mc_iNone;
		CSendReservation *pReservations = m_SendReservations.f_GetArray();
		for (CSendReservation *pReservation = pReservations + m_SendReservations.f_GetLen(); pReservation != pReservations;)
		{
			--pReservation;
			pReservation->m_nBytes = 0;
			pReservation->m_iNextFree = m_iFreeSendReservation;
			m_iFreeSendReservation = uint32(pReservation - pReservations);
		}

		m_nSendReservationsInUse = 0;
		m_nSendBytesUnreleased = 0;
	}

	auto CWebSocketActor::CInternal::f_GatherSendSpans
		(
			NSys::CIoSpan *o_pSpans
			, umint &o_nSpans
			, NContainer::TCVector<NStorage::TCSharedPointer<CPayloadOwner>> &o_KeepAlives
			, NStorage::TCSharedPointer<NContainer::CIOByteVector> &o_pArenaCopy
		)
		-> umint
	{
		umint nSpans = 0;
		umint nGatheredBytes = 0;
		umint nArenaBytes = 0;
		NContainer::TCBitArray<NNetwork::ICSocket::mc_MaxSendSpans> ArenaSpans;

		// Gather at least one frame, retaining a useful minimum batch for small-fragment connections.
		umint nMaxGatherBytes = fg_Max(umint(256 * 1024), m_Settings.m_FragmentationSize);

		// Skip earlier reservations to avoid duplicate sends; advance the arena offset over even skipped segments.
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
				// Bound spans for scalar fallbacks and SSL's signed transfer length.
				umint iStart = Segment.m_iSent + nSkipWithin;
				umint nSegmentBytes = fg_Min(Segment.m_nBytes - iStart, nMaxGatherBytes - nGatheredBytes);

				o_pSpans[nSpans].m_pData = Segment.m_pData + iStart;
				o_pSpans[nSpans].m_nBytes = nSegmentBytes;
				nGatheredBytes += nSegmentBytes;
				++nSpans;
				bFull = nSpans >= NNetwork::ICSocket::mc_MaxSendSpans || nGatheredBytes >= nMaxGatherBytes;

				// Retain payload owners until kernel buffer release, which may follow completion and segment retirement.
				if (Segment.m_pOwnerKeepAlive)
					o_KeepAlives.f_Insert(Segment.m_pOwnerKeepAlive);
			}
		}

		// Arena pages are recycled at completion. Copy them into release-owned storage for late-release sends;
		// prompt-release and readiness paths can borrow them directly.
		if (nArenaBytes && m_pCompletionIo && !m_pCompletionIo->f_SendReleaseIsPrompt())
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

	// Advance arena/view progress and settle write promises in stream order.
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

	// Release retained payloads when disconnected, even if the actor remains alive; wait for kernel sends first.
	void CWebSocketActor::CInternal::f_ReleaseOutgoingState()
	{
		m_OutgoingSegments.f_Clear();
		m_pLastOutgoingSegment = nullptr;
		m_nOutgoingQueuedBytes = 0;
		m_nOutgoingSubmitted = 0;
		f_ResetSendReservations();
		m_OutgoingData.f_RemoveFront(m_OutgoingData.f_GetLen());
	}

	// Release close-deferred storage only after kernel buffer references end.
	void CWebSocketActor::CInternal::f_TryReleaseDeferredReceiveState()
	{
		if (m_nSendOpsInFlight)
			return;

		// Take deferred close states before reporting them; reporting can reenter disconnect and must not deliver them twice.
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
		// Release receive allocations when the socket is gone; a retained closed actor must not pin an advertised frame indefinitely.
		if (m_nSendOpsInFlight)
			m_bDeferredShutdownCleanup = true;
		else
		{
			f_ReleaseReceiveState();
			f_ReleaseOutgoingState();
		}

		// Cancel queued-message promises now; the deferred flush cannot run without a socket.
		// Retain arena and segments still referenced by kernel sends until completion releases them.
		m_PendingMessages.f_Clear();
		m_pLastPendingMessagesList = nullptr;
		m_PostCloseMessages.f_Clear();
		m_OutgoingDataPromises.clear();
		m_bCloseFramePending = false;
		m_CloseFramePayload.f_Clear();

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

		// Destroy aborts outstanding output so an unresponsive peer cannot hold the release fence through retransmission timeout.
		// Graceful callers use f_CloseWithLinger to drain and shut down first.
		if (Internal.m_pSocket)
			Internal.m_pSocket->f_SetAbortOnClose();

		if (Internal.m_pOpTracker)
		{
			// Close cancels kernel work; keep queues, arena and payload owners alive until the operation tracker drains across this await.
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
		Internal.f_ResetSendReservations();
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

			// A disconnected socket can still be sending its close reply or awaiting peer FIN; destroying it now would cancel graceful output.
			if (!Internal.m_pSocket)
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

	// The future resolving means sent, not that the storage may change: zero-copy sends keep reading it until kernel release
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

	// Queues separate binary messages together; the last message's promise reports the batch outcome.
	NConcurrency::TCFuture<void> CWebSocketActor::f_SendBinaryStorages(NContainer::TCVector<NStorage::TCSharedPointer<NStream::CBinaryStorage const>> _Messages, uint32 _Priority)
	{
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

		// Validate before queueing so an invalid batch cannot partially reach the wire.
		for (auto const &pMessage : _Messages)
		{
			if (pMessage->f_GetTotalLength() > Internal.m_Settings.m_MaxMessageSize)
				co_return DMibErrorInstance("Message is bigger than max message size");
		}

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		// Queue-order completion and rejection of pending promises make the last promise cover the whole batch.
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

		// Defer behind already-queued send calls so a burst shares one gather and flush.
		DMibLogWarningOrDiscardResult(fg_ThisActor(this).f_Bind<&CWebSocketActor::fp_FlushSend>(), "Mib/Web", "Flushing the websocket send queue failed");
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

		auto *pCompletionIo = Internal.m_pSocket->f_GetCompletionIo();
		if (!pCompletionIo)
			return;

		Internal.m_pCompletionIo = pCompletionIo;
		Internal.m_bCompletionIo = true;
		Internal.m_pOpTracker = fg_Construct();

		// Reject synchronous entry points before submitting the first operation.
		pCompletionIo->f_OnCompletionActivated();

		DMibLog(DebugVerbose3, " ++++ {} {} Completion transfers active", fg_ThisActor(this), !Internal.m_bClient);

		if (_bSubmitReceive)
			fp_StartReceiveStream();
	}

	void CWebSocketActor::fp_StartReceiveStream()
	{
		auto &Internal = *mp_pInternal;

		// Never restart after the stream's terminal segment, even if a late readiness edge arrives.
		if (Internal.m_bReceiveStreamActive || Internal.m_bReceiveStreamEnded)
			return;

		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
			return;

		auto *pCompletionIo = Internal.f_GetCompletionIoReceive();
		if (!pCompletionIo)
			return;

		// Buffer destructors release charged capacity; resume on this actor after crossing the threshold.
		umint nBufferBytes = pCompletionIo->f_GetReceiveBufferBytes();
		auto pBackpressure = NStorage::TCSharedPointer<NSys::CIoStreamBackpressure>(fg_Construct());
		pBackpressure->m_nLimitBytes = NNetwork::fg_GetReceiveWindowBytes(*Internal.m_pIo, nBufferBytes);

		// State the maximum retained message plus interleaving overhead before stream start, so the backend reserves
		// partial-buffer headroom and can resume before the final frame arrives. Preserve explicit test overrides.
		pBackpressure->m_nResumeBytes = pBackpressure->m_nLimitBytes / 2;
		if (!Internal.m_pIo->f_ReceiveWindowBytesOverride())
		{
			pBackpressure->m_nConsumerHoldBytes =
				fg_Min(Internal.m_Settings.m_MaxMessageSize, TCLimitsInt<umint>::mc_Max - gc_MaxInterleavedBytes) + gc_MaxInterleavedBytes
			;
		}
		pBackpressure->m_fResume = [WeakThis = fg_ThisActor(this).f_Weak()]() mutable
			{
				if (auto This = WeakThis.f_Lock())
					DMibLogWarningOrDiscardResult(This.f_Bind<&CWebSocketActor::fp_ReceiveWindowResume>(), "Mib/Web", "Resuming the receive window failed");
			}
		;
		Internal.m_pReceiveBackpressure = pBackpressure;

		bool bStarted = pCompletionIo->f_StartReceiveStream
			(
				fg_Move(pBackpressure)
				, [WeakThis = fg_ThisActor(this).f_Weak()](NSys::CIoStreamSegment &&_Segment) mutable
				{
					// The queued segment retains its buffer even if teardown drops the job.
					if (auto This = WeakThis.f_Lock())
						DMibLogWarningOrDiscardResult(This.f_Bind<&CWebSocketActor::fp_ReceiveSegment>(fg_Move(_Segment)), "Mib/Web", "Delivering a received segment failed");
				}
			)
		;

		if (bStarted)
		{
			Internal.m_bReceiveStreamActive = true;

			// Drain readiness-held bytes in a later actor job to avoid reentering the processing that started the stream.
			DMibLogWarningOrDiscardResult(fg_ThisActor(this).f_Bind<&CWebSocketActor::fp_DrainHeldInput>(), "Mib/Web", "Draining the input the socket held failed");
		}

	}

	void CWebSocketActor::fp_ReceiveWindowResume()
	{
		auto &Internal = *mp_pInternal;

		if (f_IsDestroyed())
			return;

		if (Internal.m_pSocket && Internal.m_pSocket->f_IsValid() && Internal.m_pCompletionIo)
			Internal.m_pCompletionIo->f_ResumeReceiveStream();
	}

	// Transport-generated output needs explicit drain operations because completion sends provide no write-readiness edge.
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

	// A continuation advances transport-held output and may offer no new plaintext.
	void CWebSocketActor::fp_SubmitSendOp(bool _bContinue, umint _iInheritedReservation)
	{
		auto &Internal = *mp_pInternal;

		// A continuation inherits its reservation. Release it on every refused path or the queue remains permanently reserved.
		umint iReservation = _bContinue ? _iInheritedReservation : Internal.mc_iNoReservation;

		auto fReleaseOnFailure = NMib::g_OnScopeExit / [&]
			{
				if (iReservation == Internal.mc_iNoReservation)
					return;

				auto &Reservation = Internal.m_SendReservations[iReservation];

				if (!Reservation.m_nBytes)
					return;

				DMibFastCheck(Internal.m_nOutgoingSubmitted >= Reservation.m_nBytes);

				Internal.m_nOutgoingSubmitted -= Reservation.m_nBytes;
				Reservation.m_nBytes = 0;
				Reservation.m_iNextFree = Internal.m_iFreeSendReservation;
				Internal.m_iFreeSendReservation = uint32(iReservation);
				--Internal.m_nSendReservationsInUse;
			}
		;

		if (!Internal.m_nOutgoingQueuedBytes && !_bContinue)
			return;

		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
			return;

		auto *pCompletionIo = Internal.f_GetCompletionIoSend();
		if (!pCompletionIo)
			return;

		// Gate new batches only; buffer release retries when the byte window opens.
		if (!_bContinue && pCompletionIo->f_IsSendWindowFull(Internal.m_nSendBytesUnreleased, Internal.f_SendWindowStartBytes()))
		{
#if DMibConfig_IoDebug_Enable
			if (auto *pStats = NNetwork::fg_NetIoStats())
				pStats->m_nSendBlocked.f_FetchAdd(1, NAtomic::gc_MemoryOrder_Relaxed);
#endif

			return;
		}

		// Never gate continuations on staging capacity: sending held ciphertext is what frees that capacity.
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


		// Reservations outlive transport records until continuation chains settle; retry new batches when a slot is released.
		if (!_bContinue)
		{
			umint nMaxReservations = pCompletionIo->f_SupportsSendStaging() ? umint(8) : Internal.m_nMaxSendReservations;
			if (Internal.m_nSendReservationsInUse >= nMaxReservations)
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

		// Continuations offer no plaintext; the queue still holds bytes already reserved by the original transfer.
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

		if (!_bContinue)
		{
			DMibFastCheck(Internal.m_iFreeSendReservation != CInternal::CSendReservation::mc_iNone);
			iReservation = umint(Internal.m_iFreeSendReservation);
			Internal.m_iFreeSendReservation = Internal.m_SendReservations[iReservation].m_iNextFree;

			++Internal.m_nSendReservationsInUse;

			Internal.m_SendReservations[iReservation].m_nBytes = uint32(nGatheredBytes);
			Internal.m_nOutgoingSubmitted += nGatheredBytes;
		}

		// Both completion and release functors retain the tracker until destruction, including refusal and exception paths.
		NSys::FIoCompletion fOnComplete =
			[
				Hold = NConcurrency::CIoCompletionOpHold(Internal.m_pOpTracker)
				, iReservation
				, WeakThis = fg_ThisActor(this).f_Weak()
			]
			(NSys::CIoCompletion _Result) mutable
			{
				if (auto This = WeakThis.f_Lock())
					DMibLogWarningOrDiscardResult(This.f_Bind<&CWebSocketActor::fp_SendCompleted>(_Result, iReservation), "Mib/Web", "Completing a send failed");
			}
		;

		NNetwork::FSocketSendReleased fOnReleased =
			[
				Hold = NConcurrency::CIoCompletionOpHold(Internal.m_pOpTracker)
				, KeepAlives = fg_Move(KeepAlives)
				, pArenaCopy = fg_Move(pArenaCopy)
				, WeakThis = fg_ThisActor(this).f_Weak()
				, nGatheredBytes
			]
			(umint _iTransfer) mutable
			{
				KeepAlives.f_Clear();
				pArenaCopy.f_Clear();

				if (auto This = WeakThis.f_Lock())
					DMibLogWarningOrDiscardResult(This.f_Bind<&CWebSocketActor::fp_SendBufferReleased>(_iTransfer, nGatheredBytes), "Mib/Web", "Releasing a send buffer failed");
			}
		;

		umint nScheduled = 0;
		bool bSubmitted;
		if (_bContinue)
			bSubmitted = pCompletionIo->f_ContinueSend(fg_Move(fOnComplete), fg_Move(fOnReleased));
		else
		{
			nScheduled = pCompletionIo->f_SubmitSendVectored(Spans, nSpans, fg_Move(fOnComplete), fg_Move(fOnReleased));
			DMibFastCheck(nScheduled <= nGatheredBytes);
			bSubmitted = nScheduled != 0;
		}

		if (bSubmitted)
		{
			fReleaseOnFailure.f_Clear();

			// Reserve only the accepted prefix; the untaken suffix remains available for the next gather.
			if (!_bContinue && nScheduled < nGatheredBytes)
			{
				Internal.m_nOutgoingSubmitted -= nGatheredBytes - nScheduled;
				Internal.m_SendReservations[iReservation].m_nBytes = nScheduled;
			}

			// Release retains the whole gather, including the untaken tail; temporary double-counting only applies backpressure early.
			Internal.m_nSendBytesUnreleased += nGatheredBytes;
		}
		else
		{
			// Refusal is terminal; leaving reserved plaintext queued would strand send futures.
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
		fp_ReceiveStreamInput(fg_Move(_Segment), false);
	}

	// Drain readiness-held TLS bytes on activation; the peer may send no further segment to trigger them.
	void CWebSocketActor::fp_DrainHeldInput()
	{
		fp_ReceiveStreamInput(NSys::CIoStreamSegment(), true);
	}

	void CWebSocketActor::fp_ReceiveStreamInput(NSys::CIoStreamSegment &&_Segment, bool _bHeldOnly)
	{
		auto &Internal = *mp_pInternal;

		if (f_IsDestroyed())
			return;

		// Only TLS sockets hold input, and they never deliver shared segments. A shared segment queued
		// ahead of the drain can have started a storage-backed direct read, which has no contiguous destination
		if (_bHeldOnly && Internal.m_bReceiveStreamShared)
			return;

		auto &Segment = _Segment;
		bool bTerminal = !_bHeldOnly && (Segment.m_Status != NSys::EIoCompletionStatus::mc_Done || !Segment.m_nBytes);

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
			NSys::CIoCompletion Result;
			Internal.m_pCompletionIo->f_ResolveReceiveSegment(Segment, nullptr, 0, Result);
			Internal.f_TryReleaseDeferredReceiveState();
			return;
		}

#if DMibConfig_Tests_Enable
		// Bank test-held bytes without delivery or timeout refresh; the debug re-drive flushes them later.
		bool bDebugBank = Internal.m_bDebugNoProcessing || Internal.m_bDebugNoProcessingReceive;
#else
		constexpr bool bDebugBank = false;
#endif

		// Parse retaining views directly; route payload to its message destination and framing bytes to the incoming pages.
		if (!bTerminal && !_bHeldOnly)
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
					Internal.m_bReceiveStreamShared = true;

					Internal.m_nReceivedBytes += SharedData.f_GetLen();

					umint iOffset = 0;
					while (iOffset < SharedData.f_GetLen())
					{
						// Drop payload only after the close callback; disconnecting still needs the peer's close frame.
						if (Internal.m_State == EState_Disconnected)
							break;

						if (Internal.m_nDirectReadRemaining)
						{
							umint nRemaining = (umint)Internal.m_nDirectReadRemaining;
							umint nCopy = fg_Min(nRemaining, SharedData.f_GetLen() - iOffset);

							if (Internal.m_bDirectReadToStorage)
							{
								// Retain views only within the interleave budget; larger gaps would pin more capacity than message size permits.
								auto &Target = Internal.m_bPendingMessage ? Internal.m_PendingMessage : Internal.m_NextMessage;
								if (Target.m_nInterleavedBytes > gc_MaxInterleavedBytes)
									Target.m_Storage.f_AppendBytes(SharedData.f_GetArray() + iOffset, nCopy);
								else
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
							// Feed small prefixes until the parser finds a data header, then let direct-read payload bypass the incoming pages.
							umint nInsert = fg_Min(SharedData.f_GetLen() - iOffset, umint(4096));
							Internal.m_IncomingData.f_InsertBack(SharedData.f_GetArray() + iOffset, nInsert);
							iOffset += nInsert;

							// Charge nonpayload gaps; subtract a frame's own prefix when direct reading begins.
							if (Internal.m_bPendingMessage && !Internal.m_PendingMessage.m_Storage.f_IsEmpty())
								Internal.m_PendingMessage.m_nInterleavedBytes += nInsert;

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

		// Resolve on the actor thread; one segment can cross framing and direct-payload destinations.
		bool bResolvedSegment = _bHeldOnly;
		bool bError = false;

		try
		{
			for (;;)
			{
				// Terminals need no destination, including when they interrupt a storage-backed direct read.
				void *pTarget = nullptr;
				umint nTarget = 0;
				bool bDirect = false;
				if (!bTerminal)
				{
					bDirect = Internal.m_nDirectReadRemaining && Internal.m_State != EState_Disconnected;
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

				// After the close callback, discard payload but continue draining for peer progress. Disconnecting still processes its close frame.
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

		// TLS close_notify can end the stream before kernel EOF. Release deferred close state now; the peer may wait for our alert before FIN.
		if (!bTerminal && Internal.m_pCompletionIo->f_ReceiveStreamEndedByProtocol())
		{
			bTerminal = true;
			Internal.m_bReceiveStreamActive = false;
			Internal.m_bReceiveStreamEnded = true;
		}

		if (Segment.m_Status == NSys::EIoCompletionStatus::mc_Error)
		{
			fp_Disconnect
				(
					EWebSocketStatus_AbnormalClosure
					, NStr::fg_Format("Socket receive error: {}", NNetwork::fg_FormatSocketIoError(Segment.m_Error))
					, true
					, EWebSocketCloseOrigin_Remote
				)
			;
			return;
		}

		if (bError)
		{
			fp_Disconnect(EWebSocketStatus_AbnormalClosure, "Socket receive failed", true, EWebSocketCloseOrigin_Remote);
			return;
		}

		if (bTerminal)
		{
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
				// Keep the reservation until the transport finishes or the same plaintext can be gathered twice. Continuations reserve no new bytes.
				if (_iReservation == Internal.mc_iNoReservation)
					return;

				auto &Reservation = Internal.m_SendReservations[_iReservation];

				if (!Reservation.m_nBytes)
					return;

				DMibFastCheck(Internal.m_nOutgoingSubmitted >= Reservation.m_nBytes);

				Internal.m_nOutgoingSubmitted -= Reservation.m_nBytes;
				Reservation.m_nBytes = 0;
				Reservation.m_iNextFree = Internal.m_iFreeSendReservation;
				Internal.m_iFreeSendReservation = uint32(_iReservation);
				--Internal.m_nSendReservationsInUse;
			}
		;

		if (f_IsDestroyed())
			return;

		// Resolve wire bytes to plaintext progress; pending transport records can require a continuation.
		bool bSocketUsable = Internal.m_pSocket && Internal.m_pSocket->f_IsValid();
		bool bResolved = true;
		if (bSocketUsable && Internal.m_pCompletionIo)
			bResolved = Internal.m_pCompletionIo->f_ResolveSend(_Result);

		if (_Result.m_Status == NSys::EIoCompletionStatus::mc_Cancelled || !bSocketUsable)
		{
			Internal.m_nOutgoingSubmitted = 0;
			Internal.f_ResetSendReservations();
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

		// Move the string into its retained owner before taking a span of its final storage.
		NStorage::TCSharedPointer<TCPayloadOwner<NStr::CStr>> pStringOwner = fg_Construct(fg_Move(Data));

		// Read spans before moving the owner; function arguments have unspecified evaluation order.
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

	// The future resolving means sent, not that the payload may change: zero-copy sends keep reading it until kernel release
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

		// Retain one owner for every message sliced from the same buffer set.
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

	// Buffer release unblocks staging generations and queued plaintext.
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

		fp_UpdateSend();
		fp_DrainSocketOutput();
	}


	// Partial frame construction cannot be rolled back; terminate rather than unwind after publishing incomplete framing.
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

	// Unmasked frames retain payload spans; masking copies into the arena so the source remains immutable.
	void CWebSocketActor::CInternal::f_SendMessageFrameSegmented(COutgoingMessage &_Message, umint _nFrameBytes) noexcept
	{
		umint nRemaining = _Message.m_nTotalBytes - _Message.m_iPayloadSent;
		DMibFastCheck(_nFrameBytes <= nRemaining);

		bool bLastFrame = _nFrameBytes == nRemaining;
		EOpcode Opcode = _Message.m_iPayloadSent == 0 ? _Message.m_Opcode : EOpcode_ContinuationFrame;
		bool bFinished = bLastFrame && _Message.m_bFinished;
		bool bMask = m_bClient && m_bMaskFrames;

		// Copy small frames to save span bookkeeping; masked frames always copy to preserve shared source bytes.
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
				// Fatal closure aborts retransmission so kernel-held pages release promptly.
				if (Internal.m_pSocket)
					Internal.m_pSocket->f_SetAbortOnClose();
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
				DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 2 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);

				// Local close follows earlier messages through bounded framing. Remote close finishes only a started fragmented message,
				// then replies promptly (RFC 6455 5.5.1); drop unstarted messages/pings but retain owed pongs.
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
						fp_ReleaseDeferredCloseStates();

						// Transport loss leaves no event to drive queued frames; reject them and attempt the close best effort.
						// A received close frame still follows the graceful incremental drain.
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

							// Delay write shutdown until the close frame drains; retain its progress timeout.
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
							// Keep the drain timeout until shutdown completes; the peer can stop reading after sending close.
						}
					}
					else if (WasState == EState_Disconnecting)
					{
						DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 10 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
						Internal.m_State = EState_Disconnected;
						fp_ReleaseDeferredCloseStates();

						// Drop undeliverable messages before the close reply so no payload follows close. A close may interrupt a fragment sequence.
						if (Internal.m_bCloseFramePending)
						{
							Internal.m_PendingMessages.f_Clear();
							Internal.m_pLastPendingMessagesList = nullptr;

							Internal.m_bCloseFramePending = false;
							NContainer::CByteVector Payload = fg_Move(Internal.m_CloseFramePayload);
							Internal.f_SendMessage(EOpcode_ConnectionClose, Payload.f_GetArray(), Payload.f_GetLen(), true);
							fp_UpdateSend();
						}

						// Delay write shutdown until the close reply drains; retain its progress timeout.
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

			if (Internal.m_pSocket)
				Internal.m_pSocket->f_SetAbortOnClose();
			Internal.m_pSocket.f_Clear();
			Internal.f_ShutdownDone(_Reason);
		}

		DMibLog(DebugVerbose3, " ++++ {} {} CWebSocketActor::fp_Disconnect 17 {}", fg_ThisActor(this), !Internal.m_bClient, _Reason);
		Internal.m_State = EState_Disconnected;
		Internal.f_StopTimeout();
		fp_ReleaseDeferredCloseStates();
	}

	// Release deferred close state once the handshake ends; route reentry through active state processing.
	void CWebSocketActor::fp_ReleaseDeferredCloseStates()
	{
		auto &Internal = *mp_pInternal;
		if (!Internal.m_DeferredCloseStates)
			return;

		NNetwork::ENetTCPState DeferredStates = Internal.m_DeferredCloseStates;
		Internal.m_DeferredCloseStates = NNetwork::ENetTCPState_None;
		fp_ProcessState(DeferredStates);
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

				// A TLS socket under completion sends leaves its close alert for the drain to carry
				fp_DrainSocketOutput();
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
			// Keep teardown framing bounded; after the close frame, later messages must remain off the wire and reject at destruction.
			Internal.f_WriteQueuedMessages(false);
			Internal.f_WriteCloseFrameWhenDrained();
		}

#if DMibConfig_Tests_Enable
		if (Internal.m_bDebugNoProcessing || Internal.m_bDebugNoProcessingSend)
			return;
#endif

		// Choose completion I/O per direction; readiness sends still need write edges to finish short transfers.
		if (auto *pCompletionIoSend = Internal.f_GetCompletionIoSend())
		{
			// Submit while the staging gate permits, preserving order; continuations still resolve through fp_SendCompleted.
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
					Internal.f_WriteQueuedMessages(false);
					Internal.f_WriteCloseFrameWhenDrained();
				}
			}

			// An empty payload queue can still owe a close frame or final shutdown.
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
				// Emit the parked close when backlog drains here; no later write edge may arrive.
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

		// Bound advertised data-frame length before reserving direct-read memory.
		if (!bControlMessage && Length > uint64(Internal.m_Settings.m_MaxFragmentSize))
		{
			fp_Disconnect(EWebSocketStatus_MessageTooBig, "Frame is bigger than the maximum fragment size", false, EWebSocketCloseOrigin_Local);
			return false;
		}

		umint nBytesAvailable = Internal.m_IncomingData.f_GetLen();

		if (!bControlMessage && Length >= gc_DirectReadThreshold && nBytesAvailable < Length)
		{
			auto &Target = Internal.m_bPendingMessage ? Internal.m_PendingMessage : Message;

			// Only unmasked binary frames can retain stream views; masking requires writable contiguous payload.
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

				// Flush earlier contiguous fragments and the buffered prefix before appending new views.
				Target.f_FlushDataToStorage();

				if (nBytesAvailable)
				{
					// Share the copied prefix so payload consumers can retain subviews.
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

				// The prefix went through the pages as payload, not as something between fragments
				Target.m_nInterleavedBytes -= fg_Min(Target.m_nInterleavedBytes, nBytesAvailable);

				Internal.m_pDirectReadData = nullptr;
				Internal.m_DirectReadFrameStart = 0;
				Internal.m_nDirectReadRemaining = Length - nBytesAvailable;
				Internal.m_bDirectReadToStorage = true;

				return false; // The receive loop completes the payload
			}

			// Count storage-backed fragments toward the whole-message limit; copy offsets remain relative to the contiguous buffer.
			auto &Dest = Target.m_Data;
			umint iStart = Dest.f_GetLen();
			umint nMessageBytes = iStart + umint(Target.m_Storage.f_GetTotalLength());
			if (nMessageBytes > Internal.m_Settings.m_MaxMessageSize || Length > uint64(Internal.m_Settings.m_MaxMessageSize - nMessageBytes))
			{
				fp_Disconnect(EWebSocketStatus_MessageTooBig, "Unsupported message length", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			// Use geometric f_SetLen growth for amortized assembly. The frame limit bounds speculative allocation,
			// including Windows commit charges when a peer stalls after advertising length.
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
			return false;

		uint8 *pMaskStart;
		if (Internal.m_bPendingMessage && !bControlMessage)
		{
			// Storage-backed fragments count toward the message limit; mask offsets stay relative to the contiguous buffer.
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

	void CWebSocketActor::CInternal::f_FinishDirectReadFrame()
	{
		auto &Message = m_NextMessage;
		CHeader &Header = Message.m_Header;

		// Storage-backed direct reads are unmasked; only contiguous payload needs the mask pass.
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

								// Stop masking only after the server accepts the offered extension.
								if (Internal.m_Settings.m_bNegotiateUnmaskedFrames)
								{
									auto pExtensions = EntityFields.f_GetUnknownField("Sec-WebSocket-Extensions");
									if (pExtensions && fg_ContainsExtension(*pExtensions, gc_pUnmaskedFramesExtension))
										Internal.m_bMaskFrames = false;
								}
								if (Internal.m_pSocket)
									ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
								ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;
								ConnectionInfo.m_FragmentationSize = Internal.m_Settings.m_FragmentationSize;
								ConnectionInfo.m_MaxFragmentSize = Internal.m_Settings.m_MaxFragmentSize;
								Internal.m_State = EState_Connected;

								// Let the enclosing readiness drain consume buffered input before arming the receive stream.
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
								ConnectionInfo.m_FragmentationSize = Internal.m_Settings.m_FragmentationSize;
								ConnectionInfo.m_MaxFragmentSize = Internal.m_Settings.m_MaxFragmentSize;

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

		// Accept the extension only when both configured locally and offered by the client.
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
			&& Internal.m_State != EState_Disconnected
			&& (_StateAdded & (NNetwork::ENetTCPState_Closed | NNetwork::ENetTCPState_RemoteClosed))
		)
		{
			// Poll close state can overtake the peer's close frame; defer until stream terminal.
			// After the close handshake, stop waiting: retained consumer buffers may prevent kernel terminal delivery.
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
				// Drain readable bytes before hangup because both can share one unordered event batch containing the close frame.
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
			// Finish banked direct-read payload before parsing more input; never overlap readiness reads with the stream.
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
						// Direct reads stop at the payload boundary so the next header enters the framing buffer.
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

					// Use a larger bounce buffer when the page tail is small to avoid shrinking each receive syscall.
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

					// Processing can activate completion I/O; leave the readiness loop before another receive overlaps it.
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
			// Only the drain-first path reaches here; a close frame may already have completed disconnect.
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
					fp_Disconnect
						(
							Internal.m_CloseInfo.m_Status == EWebSocketStatus_None ? EWebSocketStatus_AbnormalClosure : EWebSocketStatus_NormalClosure
							, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason())
							, false
							, EWebSocketCloseOrigin_Remote
							, true
						)
					;
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

		// Reevaluate completion support for the replacement socket.
		Internal.m_pCompletionIo = nullptr;

		Internal.m_pSocket->f_SetTransferSizeHint(fg_Max(Internal.m_Settings.m_FragmentationSize, umint(4096)) + NNetwork::gc_SocketFramingMargin);

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

		// Offer only for confidential point-to-point transport; unsupported peers retain masking.
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

		// Freeze reusable ping storage because previous sends may still retain it.
		umint MessageSize = NStr::fg_StrLen(gs_PingMessageData);
		NContainer::CIOByteVector PingMessage;
		PingMessage.f_SetLen(MessageSize);
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
		// Keep the drain watchdog armed after remote close; a peer that stops reading must not strand backlog promises.
		else if (m_State != EState_Disconnected || m_nOutgoingQueuedBytes || m_bCloseFramePending || !m_PendingMessages.f_IsEmpty())
		{
			NNetwork::ENetTCPState State = NNetwork::ENetTCPState_None;
			if (m_pSocket && m_pSocket->f_IsValid())
				State = m_pSocket->f_GetState();
			if (State)
				m_pThis->fp_ProcessState(State);

			// A peer may keep writing without reading; disconnected send backlog must time out on send progress alone.
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
