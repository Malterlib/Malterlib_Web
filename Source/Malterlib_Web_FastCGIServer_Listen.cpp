// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/WeakActor>

#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Listen.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"
#include "Malterlib_Web_FastCGIServer_Protocol.h"


namespace NMib::NWeb::NFastCGI
{
	CListenActor::CListenActor
		(
			NConcurrency::TCActor<CFastCGIServer> const &_Server
			, NStorage::TCSharedPointer<NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)>> const &_pOnRequest
		)
		: mp_Server(_Server)
		, mp_pOnRequest(_pOnRequest)
	{
	}

	CListenActor::~CListenActor()
	{
	}

	void CListenActor::f_SetSocket(NStorage::TCSharedPointer<NNetwork::CSocket> const &_pSocket)
	{
		mp_Socket = fg_Move(*_pSocket);
		fp_ProcessState();
	}

	NConcurrency::TCFuture<void> CListenActor::fp_Destroy()
	{
		mp_Socket.f_Close();
		return fg_Explicit();
	}

	void CListenActor::f_StateAdded(NNetwork::ENetTCPState _StateAdded)
	{
		if (mp_Socket.f_IsValid())
			fp_ProcessState();
	}

	void CListenActor::fp_ProcessState()
	{
		DMibFastCheck(mp_Socket.f_IsValid());
		auto StateAdded = mp_Socket.f_GetState();
		if (StateAdded & NNetwork::ENetTCPState_Connection)
		{
			while (true)
			{
				NConcurrency::TCActor<CFastCGIConnectionActor> ConnectionActor = NConcurrency::fg_ConstructActor<CFastCGIConnectionActor>(mp_Server, mp_pOnRequest);
				NConcurrency::TCWeakActor<CFastCGIConnectionActor> WeakConnectionActor = ConnectionActor;
				NNetwork::CSocket AcceptedSocket;
				try
				{
					AcceptedSocket.f_Accept
						(
							&mp_Socket
							, [WeakConnectionActor](NNetwork::ENetTCPState _StateAdded)
							{
								auto ConnectionActor = WeakConnectionActor.f_Lock();
								if (ConnectionActor)
								{
									ConnectionActor(&CFastCGIConnectionActor::f_StateAdded, _StateAdded)
										> NConcurrency::fg_DiscardResult()
									;
								}
							}
						)
					;
				}
				catch (NException::CException const &_Exception)
				{
					(void)_Exception;
					DMibLogWithCategory(FastCGI, Error, "Exception accepting incoming connection: {}", _Exception);
					break;
				}
				if (!AcceptedSocket.f_IsValid())
					break;
				NStorage::TCSharedPointer<NNetwork::CSocket> pSocket = fg_Construct(fg_Move(AcceptedSocket));

				ConnectionActor(&CFastCGIConnectionActor::f_SetSocket, pSocket) > NConcurrency::fg_DiscardResult();

				mp_Server(&CFastCGIServer::fp_AddConnection, ConnectionActor) > NConcurrency::fg_DiscardResult();
			}
		}
	}
}
