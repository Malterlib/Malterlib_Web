// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/ActorFunctorWeak>

#include "Malterlib_Web_FastCGIServer.h"

namespace NMib::NWeb::NFastCGI
{
	class CListenActor : public NConcurrency::CActor
	{
	public:
		CListenActor
			(
				NConcurrency::TCActor<CFastCGIServer> const &_Server
				, NStorage::TCSharedPointer<NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)>> const &_pOnRequest
			)
		;
		~CListenActor();

		void f_SetSocket(NStorage::TCSharedPointer<NNetwork::CSocket>const& _pSocket);
		void f_StateAdded(NNetwork::ENetTCPState _StateAdded);

	private:

		NConcurrency::TCFuture<void> fp_Destroy();
		void fp_ProcessState();

	private:
		NNetwork::CSocket mp_Socket;
		NConcurrency::TCActor<CFastCGIServer> mp_Server;
		NStorage::TCSharedPointer<NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)>> mp_pOnRequest;
	};
}
