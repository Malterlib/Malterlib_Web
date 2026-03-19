// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/WeakActor>
#include <Mib/Concurrency/LogError>

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

	NConcurrency::TCFuture<void> CFastCGIServer::CInternal::f_StartListenAddress
		(
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
			, NContainer::TCVector<NNetwork::CNetAddress> _Addresses
		)
	{
		if (mp_pOnRequest)
			co_return DMibErrorInstance("Already started");
		mp_pOnRequest = fg_Construct(fg_Move(_fOnRequest));

		umint nThreads = _Addresses.f_GetLen();

		NContainer::TCVector<NConcurrency::TCActor<NFastCGI::CListenActor>> ListenSockets;
		ListenSockets.f_SetLen(nThreads);
		auto Cleanup = g_OnScopeExit / [&]
			{
				for (auto &ListenSocket : ListenSockets)
				{
					if (ListenSocket)
						fg_Move(ListenSocket).f_Destroy().f_DiscardResult();
				}
			}
		;

		for (umint i = 0; i < nThreads; ++i)
		{
			NNetwork::CNetAddress Address = _Addresses[i];

			NConcurrency::TCActor<CListenActor> &ListenActor = ListenSockets[i];
			ListenActor = NConcurrency::fg_ConstructActor<CListenActor>(fg_ThisActor(mp_pThis), mp_pOnRequest);

			NNetwork::CSocket ListenSocket;
			{
				auto CaptureScope = co_await (NConcurrency::g_CaptureExceptions % NStr::CStr("Failed to listen in FastCGI server"));
				ListenSocket.f_Listen
					(
						Address
						, [WeakListenActor = ListenActor.f_Weak()](NNetwork::ENetTCPState _StateAdded)
						{
							auto ListenActor = WeakListenActor.f_Lock();
							if (!ListenActor)
								return;
							ListenActor.f_Bind<&CListenActor::f_StateAdded>(_StateAdded).f_DiscardResult();
						}
						, NNetwork::ENetFlag_None
					)
				;
			}

			NStorage::TCSharedPointer<NNetwork::CSocket> pSocket = fg_Construct(fg_Move(ListenSocket));

			co_await ListenActor(&CListenActor::f_SetSocket, pSocket);
		}

		mp_ListenSockets = fg_Move(ListenSockets);

		co_return {};
	}

	NConcurrency::TCFuture<void> CFastCGIServer::CInternal::f_Start
		(
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
			, uint16 _FastCGIListenStartPort
			, uint16 _nListen
			, NNetwork::CNetAddress _BindAddress
		)
	{
		NContainer::TCVector<NNetwork::CNetAddress> Addresses;

		umint nThreads = _nListen;
		uint16 StartListen = _FastCGIListenStartPort;

		for (umint i = 0; i < nThreads; ++i)
		{
			NNetwork::CNetAddress Address = _BindAddress;
			Address.f_SetPort(StartListen + i);
			Addresses.f_Insert(fg_Move(Address));
		}

		co_await f_StartListenAddress(fg_Move(_fOnRequest), fg_Move(Addresses));

		co_return {};
	}

	void CFastCGIServer::fp_AddConnection(NConcurrency::TCActor<CFastCGIConnectionActor> _Connection)
	{
		auto &Internal = *mp_pInternal;
		Internal.mp_Connections[fg_Move(_Connection)];
	}

	void CFastCGIServer::fp_RemoveConnection(NConcurrency::TCWeakActor<CFastCGIConnectionActor> _Connection)
	{
		auto Connection = _Connection.f_Lock();
		auto &Internal = *mp_pInternal;
		if (Internal.mp_Connections.f_Remove(_Connection))
		{
			if (Connection)
				fg_Move(Connection).f_Destroy() > Internal.mp_pCanDestroyTracker->f_Track();
		}
	}

	NConcurrency::TCFuture<void> CFastCGIServer::fp_Destroy()
	{
		NConcurrency::CLogError LogError("FastCGIServer");

		auto &Internal = *mp_pInternal;

		{
			auto pCanDestroy = fg_Move(Internal.mp_pCanDestroyTracker);
			auto CanDestroyFuture = fg_Exchange(pCanDestroy, nullptr)->f_Future();

			co_await fg_Move(CanDestroyFuture).f_Wrap() > LogError.f_Warning("Failed to destroy can destroy tracker");
		}

		NConcurrency::TCFutureVector<void> DestroyResults;

		for (auto& ListenSocket : Internal.mp_ListenSockets)
		fg_Move(ListenSocket).f_Destroy() > DestroyResults;

		Internal.mp_ListenSockets.f_Clear();

		Internal.mp_Connections.f_ExtractAll
			(
				[&](auto &&_Handle)
				{
					fg_Move(*_Handle).f_Destroy() > DestroyResults;
				}
			)
		;

		co_await fg_AllDone(DestroyResults).f_Wrap() > LogError.f_Warning("Failed to destroy fast CGI server");;

		co_return {};
	}
}
