// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorCallbackManager>
#include <Mib/Container/PagedByteVector>

#include <Mib/Web/HTTP/Request>
#include <Mib/Web/HTTP/Response>

#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Cryptography/RandomData>
#include <Mib/Cryptography/Exception>
#include <Mib/Encoding/Base64>

#include <Mib/Encoding/JSON>

#include <deque>

#include "Malterlib_Web_WebSocket.h"

#if defined(DCompiler_clang) && !defined(DPlatformFamily_Emscripten)
#	define DEnableVector
#endif

#ifdef DEnableVector
typedef uint32 vec4uint32 __attribute__((ext_vector_type(4)));
#endif

namespace NMib::NWeb
{
	static ch8 const gs_PingMessageData[] = "WdI6Q6-HvOxlK5Vc";

	typedef NContainer::TCBinaryStreamPagedByteVector<NStream::CBinaryStreamBigEndian> CBinaryStreamPagedByteVector;
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

		enum
		{
			EOutgoingPageSize = 2048
			, EIncomingPageSize = 2048
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
			uint64 m_Length = 0;
			NContainer::CSecureByteVector m_Data;
			mint m_Position = 0;
			uint8 m_Mask[4];
			CHeader m_Header;
			bool m_bHeaderFinished = false;
		};

		struct COutgoingMessage
		{
			~COutgoingMessage()
			{
				if (m_pPromise)
					m_pPromise->f_SetException(DMibErrorInstance("Outgoing message abandoned"));
			}

			NStorage::TCSharedPointer<NContainer::CSecureByteVector> m_pData;
			EOpcode m_Opcode;
			NStorage::TCUniquePointer<NConcurrency::TCPromise<void>> m_pPromise;
			bool m_bFinished = false;
		};

		struct COutgoingDataPromise
		{
			COutgoingDataPromise() = default;
			COutgoingDataPromise(COutgoingDataPromise &&) = default;

			~COutgoingDataPromise()
			{
				if (m_pPromise)
					m_pPromise->f_SetException(DMibErrorInstance("Outgoing message abandoned"));
			}

			mint m_Position = 0;
			NStorage::TCUniquePointer<NConcurrency::TCPromise<void>> m_pPromise;
		};

