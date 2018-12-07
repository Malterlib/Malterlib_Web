// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

namespace NMib::NWeb::NWebSocket
{
	CListenActor::CListenActor(NConcurrency::TCActor<CWebSocketServerActor> const& _Server, mint _MaxMesageSize, mint _FragmentationSize, fp64 _Timeout)
		: mp_Server(_Server)
		, mp_MaxMessageSize(_MaxMesageSize)
		, mp_FragmentationSize(_FragmentationSize)
		, mp_Timeout(_Timeout)
	{
	}

	CListenActor::~CListenActor()
	{
	}

	void CListenActor::f_SetSocket(NStorage::TCUniquePointer<NNetwork::ICSocket> && _pSocket)
	{
		mp_pSocket = fg_Move(_pSocket);
		fp_ProcessState();
	}

	NConcurrency::TCContinuation<void> CListenActor::fp_Destroy()
	{
		if (mp_pSocket)
			mp_pSocket.f_Clear();
		return NConcurrency::TCContinuation<void>::fs_Finished();
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
				NConcurrency::TCActor<CWebSocketActor> ConnectionActor = NConcurrency::fg_ConstructActor<CWebSocketActor>(false, mp_MaxMessageSize, mp_FragmentationSize, mp_Timeout);
				NConcurrency::TCWeakActor<CWebSocketActor> WeakConnectionActor = ConnectionActor;
				NStorage::TCUniquePointer<NNetwork::ICSocket> pAcceptedSocket = mp_pSocket->f_Accept
					(
						[WeakConnectionActor](NNetwork::ENetTCPState _StateAdded)
						{
							auto ConnectionActor = WeakConnectionActor.f_Lock();
							if (ConnectionActor)
							{
								ConnectionActor(&CWebSocketActor::fp_StateAdded, _StateAdded)
									> NConcurrency::fg_DiscardResult()
								;
							}
						}
					)
				;

				if (!pAcceptedSocket)
					break;

				ConnectionActor(&CWebSocketActor::fp_SetSocket, fg_Move(pAcceptedSocket))
					> NConcurrency::fg_DiscardResult()
				;

				auto Server = mp_Server.f_Lock();

				if (Server)
				{
					Server(&CWebSocketServerActor::fp_AddConnection, fg_Move(ConnectionActor))
						> NConcurrency::fg_DiscardResult()
					;
				}
			}
		}
	}
}
