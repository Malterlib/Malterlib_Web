// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>

#include "Malterlib_Web_FastCGIServer.h"

namespace NMib::NWeb::NFastCGI
{
	class CListenActor : public NConcurrency::CActor
	{
	public:
		CListenActor(NConcurrency::TCActor<CFastCGIServer::CInternal> const& _Server, CFastCGIServer::CInternal& _ServerInternal);
		~CListenActor();

		void f_SetSocket(NStorage::TCSharedPointer<NNetwork::CSocket>const& _pSocket);
		void f_StateAdded(NNetwork::ENetTCPState _StateAdded);

	private:

		NConcurrency::TCContinuation<void> fp_Destroy();
		void fp_ProcessState();

	private:
		NNetwork::CSocket mp_Socket;
		NConcurrency::TCActor<CFastCGIServer::CInternal> mp_Server;
		CFastCGIServer::CInternal& mp_ServerInternal;
	};
}
