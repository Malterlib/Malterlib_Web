// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorCallbackManager>

#include <Mib/Web/HTTP/Request>
#include <Mib/Web/HTTP/Response>

#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Cryptography/RandomData>
#include <Mib/Encoding/Base64>

#include <Mib/Encoding/JSON>

#include "Malterlib_Web_WebSocket.h"

#if defined(DCompiler_clang) && !defined(DPlatformFamily_Emscripten)
#	define DEnableVector
#endif

#ifdef DEnableVector
typedef uint32 vec4uint32 __attribute__((ext_vector_type(4)));
#endif


namespace NMib
{
	namespace NWeb
	{
		typedef NHTTP::TCBinaryStreamPagedByteVector<NStream::CBinaryStreamBigEndian> CBinaryStreamPagedByteVector;
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
					NMem::fg_MemClear(m_Mask); // MSVC does not support inline initializing of array
				}
				uint64 m_Length = 0;
				NContainer::TCVector<uint8> m_Data;
				mint m_Position = 0;
				uint8 m_Mask[4];
				CHeader m_Header;
				bool m_bHeaderFinished = false;
			};
			
			struct COutgoingMessage
			{
				NPtr::TCSharedPointer<NContainer::TCVector<uint8>> m_pData;
				EOpcode m_Opcode;
				NPtr::TCUniquePointer<NConcurrency::TCContinuation<void>> m_pContinuation;
				zbool m_bFinished;
			};
		}
		
		struct CWebSocketActor::CInternal
		{
			CInternal(CWebSocketActor *_pThis, bool _bClient, mint _MaxMessageSize, mint _FragmentationSize)
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
				, m_Framentationsize(_FragmentationSize)
				, m_pLastPendingMessagesList(nullptr)
			{
				if (_bClient)
					m_ConnectionInfo.f_Set<2>();
				else
					m_ConnectionInfo.f_Set<1>();
			}		
			
			~CInternal()
			{
			}


			struct CClientConnectionInput
			{
				NStr::CStr m_EncodedKey;
				NContainer::TCSet<NStr::CStr> m_Protocols;
			};

		public:
			CWebSocketActor *m_pThis;
			NPtr::TCUniquePointer<NNet::ICSocket> m_pSocket;

			EState m_State = EState_None;
			
			NHTTP::CPagedByteVector m_IncomingData;
			NHTTP::CPagedByteVector m_OutgoingData;
			
			NContainer::TCVariant<void, CConnectionInfo, CClientConnectionInfo> m_ConnectionInfo;
			CClientConnectionInput m_ClientConnectionInput;
			NStr::CStr m_Key;
			NStr::CStr m_Version;
			
			CMessage m_NextMessage;
			CMessage m_PendingMessage;
			mint m_MaxMessageSize;
			mint m_Framentationsize;
			zbool m_bPendingMessage;
			bool m_bClient;
			bool m_bOnFinishDone = false;
			bool m_bWantStopDefer = false;

			NContainer::TCMap<uint32, NContainer::TCLinkedList<COutgoingMessage>> m_PendingMessages;
			NContainer::TCLinkedList<COutgoingMessage> *m_pLastPendingMessagesList;
			
			NPtr::TCUniquePointer<NConcurrency::TCContinuation<CWebSocketActor::CCloseInfo>> m_pCloseContinuation;

			NConcurrency::TCActorCallbackManager<void (NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const& _pMessage)> m_OnReceiveBinaryMessage;
			NConcurrency::TCActorCallbackManager<void (NStr::CStr const& _Message)> m_OnReceiveTextMessage;
			NConcurrency::TCActorCallbackManager<void (NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const& _ApplicationData)> m_OnReceivePing;
			NConcurrency::TCActorCallbackManager<void (NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const& _ApplicationData)> m_OnReceivePong;
			NConcurrency::TCActorCallbackManager<void (EWebSocketStatus _Status, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> m_OnClose;
			NConcurrency::TCActorCallbackManager<void (EFinishConnectionResult _Result, CConnectionInfo &&_ConnectionInfo)> m_OnFinishConnection;
			NConcurrency::TCActorCallbackManager<void (EFinishConnectionResult _Result, CClientConnectionInfo &&_ConnectionInfo)> m_OnFinishClientConnection;
			
			void f_HandleControlMessage(CMessage &_Message);
			void f_HandleDataMessage(CMessage &_Message);
			void f_SendMessage(EOpcode _Opcode, uint8 const *_pData, mint _nBytes, bool _bFinished);
			
			COutgoingMessage &f_QueueMessage(EOpcode _Opcode, NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const &_pData, uint32 _Priority);
			COutgoingMessage &f_QueueFragmentedMessage(EOpcode _Opcode, uint8 const *_pData, mint _nBytes, uint32 _Priority);
			void f_WriteQueuedMessages(EOpcode _UntilPriority = EOpcode_ContinuationFrame);
			static void fs_ApplyMask(uint8 *_pData, mint _iDataStart, mint _nBytes, uint8 const *_pMask);
		};

		CWebSocketActor::CWebSocketActor(bool _bClient, mint _MaxMessageSize, mint _FragmentationSize)
			: mp_pInternal(fg_Construct(this, _bClient, _MaxMessageSize, _FragmentationSize))
		{
		}
		
		CWebSocketActor::~CWebSocketActor()
		{
		}
		
		NConcurrency::CActorCallback CWebSocketActor::fp_SetCallbacks
			(
				NConcurrency::TCActor<NConcurrency::CActor> && _Actor
				, NFunction::TCFunction<void (NFunction::CThisTag &, NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const& _pMessage)> && _fReceiveBinaryMessage
				, NFunction::TCFunction<void (NFunction::CThisTag &, NStr::CStr const& _Message)> && _fReceiveTextMessage
				, NFunction::TCFunction<void (NFunction::CThisTag &, NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const& _ApplicationData)> && _fReceivePing
				, NFunction::TCFunction<void (NFunction::CThisTag &, NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const& _ApplicationData)> && _fReceivePong
				, NFunction::TCFunction<void (NFunction::CThisTag &, EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> && _fOnClose
			)
		{
			auto &Internal = *mp_pInternal;
			NPtr::TCUniquePointer<NConcurrency::CCombinedCallbackReference> pCombinedReference = fg_Construct();
			if (_fReceiveBinaryMessage)
				pCombinedReference->m_References.f_Insert(Internal.m_OnReceiveBinaryMessage.f_Register(_Actor, fg_Move(_fReceiveBinaryMessage)));
			if (_fReceiveTextMessage)
				pCombinedReference->m_References.f_Insert(Internal.m_OnReceiveTextMessage.f_Register(_Actor, fg_Move(_fReceiveTextMessage)));
			if (_fReceivePing)
				pCombinedReference->m_References.f_Insert(Internal.m_OnReceivePing.f_Register(_Actor, fg_Move(_fReceivePing)));
			if (_fReceivePong)
				pCombinedReference->m_References.f_Insert(Internal.m_OnReceivePong.f_Register(_Actor, fg_Move(_fReceivePong)));
			if (_fOnClose)
				pCombinedReference->m_References.f_Insert(Internal.m_OnClose.f_Register(_Actor, fg_Move(_fOnClose)));
			
			return fg_Move(pCombinedReference);
		}

		COutgoingMessage &CWebSocketActor::CInternal::f_QueueMessage(EOpcode _Opcode, NPtr::TCSharedPointer<NContainer::TCVector<uint8>> const &_pData, uint32 _Priority)
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
				mint ThisTime = fg_Min(nBytes, m_Framentationsize);
				NContainer::TCVector<uint8> VectorData;
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
			
				if (pPending->m_pContinuation)
					pPending->m_pContinuation->f_SetResult();
				
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
		
		NConcurrency::TCContinuation<CWebSocketActor::CCloseInfo> CWebSocketActor::f_Close(EWebSocketStatus _Status, const NStr::CStr &_Reason)
		{
			auto &Internal = *mp_pInternal;
			if (Internal.m_pCloseContinuation)
			{
				NConcurrency::TCContinuation<CWebSocketActor::CCloseInfo> Ret;
				Ret.f_SetException(DMibErrorInstance("Socket already closed"));
				return Ret;
			}
			
			Internal.m_pCloseContinuation = fg_Construct();
			
			fp_Disconnect(_Status, _Reason, false, EWebSocketCloseOrigin_Local);
			
			return *Internal.m_pCloseContinuation;
		}
		
		NConcurrency::TCContinuation<void> CWebSocketActor::f_SendBinary(NPtr::TCSharedPointer<NContainer::TCVector<uint8>> _Message, uint32 _Priority)
		{
			auto &Internal = *mp_pInternal;
			
			NConcurrency::TCContinuation<void> Result;
			
			NStr::CStr Data;
			
			mint nBytes = _Message->f_GetLen();

			if (nBytes > Internal.m_MaxMessageSize)
			{
				Result.f_SetException(DMibErrorInstance("Message is bigger than max message size"));
				return Result;
			}

			if (_Priority == TCLimitsInt<uint32>::mc_Max)
			{
				Result.f_SetException(DMibErrorInstance("0xffffffff priority is reserved for internal messages"));
				return Result;
			}
			
			if (nBytes <= Internal.m_Framentationsize)
			{
				auto &NewMessage = Internal.f_QueueMessage(EOpcode_BinaryFrame, _Message, _Priority);
				NewMessage.m_pContinuation = fg_Construct(Result);
				return Result;
			}
			
			Internal.f_QueueFragmentedMessage(EOpcode_BinaryFrame, _Message->f_GetArray(), nBytes, _Priority)
				.m_pContinuation = fg_Construct(Result)
			;
			
			fp_UpdateSend();
			
			return Result;
		}
		
		NConcurrency::TCContinuation<void> CWebSocketActor::f_SendText(NStr::CStr const& _Data, uint32 _Priority)
		{
			auto &Internal = *mp_pInternal;
			
			NConcurrency::TCContinuation<void> Result;
			
			NStr::CStr Data = _Data;
			
			mint nBytes = Data.f_GetLen();
			
			if (_Priority == TCLimitsInt<uint32>::mc_Max)
			{
				Result.f_SetException(DMibErrorInstance("0xffffffff priority is reserved for internal messages"));
				return Result;
			}
			if (nBytes > Internal.m_MaxMessageSize)
			{
				Result.f_SetException(DMibErrorInstance("Message is bigger than max message size"));
				return Result;
			}

			Internal.f_QueueFragmentedMessage(EOpcode_TextFrame, (uint8 const *)Data.f_GetStr(), nBytes, _Priority)
				.m_pContinuation = fg_Construct(Result)
			;
			
			fp_UpdateSend();
			
			return Result;
		}
		NConcurrency::TCContinuation<void> CWebSocketActor::f_SendPing(NPtr::TCSharedPointer<NContainer::TCVector<uint8>> _ApplicationData)
		{
			auto &Internal = *mp_pInternal;
			NConcurrency::TCContinuation<void> Result;
			mint nBytes = _ApplicationData->f_GetLen();
			if (nBytes > Internal.m_MaxMessageSize)
			{
				Result.f_SetException(DMibErrorInstance("Message is bigger than max message size"));
				return Result;
			}
			
			auto &NewMessage = Internal.f_QueueMessage(EOpcode_Ping, _ApplicationData, TCLimitsInt<uint32>::mc_Max);
			NewMessage.m_pContinuation = fg_Construct(Result);

			fp_UpdateSend();
			
			return Result;
		}
		NConcurrency::TCContinuation<void> CWebSocketActor::f_SendPong(NPtr::TCSharedPointer<NContainer::TCVector<uint8>> _ApplicationData)
		{
			auto &Internal = *mp_pInternal;
			NConcurrency::TCContinuation<void> Result;
			mint nBytes = _ApplicationData->f_GetLen();
			if (nBytes > Internal.m_MaxMessageSize)
			{
				Result.f_SetException(DMibErrorInstance("Message is bigger than max message size"));
				return Result;
			}
			
			auto &NewMessage = Internal.f_QueueMessage(EOpcode_Pong, _ApplicationData, TCLimitsInt<uint32>::mc_Max);
			NewMessage.m_pContinuation = fg_Construct(Result);

			fp_UpdateSend();
			
			return Result;
		}
		
		void CWebSocketActor::fp_StateAdded(NNet::ENetTCPState _StateAdded)
		{
			fp_ProcessState();
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
					Internal.m_pSocket.f_Clear();
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
					if (Internal.m_pCloseContinuation)
					{
						CWebSocketActor::CCloseInfo CloseInfo;
						CloseInfo.m_Status = _Status;
						CloseInfo.m_Reason = _Reason;
						Internal.m_pCloseContinuation->f_SetResult(fg_Move(CloseInfo));
					}
					Internal.m_OnClose(_Status, _Reason, _Origin);
					
					if (!_bFatal)
					{
						if (WasState == EState_Connected)
						{
							Internal.m_State = EState_Disconnected;
							auto *pHighestPrioMessages = Internal.m_PendingMessages.f_FindLargest();
							if (!pHighestPrioMessages || Internal.m_PendingMessages.fs_GetKey(*pHighestPrioMessages) < EOpcode_ConnectionClose)
								fp_Shutdown();
						}
						else if (WasState == EState_Disconnecting)
						{
							Internal.m_State = EState_Disconnected;
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
					ConnectionInfo.m_Error = _Reason;
					Internal.m_OnFinishClientConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo));
				}
				else
				{
					auto &ConnectionInfo = Internal.m_ConnectionInfo.f_GetAsType<CConnectionInfo>();
					ConnectionInfo.m_Error = _Reason;
					Internal.m_OnFinishConnection(EFinishConnectionResult_Error, fg_Move(ConnectionInfo));
				}
			}

			if (_bFatal)
				Internal.m_pSocket.f_Clear();

			Internal.m_State = EState_Disconnected;
		}
		
		void CWebSocketActor::fp_Shutdown()
		{
			try
			{
				auto &Internal = *mp_pInternal;
				Internal.m_pSocket->f_Shutdown();
			}
			catch (NNet::CExceptionNet const &_Error)
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
			
			while (!Internal.m_OutgoingData.f_IsEmpty() && Internal.m_pSocket->f_IsValid())
			{
				mint SentBytes = 0;
				bool bStuffed = false;
				Internal.m_OutgoingData.f_ReadFront
					(
						[&](mint _iStart, uint8 const* _pPtr, mint _nBytes) -> bool
						{
							try
							{
								mint nSent = Internal.m_pSocket->f_Send(_pPtr, _nBytes);
							
								SentBytes += nSent;
								if (nSent != _nBytes)
								{
									bStuffed = true;
									return false;
								}
								return true;
							}
							catch (NNet::CExceptionNet const &_Error)
							{
								fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket exception: {}", _Error.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
								return false;
							}
						}
					)
				;
				Internal.m_OutgoingData.f_RemoveFront(SentBytes);
				if (bStuffed)
					break;
				if (Internal.m_State == EState_Connected)
					Internal.f_WriteQueuedMessages();
				else if (Internal.m_State == EState_Disconnecting)				
					Internal.f_WriteQueuedMessages(EOpcode_ConnectionClose);
			}
			
			if (Internal.m_State == EState_Disconnected && Internal.m_OutgoingData.f_IsEmpty())
			{
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

		void CWebSocketActor::CInternal::fs_ApplyMask(uint8 *_pData, mint _iDataStart, mint _nBytes, uint8 const *_pMask)
		{
			uint8 DoubleMask[8];
			NMem::fg_MemCopy(DoubleMask, _pMask, 4);
			NMem::fg_MemCopy(DoubleMask + 4, _pMask, 4);

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
			NMem::fg_MemCopy(&AlignedMask, DoubleMask + MaskOffset, sizeof(uint32));
			
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
									NMem::fg_MemCopy((uint8 *)Data + (_iStart - Position), _pData, _nBytes);
									
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
									NMem::fg_MemCopy((uint8 *)&Data + (_iStart - Position), _pData, _nBytes);
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
									NMem::fg_MemCopy((uint8 *)&Data + (_iStart - Position), _pData, _nBytes);
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
									NMem::fg_MemCopy(Mask + (_iStart - Position), _pData, _nBytes);
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
						fp_Disconnect(EWebSocketStatus_ProtocolError, "Continuation frame without start frame", false, EWebSocketCloseOrigin_Local);
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
					if (m_OnReceivePing.f_IsEmpty())
					{
						// We reply automatically as quickly as possible
						f_SendMessage(EOpcode_ConnectionClose, _Message.m_Data.f_GetArray(), _Message.m_Data.f_GetLen(), true);
						m_pThis->fp_UpdateSend();
					}
					else
					{
						// Otherwise we let the application reply
						m_OnReceivePing(fg_Construct(fg_Move(_Message.m_Data)));
					}
				}
				break;
			case EOpcode_Pong:
				{
					// RFC 6455 - 5.5.3.
					m_OnReceivePong(fg_Construct(fg_Move(_Message.m_Data)));
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
			switch (_Message.m_Header.m_Opcode)
			{
			case EOpcode_TextFrame:
				{
					NStr::CStr Data;
					Data.f_AddStr(_Message.m_Data.f_GetArray(), _Message.m_Data.f_GetLen());
					m_OnReceiveTextMessage(fg_Move(Data));
				}
				break;
			case EOpcode_BinaryFrame:
				{
					m_OnReceiveBinaryMessage(fg_Construct(fg_Move(_Message.m_Data)));
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
							case NHTTP::ERequestStatus_Complete:
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
										
			
										NDataProcessing::CHash_SHA1 Hash;
										Hash.f_AddData(Internal.m_ClientConnectionInput.m_EncodedKey.f_GetStr(), Internal.m_ClientConnectionInput.m_EncodedKey.f_GetLen());
										Hash.f_AddData("258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
										
										NDataProcessing::CHash_SHA1::CMessageDigest Digest = Hash;
										
										NContainer::TCVector<uint8> DigestData;
										DigestData.f_Insert(Digest.f_GetData(), Digest.fs_GetSize());
										
										NStr::CStr CorrectKey = NDataProcessing::fg_Base64Encode(DigestData);
										
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
									Internal.m_State = EState_Connected;
									Internal.m_OnFinishClientConnection(EFinishConnectionResult_Success, fg_Move(ConnectionInfo));
									bMoreWork = true;
									
								}
								break;
							case NHTTP::ERequestStatus_Invalid:
								{
									fp_Disconnect(EWebSocketStatus_ProtocolError, "Invalid HTTP request header", true, EWebSocketCloseOrigin_Local);
								}
								return;
							case NHTTP::ERequestStatus_InProgress:
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
									ConnectionInfo.m_pSocketInfo = Internal.m_pSocket->f_GetConnectionInfo();
									
									Internal.m_OnFinishConnection(EFinishConnectionResult_Success, fg_Move(ConnectionInfo));
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
			fp_TryStopDeferring();
		}
		
		void CWebSocketActor::fp_RejectClientConnection(NStr::CStr const &_Error)
		{
			fp_TryStopDeferring();
			
			fp_Disconnect(EWebSocketStatus_Rejected, NStr::fg_Format("Rejected connection: {}", _Error), false, EWebSocketCloseOrigin_Local);
		}

		void CWebSocketActor::fp_AcceptServerConnection(NStr::CStr const &_Protocol, NHTTP::CResponseHeader &&_ResponseHeader)
		{
			auto &Internal = *mp_pInternal;
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
			
			NDataProcessing::CHash_SHA1 Hash;
			Hash.f_AddData(Internal.m_Key.f_GetStr(), Internal.m_Key.f_GetLen());
			Hash.f_AddData("258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
			NDataProcessing::CHash_SHA1::CMessageDigest Digest = Hash;
			
			NContainer::TCVector<uint8> DigestData;
			DigestData.f_Insert(Digest.f_GetData(), Digest.fs_GetSize());
			
			EntityFields.f_SetUnknownField("Sec-WebSocket-Accept", NDataProcessing::fg_Base64Encode(DigestData));

			Response.f_Complete();
			
			Internal.m_State = EState_Connected;
			
			fp_UpdateSend();
			
			//DMibTrace("Websocket connected\n", 0);
			
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
				Response.f_SetStatus(NHTTP::EStatus_BadRequest);
			
			auto Content = Response.f_Complete();
			
			if (!_Content.f_IsEmpty())
				Content.f_SendString(_Content);
			else
				Content.f_SendString(_Error);
			fp_UpdateSend();
			fp_Disconnect(EWebSocketStatus_Rejected, NStr::fg_Format("Rejected connection: {}", _Error), false, EWebSocketCloseOrigin_Local);
		}
		
		void CWebSocketActor::fp_ProcessState()
		{
			auto &Internal = *mp_pInternal;
			
			if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
				return;

			auto StateAdded = Internal.m_pSocket->f_GetState();
			if (StateAdded & NNet::ENetTCPState_Closed)
			{
				if (Internal.m_State != EState_Disconnected)
					fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket closed: {}", Internal.m_pSocket->f_GetCloseReason()), true, EWebSocketCloseOrigin_Remote);
				else
					Internal.m_pSocket.f_Clear();
				return;
			}
			if (StateAdded & NNet::ENetTCPState_Read)
			{
				uint8 Data[4096];
				try
				{
					while (true)
					{
						mint Size = 4096;
						mint Received = Internal.m_pSocket->f_Receive(Data, Size);
						if (Received == 0)
							break;
						Internal.m_IncomingData.f_InsertBack(Data, Received);
						fp_ProcessIncoming();
						if (!Internal.m_pSocket || !Internal.m_pSocket->f_IsValid())
							return;
					}
				}
				catch (NNet::CExceptionNet const& _Exception)
				{
					fp_Disconnect(EWebSocketStatus_AbnormalClosure, NStr::fg_Format("Socket error: {}", _Exception.f_GetErrorStr()), true, EWebSocketCloseOrigin_Remote);
					return;
				}
			}
			
			if (StateAdded & NNet::ENetTCPState_Write)
				fp_UpdateSend();		
		}

		void CWebSocketActor::fp_SetSocket(NPtr::TCUniquePointer<NNet::ICSocket> && _pSocket)
		{
			auto &Internal = *mp_pInternal;
			Internal.m_pSocket = fg_Move(_pSocket);
			fp_ProcessState();
		}
		
		NConcurrency::CActorCallback CWebSocketActor::fp_OnFinishServerConnection
			(
				NConcurrency::TCActor<NConcurrency::CActor> &&_Actor
				, NFunction::TCFunction<void (EFinishConnectionResult _Result, CConnectionInfo &&_ConnectionInfo)> &&_fOnFinishConnection
			)
		{
			auto &Internal = *mp_pInternal;
			auto Return = Internal.m_OnFinishConnection.f_Register(fg_Move(_Actor), fg_Move(_fOnFinishConnection));
			
			if (Internal.m_bWantStopDefer)
				fp_StopDeferring();

			return Return;
		}

		NConcurrency::CActorCallback CWebSocketActor::fp_OnFinishClientConnection
			(
				NConcurrency::TCActor<NConcurrency::CActor> &&_Actor
				, NFunction::TCFunction<void (EFinishConnectionResult _Result, CClientConnectionInfo &&_ConnectionInfo)> &&_fOnFinishConnection
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
			
			NContainer::TCVector<uint8> RandomData;
			RandomData.f_SetLen(16);
			NCryptography::fg_GenerateRandomData(RandomData.f_GetArray(), RandomData.f_GetLen());
			
			NStr::CStr EncodedRandomData = NDataProcessing::fg_Base64Encode(RandomData);
			
			if (!Protocols.f_IsEmpty())
				EntityFields.f_SetUnknownField("Sec-WebSocket-Protocol", Protocols);
			EntityFields.f_SetUnknownField("Sec-WebSocket-Version", "13");
			EntityFields.f_SetUnknownField("Sec-WebSocket-Key", EncodedRandomData);
			
			for (auto &Protocol : _Protocols)
				Internal.m_ClientConnectionInput.m_Protocols[Protocol];
			Internal.m_ClientConnectionInput.m_EncodedKey = EncodedRandomData;
			
			NContainer::TCVector<uint8> SendData;
			_RequestHeader.f_WriteHeaders
				(
					[&](uint8 const *_pData, mint _nBytes)
					{
						Internal.m_OutgoingData.f_InsertBack(_pData, _nBytes);
					}
				)
			;
			fp_UpdateSend();
			fp_ProcessState();
			
			return Return;
		}
	}
}
