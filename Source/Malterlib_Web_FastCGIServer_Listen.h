// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>

#include "Malterlib_Web_FastCGIServer.h"

namespace NMib
{
	namespace NWeb
	{
		namespace NFastCGI
		{
			class CListenActor : public NConcurrency::CActor
			{
			public:
				CListenActor(NConcurrency::TCActor<CFastCGIServer::CInternal> const& _Server, CFastCGIServer::CInternal& _ServerInternal);
				~CListenActor();
				
				void f_SetSocket(NPtr::TCSharedPointer<NNet::CSocket>const& _pSocket);
				void f_StateAdded(NNet::ENetTCPState _StateAdded);
				
			private:
				
				NConcurrency::TCContinuation<void> fp_Destroy();
				void fp_ProcessState();
				
			private:
				NNet::CSocket mp_Socket;
				NConcurrency::TCActor<CFastCGIServer::CInternal> mp_Server;
				CFastCGIServer::CInternal& mp_ServerInternal;
			};
		}
	}
}

