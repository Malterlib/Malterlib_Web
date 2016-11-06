// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/WeakActor>

#include "Malterlib_Web_WebSocket.h"

namespace NMib
{
	namespace NWeb
	{
		namespace NWebSocket
		{
			class CListenActor : public NConcurrency::CActor
			{
			public:
				CListenActor(NConcurrency::TCActor<CWebSocketServerActor> const& _Server, mint _MaxMesageSize, mint _FragmentationSize, fp64 _Timeout);
				~CListenActor();
				
				void f_SetSocket(NPtr::TCUniquePointer<NNet::ICSocket> &&_pSocket);
				NConcurrency::TCContinuation<void> f_Destroy();
				void f_StateAdded(NNet::ENetTCPState _StateAdded);
				
			private:
				void fp_ProcessState();
				
			private:
				NPtr::TCUniquePointer<NNet::ICSocket> mp_pSocket;
				mint mp_MaxMessageSize;
				mint mp_FragmentationSize;
				fp64 mp_Timeout;
				NConcurrency::TCWeakActor<CWebSocketServerActor> mp_Server;
			};
		}
	}
}

