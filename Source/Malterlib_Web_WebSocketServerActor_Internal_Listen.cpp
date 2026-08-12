// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
				// The actor's own manager, matching the loop the socket binds to below
				NConcurrency::TCActor<CWebSocketActor> ConnectionActor = f_ConcurrencyManager().f_ConstructActor(fg_Construct<CWebSocketActor>(false, mp_Settings));

				// The socket registers with a pool thread's event loop below, so an arriving
				// message is reported on that thread and the actor call it enqueues stays on the
				// local queue there. The scheduler keeps the actor local by default and only
				// distributes it under pressure, so no pinning is needed for the locality. The
				// actor's own manager, so a server hosted off the global one gets loops whose
				// threads run its pool
				auto Binding = f_ConcurrencyManager().f_PickIoLoopBinding(CWebSocketActor::mc_Priority);

				// Seed the scheduler placement to the bound queue so even the first job runs
				// where the loop reports the socket's events; no pinning — every later job
				// keeps the marker fresh and migration under pressure stays free
				if (Binding.m_pLoop)
					ConnectionActor->f_SetInitialQueue(Binding.m_iQueue);

				try
				{
					NConcurrency::CIoLoopCreateScope IoLoopScope(Binding);

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
