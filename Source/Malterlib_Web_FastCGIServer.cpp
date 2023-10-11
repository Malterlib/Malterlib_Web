// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_FastCGIServer.h"
#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"

namespace NMib::NWeb
{
	///
	/// Server
	/// ======

	CFastCGIServer::CFastCGIServer()
		: mp_pInternal{fg_Construct(this)}
	{
	}

	CFastCGIServer::~CFastCGIServer()
	{
	}

	NConcurrency::TCFuture<void> CFastCGIServer::f_Start
		(
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)> &&_fOnRequest
			, uint16 _FastCGIListenStartPort
			, uint16 _nListen
			, NNetwork::CNetAddress const &_BindAddress
		)
	{
		return NConcurrency::fg_CallSafe(*mp_pInternal, &CInternal::f_Start, fg_Move(_fOnRequest), _FastCGIListenStartPort, _nListen, _BindAddress);
	}

	NConcurrency::TCFuture<void> CFastCGIServer::f_StartListenAddress
		(
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)> &&_fOnRequest
			, NContainer::TCVector<NNetwork::CNetAddress> &&_Addresses
		)
	{
		return NConcurrency::fg_CallSafe(*mp_pInternal, &CInternal::f_StartListenAddress, fg_Move(_fOnRequest), fg_Move(_Addresses));
	}

	///
	/// Request
	/// =======

	CFastCGIRequest::CFastCGIRequest
		(
			NConcurrency::TCActor<CFastCGIConnectionActor> const &_ConnectionActor
			, NStorage::TCSharedPointer<NContainer::TCMap<NStr::CStr, NStr::CStr>> const &_pParams
		)
		: mp_ConnectionActor(_ConnectionActor)
		, mp_pParams(_pParams)
	{
	}

	CFastCGIRequest::~CFastCGIRequest()
	{
		if (!mp_bFinished)
			f_FinishRequest();
	}

	bool CFastCGIRequest::f_IsFinished() const
	{
		return mp_bFinished;
	}

	void CFastCGIRequest::f_OnStdInputRaw(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor(&CFastCGIConnectionActor::f_OnStdInputRaw, fg_Move(_fCallback)) > NConcurrency::fg_DiscardResult();
	}

	void CFastCGIRequest::f_OnData(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor(&CFastCGIConnectionActor::f_OnData, fg_Move(_fCallback)) > NConcurrency::fg_DiscardResult();
	}

	void CFastCGIRequest::f_OnStdInput(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStr::CStr const& _Input, bool _bEOF)> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor(&CFastCGIConnectionActor::f_OnStdInput, fg_Move(_fCallback)) > NConcurrency::fg_DiscardResult();
	}

	void CFastCGIRequest::f_OnAbort(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> ()> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor(&CFastCGIConnectionActor::f_OnAbort, fg_Move(_fCallback)) > NConcurrency::fg_DiscardResult();
	}

	NContainer::TCMap<NStr::CStr, NStr::CStr> const& CFastCGIRequest::f_GetParams()
	{
		return *mp_pParams;
	}

	void CFastCGIRequest::f_SendStdOutput(NStr::CStr const& _Output)
	{
		DMibRequire(!mp_bFinished);
		f_SendStdOutput((uint8 const*)_Output.f_GetStr(), _Output.f_GetLen());
	}

	void CFastCGIRequest::f_SendStdError(NStr::CStr const& _Output)
	{
		DMibRequire(!mp_bFinished);
		f_SendStdError((uint8 const*)_Output.f_GetStr(), _Output.f_GetLen());
	}

	void CFastCGIRequest::f_SendStdOutput(uint8 const* _pOutput, mint _Len)
	{
		DMibRequire(!mp_bFinished);
		NContainer::CByteVector Data;
		Data.f_Insert(_pOutput, _Len);

		mp_ConnectionActor(&CFastCGIConnectionActor::f_SendStdOutput, Data) > NConcurrency::fg_DiscardResult();
	}

	void CFastCGIRequest::f_SendStdError(uint8 const* _pOutput, mint _Len)
	{
		DMibRequire(!mp_bFinished);
		NContainer::CByteVector Data;
		Data.f_Insert(_pOutput, _Len);

		mp_ConnectionActor(&CFastCGIConnectionActor::f_SendStdError, Data) > NConcurrency::fg_DiscardResult();
	}

	void CFastCGIRequest::f_FinishRequest()
	{
		DMibRequire(!mp_bFinished);
		mp_bFinished = true;
		mp_ConnectionActor(&CFastCGIConnectionActor::f_FinishRequest) > NConcurrency::fg_DiscardResult();
	}

	void CFastCGIRequest::f_Accept()
	{
		DMibRequire(!mp_bAccepted);
		mp_bAccepted = true;
		mp_ConnectionActor(&CFastCGIConnectionActor::f_Accept) > NConcurrency::fg_DiscardResult();
	}
}
