// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctorWeak>
#include <Mib/Network/Address>

namespace NMib::NWeb
{
	class CFastCGIConnectionActor;
	namespace NFastCGI
	{
		class CListenActor;
	}

	class CFastCGIRequest
	{
	public:
		CFastCGIRequest(NConcurrency::TCActor<CFastCGIConnectionActor> const &_ConnectionActor, NStorage::TCSharedPointer<NContainer::TCMap<NStr::CStr, NStr::CStr>> const &_pParams);
		~CFastCGIRequest();

		void f_OnStdInputRaw(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CByteVector _Data, bool _bEOF)> &&_fCallback);
		void f_OnData(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CByteVector _Data, bool _bEOF)> &&_fCallback);
		void f_OnStdInput(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStr::CStr _Input, bool _bEOF)> &&_fCallback);
		void f_OnAbort(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> ()> &&_fCallback);
		void f_Accept(); // Call after setting the callbacks to continue processing incoming data

		NContainer::TCMap<NStr::CStr, NStr::CStr> const &f_GetParams();

		void f_SendStdOutput(NStr::CStr const& _Output);
		void f_SendStdError(NStr::CStr const& _Output);

		void f_SendStdOutput(uint8 const* _pOutput, umint _Len);
		void f_SendStdError(uint8 const* _pOutput, umint _Len);

		NConcurrency::TCFuture<void> f_SendAsyncStdOutput(NContainer::CIOByteVector &&_Data);
		NConcurrency::TCFuture<void> f_SendAsyncStdError(NContainer::CIOByteVector &&_Data);

		NConcurrency::TCFuture<void> f_SendAsyncStdOutput(NStr::CStr const& _Output);
		NConcurrency::TCFuture<void> f_SendAsyncStdError(NStr::CStr const& _Output);

		void f_FinishRequest();
		bool f_IsFinished() const;

	private:
		NConcurrency::TCActor<CFastCGIConnectionActor> mp_ConnectionActor;
		NStorage::TCSharedPointer<NContainer::TCMap<NStr::CStr, NStr::CStr>> mp_pParams;
		bool mp_bFinished = false;
		bool mp_bAccepted = false;
	};

	class CFastCGIServer : public NConcurrency::CActor
	{
	public:
		class CInternal;

		CFastCGIServer();
		~CFastCGIServer();

		NConcurrency::TCFuture<void> f_StartListenAddress
			(
				NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
				, NContainer::TCVector<NNetwork::CNetAddress> _Addresses
			)
		;

		NConcurrency::TCFuture<void> f_Start
			(
				NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)> _fOnRequest
				, uint16 _FastCGIListenStartPort
				, uint16 _nListen
				, NNetwork::CNetAddress _BindAddress = NNetwork::CNetAddressTCPv4(NNetwork::CNetAddressIPv4(127, 0, 0, 1), 0)
			)
		;

	private:
		friend class CFastCGIConnectionActor;
		friend class NFastCGI::CListenActor;

		NConcurrency::TCFuture<void> fp_Destroy();
		void fp_AddConnection(NConcurrency::TCActor<CFastCGIConnectionActor> _Connection);
		void fp_RemoveConnection(NConcurrency::TCWeakActor<CFastCGIConnectionActor> _Connection);

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NWeb;
#endif
