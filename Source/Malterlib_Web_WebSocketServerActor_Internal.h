// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorCallbackManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

namespace NMib::NWeb
{
	struct CWebSocketServerActor::CInternal
	{

		CInternal(CWebSocketServerActor *_pThis)
			: m_pThis(_pThis)
			, m_OnNewConnection(_pThis, false)
			, m_OnFailedConnection(_pThis, false)
			, m_MaxMessageSize(24*1024*1024)
			, m_FragmentationSize(32*1024)
			, m_Timeout(60.0)
		{
		}

	public:
		void f_Clear();

		CWebSocketServerActor *m_pThis;
		NContainer::TCVector<NConcurrency::TCActor<NWebSocket::CListenActor>> m_ListenSockets;
		NContainer::TCSet<NConcurrency::TCActor<CWebSocketActor>> m_Connections;
		NStorage::TCSharedPointer<NConcurrency::CCanDestroyTracker> m_pCanDestroyTracker;
		NConcurrency::TCActorSubscriptionManager<void (CWebSocketNewServerConnection &&_NewConnection)> m_OnNewConnection;
		NConcurrency::TCActorSubscriptionManager<void (CWebSocketActor::CConnectionInfo && _ConnectionInfo)> m_OnFailedConnection;
		fp64 m_Timeout;
		mint m_MaxMessageSize;
		mint m_FragmentationSize;
		NContainer::TCLinkedList<NConcurrency::CActorSubscription> m_Subscriptions;
	};
}
