// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Listen.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"

namespace NMib
{
	namespace NWeb
	{
		using namespace NFastCGI;
		CFastCGIServer::CInternal::CInternal(NFunction::TCFunction<void (NPtr::TCSharedPointer<CFastCGIRequest> const& _Request)>&& _fOnRequest)
			: mp_pCanDestroyTracker(fg_Construct())
			, mp_fOnRequest(fg_Move(_fOnRequest))
		{
		}
		
		CFastCGIServer::CInternal::~CInternal()
		{
		}

		void CFastCGIServer::CInternal::f_Construct()
		{
			fp_Startup();
		}
		
		void CFastCGIServer::CInternal::f_AddConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const& _Connection)
		{
			mp_Connections[_Connection];
		}
		
		void CFastCGIServer::CInternal::f_RemoveConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const& _Connection)
		{
			if (mp_Connections.f_Remove(_Connection))
			{
				auto pCanDestroy = mp_pCanDestroyTracker;
				_Connection->f_Destroy([pCanDestroy](NConcurrency::TCAsyncResult<void>&& _Result) {});
			}
		}
		
		NConcurrency::TCContinuation<void> CFastCGIServer::CInternal::f_Destroy()
		{
			auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
			
			for (auto& ListenSocket : mp_ListenSockets)
				ListenSocket->f_Destroy([pCanDestroy](NConcurrency::TCAsyncResult<void>&& _Result){});
			
			mp_ListenSockets.f_Clear();
			
			for (auto& Connection : mp_Connections)
				Connection->f_Destroy([pCanDestroy](NConcurrency::TCAsyncResult<void>&& _Result){});
			mp_Connections.f_Clear();
			
			return pCanDestroy->m_Continuation;
		}
			
		void CFastCGIServer::CInternal::fp_Startup()
		{
			mint nThreads = NSys::fg_Thread_GetVirtualCores();
			uint16 StartListen = 9000;
			
			mp_ListenSockets.f_SetLen(nThreads);

			auto ConcurrentActor = NConcurrency::fg_ConcurrentActor();
			for (mint i = 0; i < nThreads; ++i)
			{
				NNet::CNetAddressTCPv4 AnyAddress;
				AnyAddress.m_Port = StartListen + i;
				NNet::CNetAddress Address;
				Address.f_Set(AnyAddress);

				NConcurrency::TCActor<CListenActor> &ListenActor = mp_ListenSockets[i];
				ListenActor = NConcurrency::fg_ConstructActor<CListenActor>(fg_ThisActor(this), *this);
				
				NNet::CSocket ListenSocket;
				ListenSocket.f_Listen
					(
						Address
						, [ListenActor, ConcurrentActor](NNet::ENetTCPState _StateAdded)
						{
							ListenActor(&CListenActor::f_StateAdded, _StateAdded)
								> ConcurrentActor / [](NConcurrency::TCAsyncResult<void>&& _Result)
								{
								}
							;
						}
					)
				;
				
				NPtr::TCSharedPointer<NNet::CSocket> pSocket = fg_Construct(fg_Move(ListenSocket));
				
				ListenActor(&CListenActor::f_SetSocket, pSocket)
					> ConcurrentActor / [](NConcurrency::TCAsyncResult<void>&& _Result)
					{
					}
				;
				
			}			
		}
	}
}
