// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Listen.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"

namespace NMib::NWeb
{
	using namespace NFastCGI;
	CFastCGIServer::CInternal::CInternal
		(
			NFunction::TCFunction<void (NStorage::TCSharedPointer<CFastCGIRequest> const& _Request)> &&_fOnRequest
			, uint16 _FastCGIListenStartPort
			, uint16 _nListen
		)
		: mp_pCanDestroyTracker(fg_Construct())
		, mp_fOnRequest(fg_Move(_fOnRequest))
	{
		fp_Startup(_FastCGIListenStartPort, _nListen);
	}

	CFastCGIServer::CInternal::~CInternal()
	{
	}

	void CFastCGIServer::CInternal::f_AddConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const& _Connection)
	{
		mp_Connections[_Connection];
	}

	void CFastCGIServer::CInternal::f_RemoveConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const& _Connection)
	{
		if (mp_Connections.f_Remove(_Connection))
			_Connection->f_Destroy() > mp_pCanDestroyTracker->f_Track();
	}

	NConcurrency::TCFuture<void> CFastCGIServer::CInternal::fp_Destroy()
	{
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);

		for (auto& ListenSocket : mp_ListenSockets)
			ListenSocket->f_Destroy() > pCanDestroy->f_Track();

		mp_ListenSockets.f_Clear();

		for (auto& Connection : mp_Connections)
			Connection->f_Destroy() > pCanDestroy->f_Track();
		mp_Connections.f_Clear();

		return pCanDestroy->f_Future();
	}

	void CFastCGIServer::CInternal::fp_Startup(uint16 _FastCGIListenStartPort, uint16 _nListen)
	{
		mint nThreads = _nListen;
		uint16 StartListen = _FastCGIListenStartPort;

		mp_ListenSockets.f_SetLen(nThreads);

		auto ConcurrentActor = NConcurrency::fg_ConcurrentActor();
		for (mint i = 0; i < nThreads; ++i)
		{
			NNetwork::CNetAddressTCPv4 AnyAddress;
			AnyAddress.m_Port = StartListen + i;
			NNetwork::CNetAddress Address;
			Address.f_Set(AnyAddress);

			NConcurrency::TCActor<CListenActor> &ListenActor = mp_ListenSockets[i];
			ListenActor = NConcurrency::fg_ConstructActor<CListenActor>(fg_ThisActor(this), *this);

			NNetwork::CSocket ListenSocket;
			ListenSocket.f_Listen
				(
					Address
					, [ListenActor, ConcurrentActor](NNetwork::ENetTCPState _StateAdded)
					{
						ListenActor(&CListenActor::f_StateAdded, _StateAdded)
							> ConcurrentActor / [](NConcurrency::TCAsyncResult<void>&& _Result)
							{
							}
						;
					}
					, NMib::NNetwork::ENetFlag_None
				)
			;

			NStorage::TCSharedPointer<NNetwork::CSocket> pSocket = fg_Construct(fg_Move(ListenSocket));

			ListenActor(&CListenActor::f_SetSocket, pSocket)
				> ConcurrentActor / [](NConcurrency::TCAsyncResult<void>&& _Result)
				{
				}
			;

		}
	}
}
