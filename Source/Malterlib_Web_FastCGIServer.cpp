// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_FastCGIServer.h"
#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"

namespace NMib
{
	namespace NWeb
	{
		///
		/// Server
		/// ======
		
		CFastCGIServer::CFastCGIServer(NFunction::TCFunction<void (NPtr::TCSharedPointer<CFastCGIRequest> const& _Request)>&& _fOnRequest, uint16 _FastCGIListenStartPort, uint16 _nListen)
			: mp_pInternal(NConcurrency::fg_ConstructActor<CInternal>(fg_Move(_fOnRequest), _FastCGIListenStartPort, _nListen))
		{
		}
		
		CFastCGIServer::~CFastCGIServer()
		{
			mp_pInternal->f_BlockDestroy();
		}

		///
		/// Request
		/// =======
		
		CFastCGIRequest::CFastCGIRequest(NConcurrency::TCActor<CFastCGIConnectionActor> const& _ConnectionActor, CFastCGIConnectionActor& _InternalActor)
			: mp_pActor(_ConnectionActor)
			, mp_InternalActor(_InternalActor)
			, mp_pCanDestroyTracker(mp_InternalActor.mp_ServerInternal.mp_pCanDestroyTracker)
		{
			
		}
		CFastCGIRequest::~CFastCGIRequest()
		{
			if (!mp_bFinished)
				f_FinishRequest();
		}
		
		void CFastCGIRequest::f_OnStdInputRaw(NFunction::TCFunction<void (uint8 const* _pData, mint _Len, bool _bEOF)>&& _fCallback)
		{
			DMibRequire(!mp_bFinished);
			DMibRequire(mp_InternalActor.mp_ProcessingThread == NSys::fg_Thread_GetCurrentUID());
			mp_InternalActor.mp_fOnStdInputRaw = fg_Move(_fCallback);
		}
		void CFastCGIRequest::f_OnData(NFunction::TCFunction<void (uint8 const* _pData, mint _Len, bool _bEOF)>&& _fCallback)
		{
			DMibRequire(!mp_bFinished);
			DMibRequire(mp_InternalActor.mp_ProcessingThread == NSys::fg_Thread_GetCurrentUID());
			mp_InternalActor.mp_fOnData = fg_Move(_fCallback);
		}
		void CFastCGIRequest::f_OnStdInput(NFunction::TCFunction<void (NStr::CStr const& _Input, bool _bEOF)>&& _fCallback)
		{
			DMibRequire(!mp_bFinished);
			DMibRequire(mp_InternalActor.mp_ProcessingThread == NSys::fg_Thread_GetCurrentUID());
			mp_InternalActor.mp_fOnStdInputStr = fg_Move(_fCallback);
		}
		void CFastCGIRequest::f_OnAbort(NFunction::TCFunction<void ()>&& _fCallback)
		{
			DMibRequire(!mp_bFinished);
			DMibRequire(mp_InternalActor.mp_ProcessingThread == NSys::fg_Thread_GetCurrentUID());
			mp_InternalActor.mp_fOnAbort = fg_Move(_fCallback);
		}
		NContainer::TCMap<NStr::CStr, NStr::CStr> const& CFastCGIRequest::f_GetParams()
		{
			return mp_InternalActor.mp_Params;
		}

		void CFastCGIRequest::f_SendStdOutput(NStr::CStr const& _Output)
		{
			DMibRequire(!mp_bFinished);
			auto Output = fg_ForceStrUTF8(_Output);
			f_SendStdOutput((uint8 const*)Output.f_GetStr(), Output.f_GetLen());
		}
		void CFastCGIRequest::f_SendStdError(NStr::CStr const& _Output)
		{
			DMibRequire(!mp_bFinished);
			auto Output = fg_ForceStrUTF8(_Output);
			f_SendStdError((uint8 const*)Output.f_GetStr(), Output.f_GetLen());
		}
		
		void CFastCGIRequest::f_SendStdOutput(uint8 const* _pOutput, mint _Len)
		{
			DMibRequire(!mp_bFinished);
			NContainer::TCVector<uint8> Data;
			Data.f_Insert(_pOutput, _Len);
			
			mp_pActor(&CFastCGIConnectionActor::f_SendStdOutput, Data)
				> NConcurrency::fg_DiscardResult()
			;
		}
		void CFastCGIRequest::f_SendStdError(uint8 const* _pOutput, mint _Len)
		{
			DMibRequire(!mp_bFinished);
			NContainer::TCVector<uint8> Data;
			Data.f_Insert(_pOutput, _Len);
			
			mp_pActor(&CFastCGIConnectionActor::f_SendStdError, Data)
				> NConcurrency::fg_DiscardResult()
			;
		}
		
		void CFastCGIRequest::f_FinishRequest()
		{
			DMibRequire(!mp_bFinished);
			mp_bFinished = true;
			mp_pActor(&CFastCGIConnectionActor::f_FinishRequest)
				> NConcurrency::fg_DiscardResult()
			;
		}
	}
	
}

