// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyDefines>

namespace NMib
{
	namespace NWeb
	{
		class CFastCGIConnectionActor;
		
		class CFastCGIRequest
		{
		public:
			CFastCGIRequest(NConcurrency::TCActor<CFastCGIConnectionActor> const& _ConnectionActor, CFastCGIConnectionActor& _InternalActor);
			~CFastCGIRequest();
			
			// All callbacks called in the context of the fast CGI processing loop.
			// You should try not to do any blocking calls from this callback as this will
			// limit the concurrency of the web server.
			// You should only call these functions from the 
			
			void f_OnStdInputRaw(NFunction::TCFunction<void (uint8 const* _pData, mint _Len, bool _bEOF)>&& _fCallback);
			void f_OnData(NFunction::TCFunction<void (uint8 const* _pData, mint _Len, bool _bEOF)>&& _fCallback);
			void f_OnStdInput(NFunction::TCFunction<void (NStr::CStr const& _Input, bool _bEOF)>&& _fCallback);
			void f_OnAbort(NFunction::TCFunction<void ()>&& _fCallback);
			
			NContainer::TCMap<NStr::CStr, NStr::CStr> const& f_GetParams();
			
			void f_SendStdOutput(NStr::CStr const& _Output);
			void f_SendStdError(NStr::CStr const& _Output);
			
			void f_SendStdOutput(uint8 const* _pOutput, mint _Len);
			void f_SendStdError(uint8 const* _pOutput, mint _Len);
			
			void f_FinishRequest();
			
		private:
			NConcurrency::TCActor<CFastCGIConnectionActor> mp_pActor;
			CFastCGIConnectionActor& mp_InternalActor;
			NPtr::TCSharedPointer<NConcurrency::CCanDestroyTracker> mp_pCanDestroyTracker;
			zbool mp_bFinished;
		};
		
		class CFastCGIServer
		{
		public:
			class CInternal;
			
			// When the request goes out of scope it will be automatically finished
			CFastCGIServer(NFunction::TCFunction<void (NPtr::TCSharedPointer<CFastCGIRequest> const& _Request)>&& _fOnRequest);
			~CFastCGIServer();
			
		private:
			NConcurrency::TCActor<CInternal> mp_pInternal;
		};	
	}
}

#ifndef DMibPNoShortCuts
using namespace NMib::NWeb;
#endif
