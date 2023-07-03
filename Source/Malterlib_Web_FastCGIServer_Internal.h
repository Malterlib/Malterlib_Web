// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include "Malterlib_Web_FastCGIServer_Listen.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"

#include <Mib/Concurrency/ConcurrencyDefines>

namespace NMib::NWeb
{
	class CFastCGIServer::CInternal : public NConcurrency::CActorInternal
	{
	public:
		CInternal(CFastCGIServer *_pThis);
		~CInternal();

		NConcurrency::TCFuture<void> f_Start
			(
				NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
				, uint16 _FastCGIListenStartPort
				, uint16 _nListen
				, NNetwork::CNetAddress _BindAddress
			)
		;

		NConcurrency::TCFuture<void> f_StartListenAddress
			(
				NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
				, NContainer::TCVector<NNetwork::CNetAddress> _Addresses
			)
		;

		void f_Construct();

	private:
		friend class CFastCGIRequest;
		friend class CFastCGIConnectionActor;
		friend class CFastCGIServer;

		CFastCGIServer *mp_pThis;
		NContainer::TCVector<NConcurrency::TCActor<NFastCGI::CListenActor>> mp_ListenSockets;
		NContainer::TCSet<NConcurrency::TCActor<CFastCGIConnectionActor>> mp_Connections;
		NStorage::TCSharedPointer<NConcurrency::CCanDestroyTracker> mp_pCanDestroyTracker = fg_Construct();
		NStorage::TCSharedPointer<NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)>> mp_pOnRequest;
	};
}
