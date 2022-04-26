// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

namespace NMib::NWeb
{
	struct CWebSocketServerActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(CWebSocketServerActor *_pThis)
			: m_pThis(_pThis)
		{
		}

	public:
		void f_Clear();

		CWebSocketServerActor *m_pThis;
		NContainer::TCVector<NConcurrency::TCActor<NWebSocket::CListenActor>> m_ListenSockets;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketNewServerConnection &&_NewConnection)> m_fOnNewConnection;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketActor::CConnectionInfo &&_ConnectionInfo)> m_fOnFailedConnection;
		NContainer::TCLinkedList<NConcurrency::CActorSubscription> m_Subscriptions;
		fp64 m_Timeout = 60.0;
		mint m_MaxMessageSize = 24*1024*1024;
		mint m_FragmentationSize = 32*1024;
		bool m_bBroken = false;
	};
}
