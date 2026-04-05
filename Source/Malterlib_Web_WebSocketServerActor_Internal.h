// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

namespace NMib::NWeb
{
	struct CWebSocketServerActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(CWebSocketServerActor *_pThis, CWebsocketSettings const &_DefaultSettings)
			: m_pThis(_pThis)
			, m_DefaultSettings(_DefaultSettings)
		{
		}

	public:
		void f_Clear();

		CWebSocketServerActor *m_pThis;
		NContainer::TCVector<NConcurrency::TCActor<NWebSocket::CListenActor>> m_ListenSockets;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketNewServerConnection _NewConnection)> m_fOnNewConnection;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketActor::CConnectionInfo _ConnectionInfo)> m_fOnFailedConnection;
		NContainer::TCLinkedList<NConcurrency::CActorSubscription> m_Subscriptions;
		CWebsocketSettings m_DefaultSettings;
		bool m_bBroken = false;
	};
}