		struct CPendingCallbackRegister
		{
			NConcurrency::TCActor<NConcurrency::CActor> m_Actor;
			NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _pMessage)> m_fReceiveBinaryMessage;
			NFunction::TCFunctionMovable<void (NStr::CStr const& _Message)> m_fReceiveTextMessage;
			NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _ApplicationData)> m_fReceivePing;
			NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _ApplicationData)> m_fReceivePong;
			NFunction::TCFunctionMovable<void (EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> m_fOnClose;
		};
	}

	struct CWebSocketActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(CWebSocketActor *_pThis, bool _bClient, mint _MaxMessageSize, mint _FragmentationSize, fp64 _Timeout)
			: m_pThis(_pThis)
			, m_OnReceiveBinaryMessage(_pThis, true)
			, m_OnReceiveTextMessage(_pThis, true)
			, m_OnReceivePing(_pThis, true)
			, m_OnReceivePong(_pThis, true)
			, m_OnClose(_pThis, true)
			, m_OnFinishConnection(_pThis, !_bClient)
			, m_OnFinishClientConnection(_pThis, _bClient)
			, m_IncomingData(EIncomingPageSize)
			, m_OutgoingData(EOutgoingPageSize)
			, m_bClient(_bClient)
			, m_MaxMessageSize(_MaxMessageSize)
			, m_FramentationSize(_FragmentationSize)
			, m_pLastPendingMessagesList(nullptr)
			, m_Timeout(_Timeout)
		{
			if (_bClient)
				m_ConnectionInfo.f_Set<2>();
			else
				m_ConnectionInfo.f_Set<1>();
		}

		~CInternal()
		{
			if (m_pClosePromise)
				m_pClosePromise->f_SetException(DMibErrorInstance("Abandoned close"));
		}

		struct CClientConnectionInput
		{
			NStr::CStr m_EncodedKey;
			NContainer::TCSet<NStr::CStr> m_Protocols;
		};

		void f_OnReceivedData();
		void f_OnSentData();

		void f_UpdateTimeout();
		void f_SetupTimeout();
		void f_StopTimeout();
		void f_OnTimeoutPongReceived();

		void f_ShutdownDone(NStr::CStr const &_Error);

		void f_HandleControlMessage(CMessage &_Message);
		void f_HandleDataMessage(CMessage &_Message);
		void f_SendMessage(EOpcode _Opcode, uint8 const *_pData, mint _nBytes, bool _bFinished);

		COutgoingMessage &f_QueueMessage(EOpcode _Opcode, NStorage::TCSharedPointer<NContainer::CSecureByteVector> const &_pData, uint32 _Priority);
		COutgoingMessage &f_QueueFragmentedMessage(EOpcode _Opcode, uint8 const *_pData, mint _nBytes, uint32 _Priority);
		void f_WriteQueuedMessages(EOpcode _UntilPriority = EOpcode_ContinuationFrame);
		static void fs_ApplyMask(uint8 *_pData, mint _iDataStart, mint _nBytes, uint8 const *_pMask);

		void f_RegisterPendingCallbacks();

		CWebSocketActor *m_pThis = nullptr;
		NStorage::TCUniquePointer<NNetwork::ICSocket> m_pSocket;
		NMib::NNetwork::CNetAddress m_PeerAddress;

		EState m_State = EState_None;

		NContainer::CPagedByteVector m_IncomingData;
		NContainer::CPagedByteVector m_OutgoingData;
		std::deque<COutgoingDataPromise> m_OutgoingDataPromises;

		NStorage::TCVariant<void, CConnectionInfo, CClientConnectionInfo> m_ConnectionInfo;
		CClientConnectionInput m_ClientConnectionInput;
		NStr::CStr m_Key;
		NStr::CStr m_Version;

		CMessage m_NextMessage;
		CMessage m_PendingMessage;
		CWebSocketActor::CCloseInfo m_CloseInfo;

		NContainer::TCMap<uint32, NContainer::TCLinkedList<COutgoingMessage>> m_PendingMessages;
		NContainer::TCLinkedList<COutgoingMessage> *m_pLastPendingMessagesList;

		NStorage::TCUniquePointer<NConcurrency::TCPromise<CWebSocketActor::CCloseInfo>> m_pClosePromise;
		NContainer::TCLinkedList<NFunction::TCFunctionMovable<void (NStr::CStr const &_Error)>> m_OnShutdown;

		NStorage::TCUniquePointer<CPendingCallbackRegister> m_pPendingCallbackRegister;
		NStorage::TCSharedPointer<NConcurrency::CCombinedCallbackReference> m_pPendingCombinedCallbackReferences;

		NConcurrency::TCActorSubscriptionManager<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _pMessage)> m_OnReceiveBinaryMessage;
		NConcurrency::TCActorSubscriptionManager<void (NStr::CStr const& _Message)> m_OnReceiveTextMessage;
		NConcurrency::TCActorSubscriptionManager<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _ApplicationData)> m_OnReceivePing;
		NConcurrency::TCActorSubscriptionManager<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _ApplicationData)> m_OnReceivePong;
		NConcurrency::TCActorSubscriptionManager<void (EWebSocketStatus _Status, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> m_OnClose;
		NConcurrency::TCActorSubscriptionManager<void (EFinishConnectionResult _Result, CConnectionInfo &&_ConnectionInfo)> m_OnFinishConnection;
		NConcurrency::TCActorSubscriptionManager<void (EFinishConnectionResult _Result, CClientConnectionInfo &&_ConnectionInfo)> m_OnFinishClientConnection;

		NConcurrency::CActorSubscription m_TimeoutTimerSubscription;
		NTime::CClock m_TimeoutReceivedData;
		NTime::CClock m_TimeoutSentData;
		NStorage::TCSharedPointer<NContainer::CSecureByteVector> m_pTimeoutPingMessage;
		fp64 m_Timeout = 0.0;
		mint m_TimeoutTimerSubscriptionSequence = 0;
		mint m_nSentBytes = 0;

		mint m_MaxMessageSize = 0;
		mint m_FramentationSize = 0;

		bool m_bPendingPing = false;
		bool m_bSentPing = false;

		bool m_bPendingMessage = false;
		bool m_bClient = false;
		bool m_bDebugNoProcessing = false;
		bool m_bOnCloseCalled = false;
		bool m_bOnFinishDone = false;
		bool m_bWantStopDefer = false;
		bool m_bShutdownCalled = false;
	};

	CWebSocketActor::CWebSocketActor(bool _bClient, mint _MaxMessageSize, mint _FragmentationSize, fp64 _Timeout)
		: mp_pInternal(fg_Construct(this, _bClient, _MaxMessageSize, _FragmentationSize, _Timeout))
	{
		auto &Internal = *mp_pInternal;
		Internal.f_SetupTimeout();
	}

	CWebSocketActor::~CWebSocketActor()
	{
	}

	NConcurrency::CActorSubscription CWebSocketActor::fp_SetCallbacks
		(
			NConcurrency::TCActor<NConcurrency::CActor> && _Actor
			, NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _pMessage)> && _fReceiveBinaryMessage
			, NFunction::TCFunctionMovable<void (NStr::CStr const& _Message)> && _fReceiveTextMessage
			, NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _ApplicationData)> && _fReceivePing
			, NFunction::TCFunctionMovable<void (NStorage::TCSharedPointer<NContainer::CSecureByteVector> const& _ApplicationData)> && _fReceivePong
			, NFunction::TCFunctionMovable<void (EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> && _fOnClose
		)
	{
		auto &Internal = *mp_pInternal;
		auto &Pending = *(Internal.m_pPendingCallbackRegister = fg_Construct());

		Pending.m_Actor = fg_Move(_Actor);
		Pending.m_fReceiveBinaryMessage = fg_Move(_fReceiveBinaryMessage);
		Pending.m_fReceiveTextMessage = fg_Move(_fReceiveTextMessage);
		Pending.m_fReceivePing = fg_Move(_fReceivePing);
		Pending.m_fReceivePong = fg_Move(_fReceivePong);
		Pending.m_fOnClose = fg_Move(_fOnClose);

		Internal.m_pPendingCombinedCallbackReferences = fg_Construct();

		class CDeferredCalbackReference : public NConcurrency::CActorSubscriptionReference
		{
		public:
			NConcurrency::TCFuture<void> f_Destroy() override
			{
				NConcurrency::TCPromise<void> Promise;
				if (!m_pCombinedReferences)
					return Promise <<= g_Void;
				auto pCombinedReferences = fg_Move(m_pCombinedReferences);
				pCombinedReferences->f_Destroy() > Promise;
				return Promise.f_MoveFuture();
			}

			NStorage::TCSharedPointer<NConcurrency::CCombinedCallbackReference> m_pCombinedReferences;
		};

		NStorage::TCUniquePointer<CDeferredCalbackReference> pReturn = fg_Construct();
		pReturn->m_pCombinedReferences = Internal.m_pPendingCombinedCallbackReferences;

		return fg_Move(pReturn);
	}

	COutgoingMessage &CWebSocketActor::CInternal::f_QueueMessage
		(
			EOpcode _Opcode
			, NStorage::TCSharedPointer<NContainer::CSecureByteVector> const &_pData
			, uint32 _Priority
		)
	{
		auto &NewMessage = m_PendingMessages[_Priority].f_Insert();
		NewMessage.m_pData = _pData;
		NewMessage.m_Opcode = _Opcode;
		NewMessage.m_bFinished = true;

		return NewMessage;

	}

	COutgoingMessage &CWebSocketActor::CInternal::f_QueueFragmentedMessage(EOpcode _Opcode, uint8 const *_pData, mint _nBytes, uint32 _Priority)
	{
		COutgoingMessage *pLastMessage = nullptr;
		uint8 const *pBytes = _pData;
		mint nBytes = _nBytes;
		EOpcode Opcode = _Opcode;
		while (true)
		{
			mint ThisTime = fg_Min(nBytes, m_FramentationSize);
			NContainer::CSecureByteVector VectorData;
			VectorData.f_Insert(pBytes, ThisTime);
			nBytes -= ThisTime;
			pBytes += ThisTime;
			auto &NewMessage = f_QueueMessage(Opcode, fg_Construct(fg_Move(VectorData)), _Priority);
			pLastMessage = &NewMessage;
			Opcode = EOpcode_ContinuationFrame;
			if (nBytes == 0)
				break;
			else
				NewMessage.m_bFinished = false;
		}

		return *pLastMessage;
	}

	void CWebSocketActor::CInternal::f_WriteQueuedMessages(EOpcode _UntilPriority)
	{
		if (m_PendingMessages.f_IsEmpty())
			return;

		mint OutgoingData = m_OutgoingData.f_GetLen();
		mint TargetData = 0;

		if (_UntilPriority == EOpcode_ContinuationFrame)
		{
			TargetData = m_OutgoingData.f_GetFirstPageSpace();
			if (TargetData < EOutgoingPageSize)
				TargetData += EOutgoingPageSize;

			if (OutgoingData >= TargetData)
				return;
		}

		auto pList = m_pLastPendingMessagesList;

		if (!pList)
		{
			pList = m_PendingMessages.f_FindLargest();
			DMibCheck(pList);
		}
		else
		{
			auto *pHighestPrioList = m_PendingMessages.f_FindLargest();
			if (m_PendingMessages.fs_GetKey(*pList) >= EOpcode_ConnectionClose)
				pList = pHighestPrioList;
		}

		if (m_PendingMessages.fs_GetKey(*pList) < _UntilPriority)
			return;

		DMibCheck(!pList->f_IsEmpty());

		auto *pPending = &pList->f_GetFirst();

		bool bFinished = false;

		while (OutgoingData < TargetData || _UntilPriority > EOpcode_ContinuationFrame)
		{
			bFinished = pPending->m_bFinished;

			f_SendMessage(pPending->m_Opcode, pPending->m_pData->f_GetArray(), pPending->m_pData->f_GetLen(), bFinished);
			OutgoingData = m_OutgoingData.f_GetLen();

			if (pPending->m_pPromise)
			{
				COutgoingDataPromise Promise;
				Promise.m_Position = m_nSentBytes + OutgoingData;
				Promise.m_pPromise = fg_Move(pPending->m_pPromise);
				m_OutgoingDataPromises.push_back(fg_Move(Promise));
			}

			pList->f_Remove(*pPending);
			if (pList->f_IsEmpty())
			{
				m_PendingMessages.f_Remove(pList);
				pList = m_PendingMessages.f_FindLargest();
				if (!pList)
				{
					pPending = nullptr;
					break;
				}
				if (m_PendingMessages.fs_GetKey(*pList) < _UntilPriority)
					break;
			}
			pPending = &pList->f_GetFirst();
		}

		if (bFinished)
			m_pLastPendingMessagesList = nullptr;
		else
			m_pLastPendingMessagesList = pList;
	}

	void CWebSocketActor::f_DebugStopProcessing(fp64 _Timeout)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_bDebugNoProcessing = true;
		Internal.m_Timeout = _Timeout;
		Internal.f_SetupTimeout();
	}

	NConcurrency::TCFuture<CWebSocketActor::CCloseInfo> CWebSocketActor::f_Close(EWebSocketStatus _Status, const NStr::CStr &_Reason)
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_pClosePromise)
			co_return DMibErrorInstance("Socket close already initiated");

		if (!Internal.m_pSocket || Internal.m_State == EState_Disconnected)
		{
			CWebSocketActor::CCloseInfo CloseInfo;
			CloseInfo.m_Status = EWebSocketStatus_AlreadyClosed;
			CloseInfo.m_Reason = "Already fully closed";
			co_return fg_Move(CloseInfo);
		}

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		auto CloseFuture = (Internal.m_pClosePromise = fg_Construct())->f_Future();

		fp_Disconnect(_Status, _Reason, false, EWebSocketCloseOrigin_Local);

		co_return co_await fg_Move(CloseFuture);
	}

	void CWebSocketActor::CInternal::f_ShutdownDone(NStr::CStr const &_Error)
	{
		for (auto &fOnShutdown : m_OnShutdown)
			fOnShutdown(_Error);
		m_OnShutdown.f_Clear();
	}

	NConcurrency::TCFuture<CWebSocketActor::CCloseInfo> CWebSocketActor::f_CloseWithLinger(EWebSocketStatus _Status, const NStr::CStr &_Reason, fp64 _MaxLingerTime)
	{
		{
			auto &Internal = *mp_pInternal;

			if (!Internal.m_pSocket || Internal.m_State == EState_Disconnected)
			{
				CWebSocketActor::CCloseInfo CloseInfo;
				CloseInfo.m_Status = EWebSocketStatus_AlreadyClosed;
				CloseInfo.m_Reason = "Already fully closed";
				co_return fg_Move(CloseInfo);
			}
		}

		auto ProcessingActor = NConcurrency::fg_ConcurrentActor();

		NConcurrency::TCPromise<CWebSocketActor::CCloseInfo> Promise;
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
					fg_Move(m_WebSocketActor).f_Destroy() > NConcurrency::fg_DiscardResult();
				}

				NConcurrency::TCActor<CWebSocketActor> m_WebSocketActor;
				NAtomic::TCAtomic<bool> m_bHandled;
			};

			NStorage::TCSharedPointer<CState> pState = fg_Construct();
			pState->m_WebSocketActor = fg_ThisActor(this);

			auto Cleanup = NConcurrency::g_OnScopeExitActor(ProcessingActor) > [pState, Promise]
				{
					if (!pState->m_bHandled.f_Exchange(true))
					{
						Promise.f_SetException(DMibErrorInstance("Websocket destroyed"));
						pState->f_Finish();
					}
				}
			;

			Internal.m_OnShutdown.f_Insert
				(
					[Cleanup, pState, Promise, this](NStr::CStr const &_Error)
					{
						auto &Internal = *mp_pInternal;
						if (!pState->m_bHandled.f_Exchange(true))
						{
							if (!_Error.f_IsEmpty())
								Promise.f_SetException(DMibErrorInstance(fg_Format("Unclean websocket shutdown: {}", _Error)));
							else
								Promise.f_SetResult(fg_Move(Internal.m_CloseInfo));
							pState->f_Finish();
						}
					}
				)
			;

			self(&CWebSocketActor::f_Close, _Status, _Reason) > ProcessingActor / [pState, Promise](NConcurrency::TCAsyncResult<NWeb::CWebSocketActor::CCloseInfo> &&_Result)
				{
					if (!_Result)
					{
						if (!pState->m_bHandled.f_Exchange(true))
						{
							Promise.f_SetException(fg_Move(_Result));
							pState->f_Finish();
						}
					}
				}
			;

			NConcurrency::fg_Timeout(_MaxLingerTime, false)(ProcessingActor) > [Promise, pState]
				{
					if (!pState->m_bHandled.f_Exchange(true))
					{
						Promise.f_SetException(DMibErrorInstance("Timed out waiting for websocket to close gracefully"));
						pState->f_Finish();
					}
				}
			;
		}

 		co_await fg_ContinueRunningOnActor(ProcessingActor);

		co_return co_await Promise.f_MoveFuture();
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendBinary(NStorage::TCSharedPointer<NContainer::CSecureByteVector> const &_pMessage, uint32 _Priority)
	{
		auto &Internal = *mp_pInternal;
		DMibLog(DebugVerbose2, " ++++ {} f_SendBinary", !Internal.m_bClient);

		auto &Massage = *_pMessage;
		mint nBytes = Massage.f_GetLen();

		if (nBytes > Internal.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		if (nBytes <= Internal.m_FramentationSize)
		{
			auto &NewMessage = Internal.f_QueueMessage(EOpcode_BinaryFrame, _pMessage, _Priority);
			auto Future = (NewMessage.m_pPromise = fg_Construct())->f_Future();
			DMibLog(DebugVerbose2, " ++++ {} Queue non-fragmented", !Internal.m_bClient);
			fp_UpdateSend();
			co_return co_await fg_Move(Future);
		}

		auto Future = (Internal.f_QueueFragmentedMessage(EOpcode_BinaryFrame, Massage.f_GetArray(), nBytes, _Priority).m_pPromise = fg_Construct())->f_Future();

		DMibLog(DebugVerbose2, " ++++ {} Queue fragmented", !Internal.m_bClient);
		fp_UpdateSend();

		co_return co_await fg_Move(Future);
	}

	void CWebSocketActor::fp_StateAdded(NNetwork::ENetTCPState _StateAdded)
	{
		fp_ProcessState(_StateAdded);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendText(NStr::CStr const& _Data, uint32 _Priority)
	{
		auto &Internal = *mp_pInternal;

		NStr::CStr Data = _Data;

		mint nBytes = Data.f_GetLen();

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		if (nBytes > Internal.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		auto Future = (Internal.f_QueueFragmentedMessage(EOpcode_TextFrame, (uint8 const *)Data.f_GetStr(), nBytes, _Priority).m_pPromise = fg_Construct())->f_Future();

		fp_UpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendTextBuffer(NStorage::TCSharedPointer<CMaybeSecureByteVector> const &_pMessage, uint32 _Priority)
	{
		auto &Internal = *mp_pInternal;

		auto &Message = *_pMessage;

		mint nBytes = Message.f_GetLen();

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		if (nBytes > Internal.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		auto Future = (Internal.f_QueueFragmentedMessage(EOpcode_TextFrame, Message.f_GetArray(), nBytes, _Priority).m_pPromise = fg_Construct())->f_Future();

		fp_UpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendTextBuffers(NStorage::TCSharedPointer<CMessageBuffers> const &_pMessageBuffers, uint32 _Priority)
	{
		auto &Internal = *mp_pInternal;

		if (_Priority == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("0xffffffff priority is reserved for internal messages");

		auto &MessageBuffers = *_pMessageBuffers;

		auto &MessageMarkers = MessageBuffers.m_Markers;
		auto &Message = MessageBuffers.m_Data;
		mint const *pMessageMarkersArray = MessageMarkers.f_GetArray();
		mint nMessages = MessageMarkers.f_GetLen();
		if (!nMessages)
			co_return {};

		mint MessageLength = Message.f_GetLen();
		uint8 const *pMessageArray = Message.f_GetArray();

		for (mint iMessage = 0; iMessage < nMessages; ++iMessage)
		{
			mint iStart = pMessageMarkersArray[iMessage];
			mint iEnd = iMessage == (nMessages - 1) ? MessageLength : pMessageMarkersArray[iMessage + 1];
			mint nBytes = nBytes = iEnd - iStart;

			if (nBytes > Internal.m_MaxMessageSize)
				co_return DMibErrorInstance("Message is bigger than max message size");
		}

		NConcurrency::TCFuture<void> Future;

		for (mint iMessage = 0; iMessage < nMessages; ++iMessage)
		{
			bool bIsLastMessage = iMessage == (nMessages - 1);

			mint iStart = pMessageMarkersArray[iMessage];
			mint iEnd = bIsLastMessage ? MessageLength : pMessageMarkersArray[iMessage + 1];
			mint nBytes = nBytes = iEnd - iStart;

			if (bIsLastMessage)
				nBytes = MessageLength - iStart;
			else
				nBytes = pMessageMarkersArray[iMessage + 1] - iStart;

			auto &OutMsg = Internal.f_QueueFragmentedMessage(EOpcode_TextFrame, pMessageArray + iStart, nBytes, _Priority);

			if (bIsLastMessage)
			{
				// OK, assuming messages are sent in the order they are queued attaching the promise to the last message should
				// behave as assumed.
				Future = (OutMsg.m_pPromise = fg_Construct())->f_Future();
			}
		}

		fp_UpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendPing(NStorage::TCSharedPointer<NContainer::CSecureByteVector> _ApplicationData)
	{
		auto &Internal = *mp_pInternal;
		mint nBytes = _ApplicationData->f_GetLen();
		if (nBytes > Internal.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		auto &NewMessage = Internal.f_QueueMessage(EOpcode_Ping, _ApplicationData, TCLimitsInt<uint32>::mc_Max);
		auto Future = (NewMessage.m_pPromise = fg_Construct())->f_Future();

		fp_UpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	NConcurrency::TCFuture<void> CWebSocketActor::f_SendPong(NStorage::TCSharedPointer<NContainer::CSecureByteVector> _ApplicationData)
	{
		auto &Internal = *mp_pInternal;
		mint nBytes = _ApplicationData->f_GetLen();
		if (nBytes > Internal.m_MaxMessageSize)
			co_return DMibErrorInstance("Message is bigger than max message size");

		auto &NewMessage = Internal.f_QueueMessage(EOpcode_Pong, _ApplicationData, TCLimitsInt<uint32>::mc_Max);
		auto Future = (NewMessage.m_pPromise = fg_Construct())->f_Future();

		fp_UpdateSend();

		co_await NConcurrency::ECoroutineFlag_BreakSelfReference;

		co_return co_await fg_Move(Future);
	}

	void CWebSocketActor::CInternal::f_SendMessage(EOpcode _Opcode, uint8 const *_pData, mint _nBytes, bool _bFinished)
	{
		CBinaryStreamPagedByteVector Stream(m_OutgoingData);

		bool bMask = m_bClient;

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

		mint StartPos = m_OutgoingData.f_GetLen();
		m_OutgoingData.f_InsertBack(_pData, _nBytes);

		if (bMask)
		{
			m_OutgoingData.f_Mutate
				(
					StartPos
					, _nBytes
					, [&](mint _iStart, uint8 * _pPtr, mint _nBytes) -> bool
					{
						fs_ApplyMask(_pPtr, _iStart - StartPos, _nBytes, Mask);
						return true;
					}
				)
			;
		}
	}

	void CWebSocketActor::fp_Disconnect(EWebSocketStatus _Status, NStr::CStr const &_Reason, bool _bFatal, EWebSocketCloseOrigin _Origin)
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_State == EState_Disconnected)
		{
			if (_bFatal)
			{
				Internal.m_pSocket.f_Clear();
				Internal.f_ShutdownDone(_Reason);
			}
			return; // Already disconnected
		}

		if (Internal.m_State == EState_Connected || Internal.m_State == EState_Disconnecting)
		{
			auto WasState = Internal.m_State;
			if (!_bFatal && Internal.m_State == EState_Connected)
			{
				// Send packet to other side

				if (_Status != EWebSocketStatus_NoStatusReceived)
				{
					NStream::CBinaryStreamMemory<NStream::CBinaryStreamBigEndian> Stream;
					Stream << uint16(_Status);
					NStr::CStr Reason = _Reason;
					Stream.f_FeedBytes(Reason.f_GetStr(), Reason.f_GetLen());

					auto Data = Stream.f_MoveVector();
					Internal.f_SendMessage(EOpcode_ConnectionClose, Data.f_GetArray(), Data.f_GetLen(), true);
				}
				else
					Internal.f_SendMessage(EOpcode_ConnectionClose, nullptr, 0, true);

				Internal.m_State = EState_Disconnecting;
				fp_UpdateSend();
			}
			if (_Origin == EWebSocketCloseOrigin_Remote)
			{
				Internal.m_CloseInfo.m_Status = _Status;
				Internal.m_CloseInfo.m_Reason = _Reason;
				if (Internal.m_pClosePromise)
				{
					Internal.m_pClosePromise->f_SetResult(Internal.m_CloseInfo);
					Internal.m_pClosePromise.f_Clear();
				}
				if (!Internal.m_bOnCloseCalled)
				{
					Internal.m_bOnCloseCalled = true;
					if (!Internal.m_OnClose.f_IsEmpty() || Internal.m_OnClose.f_IsDeferring())
						Internal.m_OnClose(_Status, _Reason, _Origin) > NConcurrency::fg_DiscardResult();
				}

				if (!_bFatal)
				{
					if (WasState == EState_Connected)
					{
						Internal.m_State = EState_Disconnected;
						Internal.f_StopTimeout();
						auto *pHighestPrioMessages = Internal.m_PendingMessages.f_FindLargest();
						if (!pHighestPrioMessages || Internal.m_PendingMessages.fs_GetKey(*pHighestPrioMessages) < EOpcode_ConnectionClose)
							fp_Shutdown();
					}
					else if (WasState == EState_Disconnecting)
					{
						Internal.m_State = EState_Disconnected;
						Internal.f_StopTimeout();
						fp_Shutdown();
					}
					return;
				}
			}
			else if (!_bFatal)
			{
				return;
			}
		}
		else
		{
			if (Internal.m_bClient)
			{
				auto &ConnectionInfo = Internal.m_ConnectionInfo.f_GetAsType<CClientConnectionInfo>();
				if (Internal.m_pSocket)
					ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
				ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;
				ConnectionInfo.m_ErrorStatus = _Status;
				ConnectionInfo.m_Error = _Reason;
				Internal.m_OnFinishClientConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo)) > NConcurrency::fg_DiscardResult();
			}
			else
			{
				auto &ConnectionInfo = Internal.m_ConnectionInfo.f_GetAsType<CConnectionInfo>();
				if (Internal.m_pSocket)
					ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
				ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;
				ConnectionInfo.m_ErrorStatus = _Status;
				ConnectionInfo.m_Error = _Reason;
				Internal.m_OnFinishConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo)) > NConcurrency::fg_DiscardResult();
			}
		}

		if (_bFatal)
		{
			Internal.m_CloseInfo.m_Status = _Status;
			Internal.m_CloseInfo.m_Reason = fg_Format("Abnormal closure: {}", _Reason);
			if (Internal.m_pClosePromise)
			{
				Internal.m_pClosePromise->f_SetResult(Internal.m_CloseInfo);
				Internal.m_pClosePromise.f_Clear();
			}
			if (!Internal.m_bOnCloseCalled)
			{
				Internal.m_bOnCloseCalled = true;
				Internal.m_OnClose(_Status, _Reason, _Origin) > NConcurrency::fg_DiscardResult();
			}

			Internal.m_pSocket.f_Clear();
			Internal.f_ShutdownDone(_Reason);
		}

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

		if (Internal.m_State == EState_Connected)
			Internal.f_WriteQueuedMessages();
		else if (Internal.m_State == EState_Disconnecting)
			Internal.f_WriteQueuedMessages(EOpcode_ConnectionClose);

		if (Internal.m_bDebugNoProcessing)
			return;

		bool bDidSend = false;
		while (!Internal.m_OutgoingData.f_IsEmpty() && Internal.m_pSocket->f_IsValid())
		{
			mint SentBytes = 0;
			bool bStuffed = false;
			bool bDisconnected = false;
			NNetwork::CSocketOperationResult CombinedResults;
			Internal.m_OutgoingData.f_ReadFront
				(
					[&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bool
					{
						try
						{
							bDidSend = true;
							NNetwork::CSocketOperationResult Result = Internal.m_pSocket->f_Send(_pPtr, _nBytes);
							DMibLog(DebugVerbose2, " ++++ {} Sending {} resulted in {} sent", !Internal.m_bClient, _nBytes, Result.m_nBytes);

							CombinedResults += Result;

							SentBytes += Result.m_nBytes;
							if (Result.m_nBytes != _nBytes)
							{
								bStuffed = true;
								return false;
							}
							return true;
						}
						catch (NCryptography::CExceptionCryptography const &_Error)
						{
							fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket exception: {}", _Error.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
							bDisconnected = true;
							return false;
						}
						catch (NNetwork::CExceptionNet const &_Error)
						{
							fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket exception: {}", _Error.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
							bDisconnected = true;
							return false;
						}
					}
				)
			;
			if (CombinedResults.m_bSentNetwork)
				Internal.f_OnSentData();
			if (CombinedResults.m_bReceivedNetwork)
				Internal.f_OnReceivedData();

			mint PrevSent = Internal.m_nSentBytes;
			Internal.m_nSentBytes += SentBytes;

			while (!Internal.m_OutgoingDataPromises.empty())
			{
				auto &Promise = Internal.m_OutgoingDataPromises.front();
				mint Diff = Promise.m_Position - PrevSent;
				if (Diff <= SentBytes)
				{
					Promise.m_pPromise->f_SetResult();
					Promise.m_pPromise.f_Clear();
					Internal.m_OutgoingDataPromises.pop_front();
					continue;
				}
				break;
			}

			Internal.m_OutgoingData.f_RemoveFront(SentBytes);
			if (bDisconnected)
				break;
			if (bStuffed)
				break;
			if (Internal.m_State == EState_Connected)
				Internal.f_WriteQueuedMessages();
			else if (Internal.m_State == EState_Disconnecting)
				Internal.f_WriteQueuedMessages(EOpcode_ConnectionClose);
		}

		if (!bDidSend && Internal.m_pSocket && Internal.m_pSocket->f_IsValid())
		{
			NNetwork::CSocketOperationResult SendResult = Internal.m_pSocket->f_Send(nullptr, 0);
			if (SendResult.m_bSentNetwork)
				Internal.f_OnSentData();
			if (SendResult.m_bReceivedNetwork)
				Internal.f_OnReceivedData();
		}

		if (Internal.m_State == EState_Disconnected && Internal.m_OutgoingData.f_IsEmpty())
			fp_Shutdown();
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

	void CWebSocketActor::CInternal::fs_ApplyMask(uint8 *_pData, mint _iDataStart, mint _nBytes, uint8 const *_pMask)
	{
		uint8 DoubleMask[8];
		NMemory::fg_MemCopy(DoubleMask, _pMask, 4);
		NMemory::fg_MemCopy(DoubleMask + 4, _pMask, 4);

		uint8 *pCurrent = _pData;
		uint8 *pEnd = _pData + _nBytes;
		uint8 *pAlignedStart = fg_AlignUp(_pData, 4);
		uint8 *pAlignedEnd = fg_AlignDown(pEnd, 4);

		for (mint i = _iDataStart % 4; pCurrent < pAlignedStart; pCurrent += 1)
		{
			*pCurrent ^= DoubleMask[i];
			++i;
		}

		mint MaskOffset = (_iDataStart + (pAlignedStart - _pData)) % 4;

		uint32 AlignedMask = 0;
		NMemory::fg_MemCopy(&AlignedMask, DoubleMask + MaskOffset, sizeof(uint32));

#ifdef DEnableVector
		uint8 *pAlignedVectorStart = fg_AlignUp(_pData, 16);
		uint8 *pAlignedVectorEnd = fg_AlignDown(pEnd, 16);

		for (; pCurrent < pAlignedVectorStart; pCurrent += 4)
		{
			*((uint32 *)pCurrent) ^= AlignedMask;
		}

		vec4uint32 VectorMask = {AlignedMask, AlignedMask, AlignedMask, AlignedMask};
		for (; pCurrent < pAlignedVectorEnd; pCurrent += 16)
		{
			*((vec4uint32 *)pCurrent) ^= VectorMask;
		}
#endif

		for (; pCurrent < pAlignedEnd; pCurrent += 4)
		{
			*((uint32 *)pCurrent) ^= AlignedMask;
		}
		for (mint i = 0; pCurrent < pEnd; pCurrent += 1)
		{
			*pCurrent ^= DoubleMask[MaskOffset + i];
			++i;
		}
	}

	bool CWebSocketActor::fp_ProcessIncomingMessage()
	{
		auto &Internal = *mp_pInternal;
		DMibLog(DebugVerbose2, " ++++ {} fp_ProcessIncomingMessage", !Internal.m_bClient);

		auto &Message = Internal.m_NextMessage;
		CHeader &Header = Message.m_Header;
		uint64 &Length = Message.m_Length;
		uint8 *Mask = Message.m_Mask;

		if (!Message.m_bHeaderFinished)
		{
			mint ThisPosition = 0;
			mint &Position = Message.m_Position;
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
							, [&](mint _iStart, uint8 const *_pData, mint _nBytes) -> bool
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
							, [&](mint _iStart, uint8 const *_pData, mint _nBytes) -> bool
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
							, [&](mint _iStart, uint8 const *_pData, mint _nBytes) -> bool
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

			if (Length > uint64(Internal.m_MaxMessageSize))
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
							, [&](mint _iStart, uint8 const *_pData, mint _nBytes) -> bool
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
			else if (!Internal.m_bClient)
			{
				fp_Disconnect(EWebSocketStatus_ProtocolError, "Client sent unmasked frame", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			Internal.m_IncomingData.f_RemoveFront(Position);
			Message.m_bHeaderFinished = true;
		}

		mint nBytesAvailable = Internal.m_IncomingData.f_GetLen();

		if (nBytesAvailable < Length)
			return false; // Message not finished

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

		uint8 *pMaskStart;
		if (Internal.m_bPendingMessage && !bControlMessage)
		{
			mint iStart = Internal.m_PendingMessage.m_Data.f_GetLen();
			if (iStart + Length > uint64(Internal.m_MaxMessageSize))
			{
				fp_Disconnect(EWebSocketStatus_MessageTooBig, "Unsupported message length", false, EWebSocketCloseOrigin_Local);
				return false;
			}

			Internal.m_IncomingData.f_ReadFront
				(
					Length
					, [&](mint _iStart, uint8 const *_pData, mint _nBytes) -> bool
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
					, [&](mint _iStart, uint8 const *_pData, mint _nBytes) -> bool
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

					mint Len = Stream.f_GetLength() - 2;
					ch8 *pData = Reason.f_GetStr(Len);
					Stream.f_ConsumeBytes(pData, Len);
					pData[Len] = 0;
					Reason.f_SetStrLen(-1);
				}

				// TODO: Send reponse frame
				m_pThis->fp_Disconnect(Status, Reason, false, EWebSocketCloseOrigin_Remote);
			}
			break;
		case EOpcode_Ping:
			{
				// RFC 6455 - 5.5.2.
				if (m_OnReceivePing.f_IsEmpty() || (m_pTimeoutPingMessage && _Message.m_Data == *m_pTimeoutPingMessage))
				{
					// We reply automatically as quickly as possible
					f_SendMessage(EOpcode_Pong, _Message.m_Data.f_GetArray(), _Message.m_Data.f_GetLen(), true);
					m_pThis->fp_UpdateSend();
				}
				else
				{
					// Otherwise we let the application reply
					m_OnReceivePing(fg_Construct(fg_Move(_Message.m_Data))) > NConcurrency::fg_DiscardResult();
				}
			}
			break;
		case EOpcode_Pong:
			{
				// RFC 6455 - 5.5.3.
				if (m_pTimeoutPingMessage && _Message.m_Data == *m_pTimeoutPingMessage)
					f_OnTimeoutPongReceived();
				else if (!m_OnReceivePong.f_IsEmpty())
					m_OnReceivePong(fg_Construct(fg_Move(_Message.m_Data))) > NConcurrency::fg_DiscardResult();
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
		DMibLog(DebugVerbose2, " ++++ {} f_HandleDataMessage", !m_bClient);
		switch (_Message.m_Header.m_Opcode)
		{
		case EOpcode_TextFrame:
			{
				NStr::CStr Data;
				Data.f_AddStr(_Message.m_Data.f_GetArray(), _Message.m_Data.f_GetLen());
 				if (!m_OnReceiveTextMessage.f_IsEmpty() || m_OnReceiveTextMessage.f_IsDeferring())
					m_OnReceiveTextMessage(fg_Move(Data)) > NConcurrency::fg_DiscardResult();
			}
			break;
		case EOpcode_BinaryFrame:
			{
				DMibLog(DebugVerbose2, " ++++ {} call m_OnReceiveBinaryMessage", !m_bClient);
 				if (!m_OnReceiveBinaryMessage.f_IsEmpty() || m_OnReceiveBinaryMessage.f_IsDeferring())
					m_OnReceiveBinaryMessage(fg_Construct(fg_Move(_Message.m_Data))) > NConcurrency::fg_DiscardResult();
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

								if (GeneralFields.f_GetUpgrade() != "websocket")
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

									NContainer::CSecureByteVector DigestData;
									DigestData.f_Insert(Digest.f_GetData(), Digest.fs_GetSize());

									NStr::CStr CorrectKey = NEncoding::fg_Base64Encode(DigestData);

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
								if (Internal.m_pSocket)
									ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
								ConnectionInfo.m_PeerAddress = Internal.m_PeerAddress;
								Internal.m_State = EState_Connected;
								Internal.m_OnFinishClientConnection(EFinishConnectionResult_Success, fg_Move(ConnectionInfo)) > NConcurrency::fg_DiscardResult();
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
									NEncoding::CJSON Reply;
									Reply["websocket"] = true;
									Reply["origins"].f_Array().f_Insert("*:*");
									Reply["cookie_needed"] = false;
									Reply["entropy"] = NMisc::fg_GetRandomUnsigned();
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

								Internal.m_OnFinishConnection(EFinishConnectionResult_Success, fg_Move(ConnectionInfo)) > NConcurrency::fg_DiscardResult();
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
		if (Internal.m_OnReceiveBinaryMessage.f_IsEmpty())
			Internal.m_OnReceiveBinaryMessage.f_StopDeferring();
		if (Internal.m_OnReceiveTextMessage.f_IsEmpty())
			Internal.m_OnReceiveTextMessage.f_StopDeferring();
		if (Internal.m_OnReceivePing.f_IsEmpty())
			Internal.m_OnReceivePing.f_StopDeferring();
		if (Internal.m_OnReceivePong.f_IsEmpty())
			Internal.m_OnReceivePong.f_StopDeferring();
		if (Internal.m_OnClose.f_IsEmpty())
			Internal.m_OnClose.f_StopDeferring();
		if (Internal.m_OnFinishConnection.f_IsEmpty())
			Internal.m_OnFinishConnection.f_StopDeferring();
		if (Internal.m_OnFinishClientConnection.f_IsEmpty())
			Internal.m_OnFinishClientConnection.f_StopDeferring();
	}

	void CWebSocketActor::fp_AcceptClientConnection()
	{
		auto &Internal = *mp_pInternal;
		Internal.f_RegisterPendingCallbacks();
		fp_TryStopDeferring();
	}

	void CWebSocketActor::fp_RejectClientConnection(NStr::CStr const &_Error)
	{
		fp_TryStopDeferring();

		fp_Disconnect(EWebSocketStatus_Rejected, NStr::fg_Format("Rejected connection: {}", _Error), false, EWebSocketCloseOrigin_Local);
	}

	void CWebSocketActor::CInternal::f_RegisterPendingCallbacks()
	{
		DMibRequire(m_pPendingCombinedCallbackReferences);
		DMibRequire(m_pPendingCallbackRegister);

		auto &Callbacks = *m_pPendingCallbackRegister;
		auto &References = *m_pPendingCombinedCallbackReferences;
		if (Callbacks.m_fReceiveBinaryMessage)
			References.m_References.f_Insert(m_OnReceiveBinaryMessage.f_Register(Callbacks.m_Actor, fg_Move(Callbacks.m_fReceiveBinaryMessage)));
		if (Callbacks.m_fReceiveTextMessage)
			References.m_References.f_Insert(m_OnReceiveTextMessage.f_Register(Callbacks.m_Actor, fg_Move(Callbacks.m_fReceiveTextMessage)));
		if (Callbacks.m_fReceivePing)
			References.m_References.f_Insert(m_OnReceivePing.f_Register(Callbacks.m_Actor, fg_Move(Callbacks.m_fReceivePing)));
		if (Callbacks.m_fReceivePong)
			References.m_References.f_Insert(m_OnReceivePong.f_Register(Callbacks.m_Actor, fg_Move(Callbacks.m_fReceivePong)));
		if (Callbacks.m_fOnClose)
			References.m_References.f_Insert(m_OnClose.f_Register(Callbacks.m_Actor, fg_Move(Callbacks.m_fOnClose)));

		m_pPendingCombinedCallbackReferences.f_Clear();
		m_pPendingCallbackRegister.f_Clear();
	}

	void CWebSocketActor::fp_AcceptServerConnection(NStr::CStr const &_Protocol, NHTTP::CResponseHeader &&_ResponseHeader)
	{
		auto &Internal = *mp_pInternal;
		Internal.f_RegisterPendingCallbacks();
		fp_TryStopDeferring();

		NHTTP::CResponseHeader Response = fg_Move(_ResponseHeader);

		Response.f_SetOutputMethod
			(
				[&](uint8 const *_pData, mint _nBytes)
				{
					Internal.m_OutgoingData.f_InsertBack(_pData, _nBytes);
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

		NCryptography::CHash_SHA1 Hash;
		Hash.f_AddData(Internal.m_Key.f_GetStr(), Internal.m_Key.f_GetLen());
		Hash.f_AddData("258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
		NCryptography::CHash_SHA1::CMessageDigest Digest = Hash;

		NContainer::CSecureByteVector DigestData;
		DigestData.f_Insert(Digest.f_GetData(), Digest.fs_GetSize());

		EntityFields.f_SetUnknownField("Sec-WebSocket-Accept", NEncoding::fg_Base64Encode(DigestData));

		Response.f_Complete();

		Internal.m_State = EState_Connected;

		fp_UpdateSend();
	}

	void CWebSocketActor::fp_RejectServerConnection(NStr::CStr const &_Error, NHTTP::CResponseHeader &&_ResponseHeader, NStr::CStr const &_Content)
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
				[&](uint8 const *_pData, mint _nBytes)
				{
					Internal.m_OutgoingData.f_InsertBack(_pData, _nBytes);
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

		if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
			return;

		if (_StateAdded & NNetwork::ENetTCPState_Closed)
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

		if ((_StateAdded & NNetwork::ENetTCPState_Read) && !Internal.m_bDebugNoProcessing)
		{
			NNetwork::CSocketOperationResult CombinedResults;
			uint8 Data[4096];
			try
			{
				while (true)
				{
					mint Size = 4096;
					NNetwork::CSocketOperationResult Result = Internal.m_pSocket->f_Receive(Data, Size);
					CombinedResults += Result;
					if (Result.m_nBytes == 0 && !Result.m_bSentNetwork && !Result.m_bReceivedNetwork)
						break;
					DMibLog(DebugVerbose2, " ++++ {} Received data {}", !Internal.m_bClient, Result.m_nBytes);
					Internal.m_IncomingData.f_InsertBack(Data, Result.m_nBytes);
					fp_ProcessIncoming();
					if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
						return;
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

		if (_StateAdded & NNetwork::ENetTCPState_RemoteClosed)
		{
			if (Internal.m_State <= EState_Connected)
			{
				if (Internal.m_State == EState_Connected)
					fp_Disconnect(Internal.m_CloseInfo.m_Status == EWebSocketStatus_None ? EWebSocketStatus_AbnormalClosure : EWebSocketStatus_NormalClosure, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason()), false, EWebSocketCloseOrigin_Remote);
				else
					fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason()), true, EWebSocketCloseOrigin_Remote);
			}
		}

		if ((_StateAdded & NNetwork::ENetTCPState_Write) && !Internal.m_bDebugNoProcessing)
			fp_UpdateSend();
	}

	void CWebSocketActor::fp_SetSocket(NStorage::TCUniquePointer<NNetwork::ICSocket> && _pSocket)
	{
		auto &Internal = *mp_pInternal;

		DMibFastCheck(!Internal.m_pSocket);
		Internal.m_pSocket = fg_Move(_pSocket);

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
			NConcurrency::TCActor<NConcurrency::CActor> &&_Actor
			, NFunction::TCFunctionMovable<void (EFinishConnectionResult _Result, CConnectionInfo &&_ConnectionInfo)> &&_fOnFinishConnection
		)
	{
		auto &Internal = *mp_pInternal;
		auto Return = Internal.m_OnFinishConnection.f_Register(fg_Move(_Actor), fg_Move(_fOnFinishConnection));

		if (Internal.m_bWantStopDefer)
			fp_StopDeferring();

		return Return;
	}

	NConcurrency::CActorSubscription CWebSocketActor::fp_OnFinishClientConnection
		(
			NConcurrency::TCActor<NConcurrency::CActor> &&_Actor
			, NFunction::TCFunctionMovable<void (EFinishConnectionResult _Result, CClientConnectionInfo &&_ConnectionInfo)> &&_fOnFinishConnection
			, NHTTP::CRequest &&_RequestHeader
			, NStr::CStr const &_ConnectToAddress
			, NStr::CStr const &_URI
			, NStr::CStr const &_Origin
			, NContainer::TCVector<NStr::CStr> const &_Protocols
		)
	{
		auto &Internal = *mp_pInternal;
		auto Return = Internal.m_OnFinishClientConnection.f_Register(fg_Move(_Actor), fg_Move(_fOnFinishConnection));

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

		NContainer::CSecureByteVector RandomData;
		RandomData.f_SetLen(16);
		NCryptography::fg_GenerateRandomData(RandomData.f_GetArray(), RandomData.f_GetLen());

		NStr::CStr EncodedRandomData = NEncoding::fg_Base64Encode(RandomData);

		if (!Protocols.f_IsEmpty())
			EntityFields.f_SetUnknownField("Sec-WebSocket-Protocol", Protocols);
		EntityFields.f_SetUnknownField("Sec-WebSocket-Version", "13");
		EntityFields.f_SetUnknownField("Sec-WebSocket-Key", EncodedRandomData);

		for (auto &Protocol : _Protocols)
			Internal.m_ClientConnectionInput.m_Protocols[Protocol];
		Internal.m_ClientConnectionInput.m_EncodedKey = EncodedRandomData;

		NContainer::CSecureByteVector SendData;
		_RequestHeader.f_WriteHeaders
			(
				[&](uint8 const *_pData, mint _nBytes)
				{
					Internal.m_OutgoingData.f_InsertBack(_pData, _nBytes);
				}
			)
		;
		fp_UpdateSend();

		return Return;
	}

	void CWebSocketActor::f_SetTimeout(fp64 _Seconds)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_Timeout = _Seconds;
		Internal.f_SetupTimeout();
	}

	void CWebSocketActor::CInternal::f_StopTimeout()
	{
		m_TimeoutTimerSubscription.f_Clear();
		m_pTimeoutPingMessage.f_Clear();
	}

	void CWebSocketActor::CInternal::f_SetupTimeout()
	{
		f_StopTimeout();
		if (m_Timeout == 0.0)
			return; // Timeout disabled

		m_TimeoutReceivedData.f_Start();
		m_TimeoutSentData.f_Start();

		m_pTimeoutPingMessage = fg_Construct();
		mint MessageSize = NStr::fg_StrLen(gs_PingMessageData);
		m_pTimeoutPingMessage->f_SetLen(MessageSize); // TCVector has 16 as min size
		NMemory::fg_MemCopy(m_pTimeoutPingMessage->f_GetArray(), gs_PingMessageData, MessageSize);

		auto Sequence = ++m_TimeoutTimerSubscriptionSequence;
		fg_RegisterTimer
			(
				m_Timeout/2.0
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
				m_pThis->f_SendPing(m_pTimeoutPingMessage)
					> NConcurrency::NPrivate::fg_DirectResultActor() / [this, ThisWeak = fg_ThisActor(m_pThis).f_Weak()](NConcurrency::TCAsyncResult<void> &&_Result) mutable
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
							> NConcurrency::fg_DiscardResult()
						;
					}
				;
			}

			if (m_bSentPing)
			{
				if (m_TimeoutReceivedData.f_GetTime() > m_Timeout)
					m_pThis->fp_Disconnect(EWebSocketStatus_Timeout, NStr::fg_Format("Timeout({}) receiving data", m_Timeout), true, EWebSocketCloseOrigin_Local);
			}

			if (!m_OutgoingData.f_IsEmpty())
			{
				if (m_TimeoutSentData.f_GetTime() > m_Timeout)
					m_pThis->fp_Disconnect(EWebSocketStatus_Timeout, NStr::fg_Format("Timeout({}) sending data", m_Timeout), true, EWebSocketCloseOrigin_Local);
			}
		}
		else if (m_State != EState_Disconnected)
		{
			NNetwork::ENetTCPState State = NNetwork::ENetTCPState_None;
			if (m_pSocket && m_pSocket->f_IsValid())
				State = m_pSocket->f_GetState();
			if (State)
				m_pThis->fp_ProcessState(State);

			if (m_TimeoutReceivedData.f_GetTime() > m_Timeout && m_TimeoutSentData.f_GetTime() > m_Timeout)
				m_pThis->fp_Disconnect(EWebSocketStatus_Timeout, NStr::fg_Format("Timeout({}) in non-connected state", m_Timeout), true, EWebSocketCloseOrigin_Local);
		}
	}
}
