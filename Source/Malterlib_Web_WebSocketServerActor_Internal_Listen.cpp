// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

namespace NMib::NWeb::NWebSocket
{
	CListenActor::CListenActor(NConcurrency::TCActor<CWebSocketServerActor> const& _Server, CWebsocketSettings const &_Settings)
		: mp_Server(_Server)
		, mp_Settings(_Settings)
	{
	}

	CListenActor::~CListenActor()
	{
	}

	void CListenActor::f_SetSocket(NStorage::TCUniquePointer<NNetwork::ICSocket> _pSocket)
	{
		mp_pSocket = fg_Move(_pSocket);
		fp_ProcessState();
	}

	NConcurrency::TCFuture<void> CListenActor::fp_Destroy()
	{
		if (mp_pSocket)
			mp_pSocket.f_Clear();
		co_return {};
	}

	void CListenActor::f_StateAdded(NNetwork::ENetTCPState _StateAdded)
	{
		if (mp_pSocket && mp_pSocket->f_IsValid())
			fp_ProcessState();
	}

	void CListenActor::fp_ProcessState()
	{
		DMibFastCheck(mp_pSocket && mp_pSocket->f_IsValid());
		auto StateAdded = mp_pSocket->f_GetState();
		if (StateAdded & NNetwork::ENetTCPState_Connection)
		{
			while (true)
			{
				NConcurrency::TCActor<CWebSocketActor> ConnectionActor = NConcurrency::fg_ConstructActor<CWebSocketActor>(false, mp_Settings);
				try
				{
					NStorage::TCUniquePointer<NNetwork::ICSocket> pAcceptedSocket = mp_pSocket->f_Accept
						(
							[WeakConnectionActor = ConnectionActor.f_Weak()](NNetwork::ENetTCPState _StateAdded)
							{
								auto ConnectionActor = WeakConnectionActor.f_Lock();
								if (ConnectionActor)
									ConnectionActor.f_Bind<&CWebSocketActor::fp_StateAdded>(_StateAdded).f_DiscardResult();
							}
						)
					;

					if (!pAcceptedSocket)
						break;

					DMibFastCheck(pAcceptedSocket->f_IsValid());

					ConnectionActor.f_Bind<&CWebSocketActor::fp_SetSocket>(fg_Move(pAcceptedSocket)).f_DiscardResult();

					auto Server = mp_Server.f_Lock();
					if (Server)
						Server.f_Bind<&CWebSocketServerActor::fp_AddConnection>(fg_Move(ConnectionActor)).f_DiscardResult();
				}
				catch (NException::CException const &)
				{
				}
			}
		}
	}
}
