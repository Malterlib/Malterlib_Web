// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include "Malterlib_Web_FastCGIServer_Listen.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"

#include <Mib/Concurrency/ConcurrencyDefines>

namespace NMib
{
	namespace NWeb
	{
		class CFastCGIServer::CInternal : public NConcurrency::CActor
		{
			friend class CFastCGIRequest;
			friend class CFastCGIConnectionActor;
		public:
			CInternal(NFunction::TCFunction<void (NPtr::TCSharedPointer<CFastCGIRequest> const& _Request)>&& _fOnRequest);
			~CInternal();
			
			void f_Construct();
			
			void f_AddConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const& _Connection);
			void f_RemoveConnection(NConcurrency::TCActor<CFastCGIConnectionActor> const& _Connection);
			
		private:

			NConcurrency::TCContinuation<void> fp_Destroy();
			void fp_Startup();
			
		private:
			NContainer::TCVector<NConcurrency::TCActor<NFastCGI::CListenActor>> mp_ListenSockets;
			NContainer::TCSet<NConcurrency::TCActor<CFastCGIConnectionActor>> mp_Connections;
			NPtr::TCSharedPointer<NConcurrency::CCanDestroyTracker> mp_pCanDestroyTracker;
			NFunction::TCFunction<void (NPtr::TCSharedPointer<CFastCGIRequest> const& _Request)> mp_fOnRequest;
		};	
	}
}
