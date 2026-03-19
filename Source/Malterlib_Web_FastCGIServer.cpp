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
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
			, uint16 _FastCGIListenStartPort
			, uint16 _nListen
			, NNetwork::CNetAddress _BindAddress
		)
	{
		return mp_pInternal->f_Start(fg_Move(_fOnRequest), _FastCGIListenStartPort, _nListen, _BindAddress);
	}

	NConcurrency::TCFuture<void> CFastCGIServer::f_StartListenAddress
		(
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
			, NContainer::TCVector<NNetwork::CNetAddress> _Addresses
		)
	{
		return mp_pInternal->f_StartListenAddress(fg_Move(_fOnRequest), fg_Move(_Addresses));
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

	void CFastCGIRequest::f_OnStdInputRaw(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CByteVector _Data, bool _bEOF)> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_OnStdInputRaw>(fg_Move(_fCallback)).f_DiscardResult();
	}

	void CFastCGIRequest::f_OnData(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CByteVector _Data, bool _bEOF)> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_OnData>(fg_Move(_fCallback)).f_DiscardResult();
	}

	void CFastCGIRequest::f_OnStdInput(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStr::CStr _Input, bool _bEOF)> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_OnStdInput>(fg_Move(_fCallback)).f_DiscardResult();
	}

	void CFastCGIRequest::f_OnAbort(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> ()> &&_fCallback)
	{
		DMibRequire(!mp_bFinished);
		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_OnAbort>(fg_Move(_fCallback)).f_DiscardResult();
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

	void CFastCGIRequest::f_SendStdOutput(uint8 const* _pOutput, umint _Len)
	{
		DMibRequire(!mp_bFinished);
		NContainer::CByteVector Data;
		Data.f_Insert(_pOutput, _Len);

		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_SendStdOutput>(Data).f_DiscardResult();
	}

	void CFastCGIRequest::f_SendStdError(uint8 const* _pOutput, umint _Len)
	{
		DMibRequire(!mp_bFinished);
		NContainer::CByteVector Data;
		Data.f_Insert(_pOutput, _Len);

		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_SendStdError>(Data).f_DiscardResult();
	}

	NConcurrency::TCFuture<void> CFastCGIRequest::f_SendAsyncStdOutput(NContainer::CIOByteVector &&_Data)
	{
		return mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_SendStdOutput>(fg_Move(_Data));
	}

	NConcurrency::TCFuture<void> CFastCGIRequest::f_SendAsyncStdError(NContainer::CIOByteVector &&_Data)
	{
		return mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_SendStdError>(fg_Move(_Data));
	}

	NConcurrency::TCFuture<void> CFastCGIRequest::f_SendAsyncStdOutput(NStr::CStr const& _Output)
	{
		return mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_SendStdOutput>(NContainer::CIOByteVector::fs_FromString(_Output));
	}

	NConcurrency::TCFuture<void> CFastCGIRequest::f_SendAsyncStdError(NStr::CStr const& _Output)
	{
		return mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_SendStdError>(NContainer::CIOByteVector::fs_FromString(_Output));
	}

	void CFastCGIRequest::f_FinishRequest()
	{
		DMibRequire(!mp_bFinished);
		mp_bFinished = true;
		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_FinishRequest>().f_DiscardResult();
	}

	void CFastCGIRequest::f_Accept()
	{
		DMibRequire(!mp_bAccepted);
		mp_bAccepted = true;
		mp_ConnectionActor.f_Bind<&CFastCGIConnectionActor::f_Accept>().f_DiscardResult();
	}
}
