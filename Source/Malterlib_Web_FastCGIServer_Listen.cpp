// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/WeakActor>

#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Listen.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"
#include "Malterlib_Web_FastCGIServer_Protocol.h"


namespace NMib
{
	namespace NWeb
	{
		namespace NFastCGI
		{
			CListenActor::CListenActor(NConcurrency::TCActor<CFastCGIServer::CInternal> const& _Server, CFastCGIServer::CInternal& _ServerInternal)
				: mp_Server(_Server)
				, mp_ServerInternal(_ServerInternal)
			{
			}
			
			CListenActor::~CListenActor()
			{
			}
			
			void CListenActor::f_SetSocket(NPtr::TCSharedPointer<NNet::CSocket>const& _pSocket)
			{
				mp_Socket = fg_Move(*_pSocket);
				fp_ProcessState();
			}

			NConcurrency::TCContinuation<void> CListenActor::fp_Destroy()
			{
				mp_Socket.f_Close();
				return NConcurrency::TCContinuation<void>::fs_Finished();
			}
			
			void CListenActor::f_StateAdded(NNet::ENetTCPState _StateAdded)
			{
				if (mp_Socket.f_IsValid())
					fp_ProcessState();
			}
			
			void CListenActor::fp_ProcessState()
			{
				DMibFastCheck(mp_Socket.f_IsValid());
				auto StateAdded = mp_Socket.f_GetState();
				if (StateAdded & NNet::ENetTCPState_Connection)
				{
					NConcurrency::TCActor<CFastCGIConnectionActor> ConnectionActor = NConcurrency::fg_ConstructActor<CFastCGIConnectionActor>(mp_Server, mp_ServerInternal);
					NNet::CSocket AcceptedSocket;
					NConcurrency::TCWeakActor<CFastCGIConnectionActor> WeakConnectionActor = ConnectionActor;
					AcceptedSocket.f_Accept
						(
							&mp_Socket
							, [WeakConnectionActor, this](NNet::ENetTCPState _StateAdded)
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
					NPtr::TCSharedPointer<NNet::CSocket> pSocket = fg_Construct(fg_Move(AcceptedSocket));
					
					ConnectionActor(&CFastCGIConnectionActor::f_SetSocket, pSocket)
						> NConcurrency::fg_DiscardResult()
					;					

					mp_Server(&CFastCGIServer::CInternal::f_AddConnection, ConnectionActor)
						> NConcurrency::fg_DiscardResult()
					;
				}
			}
		}
	}
}

