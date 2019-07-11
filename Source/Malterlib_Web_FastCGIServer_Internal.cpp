// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/WeakActor>

#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Listen.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"

namespace NMib::NWeb
{
	using namespace NFastCGI;

	CFastCGIServer::CInternal::CInternal(CFastCGIServer *_pThis)
		: mp_pThis(_pThis)
	{
	}

	CFastCGIServer::CInternal::~CInternal()
	{
	}

	NConcurrency::TCFuture<void> CFastCGIServer::CInternal::f_Start
		(
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)> &&_fOnRequest
			, uint16 _FastCGIListenStartPort
			, uint16 _nListen
			, NNetwork::CNetAddress const &_BindAddress
		)
	{
		if (mp_pOnRequest)
			return DMibErrorInstance("Already started");
		mp_pOnRequest = fg_Construct(fg_Move(_fOnRequest));

		mint nThreads = _nListen;
		uint16 StartListen = _FastCGIListenStartPort;

		NContainer::TCVector<NConcurrency::TCActor<NFastCGI::CListenActor>> ListenSockets;
		ListenSockets.f_SetLen(nThreads);
		auto Cleanup = g_OnScopeExit > [&]
			{
				for (auto &ListenSocket : ListenSockets)
				{
					if (ListenSocket)
						ListenSocket->f_Destroy() > NConcurrency::fg_DiscardResult();
				}
			}
		;

		for (mint i = 0; i < nThreads; ++i)
		{
			NNetwork::CNetAddress Address = _BindAddress;
			Address.f_SetPort(StartListen + i);

			NConcurrency::TCActor<CListenActor> &ListenActor = ListenSockets[i];
			ListenActor = NConcurrency::fg_ConstructActor<CListenActor>(fg_ThisActor(mp_pThis), mp_pOnRequest);

			NNetwork::CSocket ListenSocket;
			try
			{
				ListenSocket.f_Listen
					(
						Address
						, [WeakListenActor = ListenActor.f_Weak()](NNetwork::ENetTCPState _StateAdded)
						{
							auto ListenActor = WeakListenActor.f_Lock();
							if (!ListenActor)
								return;
							ListenActor(&CListenActor::f_StateAdded, _StateAdded) > NConcurrency::fg_DiscardResult();
						}
						, NNetwork::ENetFlag_None
					)
				;
			}
			catch (NException::CException const &_Exception)
			{
				using namespace NStr;
				return DMibErrorInstance("Failed to listen in FastCGI server: {}"_f <<_Exception);
			}

			NStorage::TCSharedPointer<NNetwork::CSocket> pSocket = fg_Construct(fg_Move(ListenSocket));

			ListenActor(&CListenActor::f_SetSocket, pSocket) > NConcurrency::fg_DiscardResult();
		}

		mp_ListenSockets = fg_Move(ListenSockets);

		return fg_Explicit();
	}

	void CFastCGIServer::fp_AddConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const &_Connection)
	{
		auto &Internal = *mp_pInternal;
		Internal.mp_Connections[_Connection];
	}

	void CFastCGIServer::fp_RemoveConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const &_Connection)
	{
		auto &Internal = *mp_pInternal;
		if (Internal.mp_Connections.f_Remove(_Connection))
			_Connection->f_Destroy() > Internal.mp_pCanDestroyTracker->f_Track();
	}

	NConcurrency::TCFuture<void> CFastCGIServer::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		auto pCanDestroy = fg_Move(Internal.mp_pCanDestroyTracker);

		for (auto& ListenSocket : Internal.mp_ListenSockets)
			ListenSocket->f_Destroy() > pCanDestroy->f_Track();

		Internal.mp_ListenSockets.f_Clear();

		for (auto& Connection : Internal.mp_Connections)
			Connection->f_Destroy() > pCanDestroy->f_Track();
		Internal.mp_Connections.f_Clear();

		return pCanDestroy->f_Future();
	}
}
