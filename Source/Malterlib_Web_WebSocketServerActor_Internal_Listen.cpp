// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/LogError>


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
		{
			// Wait for asynchronous deregistration before listener destruction completes.
			NConcurrency::TCPromise<void> ClosedPromise;
			auto Closed = ClosedPromise.f_Future();

			mp_pSocket->f_CloseAsync
				(
					[ClosedPromise = fg_Move(ClosedPromise)]() mutable
					{
						ClosedPromise.f_SetResult();
					}
				)
			;
			mp_pSocket.f_Clear();

			co_await fg_Move(Closed);
		}

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
				NConcurrency::TCActor<CWebSocketActor> ConnectionActor = f_ConcurrencyManager().f_ConstructActor(fg_Construct<CWebSocketActor>(false, mp_Settings));

				// Use the actor's manager so socket and loop lifetimes agree; queue seeding provides locality without pinning.
				auto Binding = f_ConcurrencyManager().f_PickIoLoopBinding(CWebSocketActor::mc_Priority);

				// Seed first-job placement on the bound loop queue without pinning later scheduling.
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
								{
									DMibLogWarningOrDiscardResult
										(
											ConnectionActor.f_Bind<&CWebSocketActor::fp_StateAdded>(_StateAdded)
											, "Mib/Web"
											, "Reporting the socket state to the connection actor failed"
										)
									;
								}
							}
						)
					;

					if (!pAcceptedSocket)
						break;

					DMibFastCheck(pAcceptedSocket->f_IsValid());

					DMibLogWarningOrDiscardResult
						(
							ConnectionActor.f_Bind<&CWebSocketActor::fp_SetSocket>(fg_Move(pAcceptedSocket))
							, "Mib/Web"
							, "Handing the accepted socket to the connection actor failed"
						)
					;

					auto Server = mp_Server.f_Lock();
					if (Server)
					{
						DMibLogWarningOrDiscardResult
							(
								Server.f_Bind<&CWebSocketServerActor::fp_AddConnection>(fg_Move(ConnectionActor))
								, "Mib/Web"
								, "Adding the accepted connection to the server failed"
							)
						;
					}
				}
				catch (NException::CException const &_Exception)
				{
					DMibLogWithCategory(Mib/Web, Warning, "Accepting a websocket connection failed: {}", _Exception);
				}
			}
		}
	}
}
