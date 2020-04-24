// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorCallbackManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

namespace NMib::NWeb
{
	struct CWebSocketServerActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(CWebSocketServerActor *_pThis)
			: m_pThis(_pThis)
			, m_OnNewConnection(_pThis, false)
			, m_OnFailedConnection(_pThis, false)
		{
		}

	public:
		void f_Clear();

		CWebSocketServerActor *m_pThis;
		NContainer::TCVector<NConcurrency::TCActor<NWebSocket::CListenActor>> m_ListenSockets;
		NConcurrency::TCActorSubscriptionManager<void (CWebSocketNewServerConnection &&_NewConnection)> m_OnNewConnection;
		NConcurrency::TCActorSubscriptionManager<void (CWebSocketActor::CConnectionInfo && _ConnectionInfo)> m_OnFailedConnection;
		NContainer::TCLinkedList<NConcurrency::CActorSubscription> m_Subscriptions;
		fp64 m_Timeout = 60.0;
		mint m_MaxMessageSize = 24*1024*1024;
		mint m_FragmentationSize = 32*1024;
		bool m_bBroken = false;
	};
}
