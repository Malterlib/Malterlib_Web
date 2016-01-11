// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib
 
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

#include <Mib/Network/Sockets/TCP>

namespace NMib
{
	namespace NWeb
	{
		using namespace NWebSocket;
		CWebSocketServerActor::CWebSocketServerActor()
			: mp_pInternal(fg_Construct(this))
		{
		}

		CWebSocketServerActor::~CWebSocketServerActor()
		{
		}
		
		void CWebSocketServerActor::f_SetDefaultMaxMessageSize(mint _MaxMessageSize)
		{
			DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
			mp_pInternal->m_MaxMessageSize = _MaxMessageSize;
		}
		
		void CWebSocketServerActor::f_SetDefaultFragmentationSize(mint _FragmentationSize)
		{
			DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
			mp_pInternal->m_FragmentationSize = _FragmentationSize;
		}

		void CWebSocketServerActor::CInternal::f_Clear()
		{
			m_ListenSockets.f_Clear();
			m_OnNewConnection.f_Clear();
			m_OnFailedConnection.f_Clear();
		}

		NConcurrency::TCContinuation<NConcurrency::CActorCallback> CWebSocketServerActor::f_StartListen
			(
				uint16 _StartListen		// The port to listen to
				, uint16 _nListen		// The number of ports to listen to. In consecutive order from the _StartListen port
				, NConcurrency::TCActor<NConcurrency::CActor> const& _Actor // The actor to receive new connections
				, NFunction::TCFunction<void (CWebSocketNewServerConnection && _Connection)> && _fNewConnection	// The functor called on the actor for each new connection 
				, NFunction::TCFunction<void (CWebSocketActor::CConnectionInfo && _ConnectionInfo)> && _fFailedConnection // The functor called on the actor for each connection attempt that failed
				, NNet::FVirtualSocketFactory &&_SocketFactory
			)
		{
			if (!_SocketFactory)
				_SocketFactory = NNet::CSocket_TCP::fs_GetFactory();
			
			NConcurrency::TCContinuation<NConcurrency::CActorCallback> Ret;
			try
			{
				if (!mp_pInternal->m_ListenSockets.f_IsEmpty())
					DMibError("Socket server is already listening");
			
				mp_pInternal->f_Clear();
				auto NewReference = mp_pInternal->m_OnNewConnection.f_Register(_Actor, fg_Move(_fNewConnection));
				auto FailedReference = mp_pInternal->m_OnFailedConnection.f_Register(_Actor, fg_Move(_fFailedConnection));
				auto Reference = fg_CombinedCallbackReference(fg_Move(NewReference), fg_Move(FailedReference));
				
				mp_pInternal->m_ListenSockets.f_SetLen(_nListen);

				for (mint i = 0; i < _nListen; ++i)
				{
					NNet::CNetAddressTCPv4 AnyAddress;
					AnyAddress.m_Port = _StartListen + i;
					NNet::CNetAddress Address;
					Address.f_Set(AnyAddress);

					NConcurrency::TCActor<CListenActor> &ListenActor = mp_pInternal->m_ListenSockets[i];
					ListenActor = NConcurrency::fg_ConstructActor<CListenActor>(fg_ThisActor(this), mp_pInternal->m_MaxMessageSize, mp_pInternal->m_FragmentationSize);
					
					NConcurrency::TCWeakActor<CListenActor> WeakListenActor = ListenActor;
					
					NPtr::TCUniquePointer<NNet::ICSocket> pListenSocket = _SocketFactory();
					pListenSocket->f_Listen
						(
							Address
							, [WeakListenActor](NNet::ENetTCPState _StateAdded)
							{
								
								auto ListenActor = WeakListenActor.f_Lock();
								if (ListenActor)
								{
									ListenActor(&CListenActor::f_StateAdded, _StateAdded)
										> NConcurrency::fg_DiscardResult()
									;
								}
							}
						)
					;
					
					ListenActor(&CListenActor::f_SetSocket, fg_Move(pListenSocket))
						> NConcurrency::fg_DiscardResult()
					;
				}			
				
				Ret.f_SetResult(fg_Move(Reference));
				
			}
			catch (NException::CException const &)
			{
				mp_pInternal->f_Clear();
				Ret.f_SetCurrentException();
			}		
			return Ret;
		}
		
		void CWebSocketServerActor::fp_AddConnection(NConcurrency::TCActor<CWebSocketActor> && _Connection)
		{
			auto pSubscription = &mp_pInternal->m_Subscriptions.f_Insert();
			NPtr::TCSharedPointer<NAtomic::TCAtomic<bool>> pHandled = fg_Construct(false);
			_Connection
				(
					&CWebSocketActor::fp_OnFinishServerConnection
					, fg_ThisActor(this)
					, [this, _Connection, pSubscription, pHandled, WeakThis = fg_ThisActor(this).f_Weak()]
					(CWebSocketActor::EFinishConnectionResult _Result, CWebSocketActor::CConnectionInfo &&_ConnectionInfo)
					{
						auto This = WeakThis.f_Lock();
						if (!This)
							return;
						
						switch (_Result)
						{
						case CWebSocketActor::EFinishConnectionResult_Error:
							{
								mp_pInternal->m_OnFailedConnection(fg_Move(_ConnectionInfo));
								pSubscription->f_Clear();
							}
							break;
						case CWebSocketActor::EFinishConnectionResult_Success:
							{
								auto Protocols = _ConnectionInfo.m_Protocols;
								CWebSocketNewServerConnection Connection(fg_Move(_ConnectionInfo), fg_Move(Protocols), _Connection);
								mp_pInternal->m_OnNewConnection(fg_Move(Connection));
								pSubscription->f_Clear();
							}
							break;
						default:
							DMibPDebugBreak;
							break;
						}
						*pHandled = true;
						mp_pInternal->m_Subscriptions.f_Remove(*pSubscription);
					}
				)
				> [pSubscription, pHandled](NConcurrency::TCAsyncResult<NConcurrency::CActorCallback> &&_Result)
				{
					if (!*pHandled)
						*pSubscription = fg_Move(*_Result);
				}
			;
		}
	}
}
