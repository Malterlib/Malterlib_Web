// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>
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

		void f_OnStdInputRaw(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> &&_fCallback);
		void f_OnData(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> &&_fCallback);
		void f_OnStdInput(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStr::CStr const& _Input, bool _bEOF)> &&_fCallback);
		void f_OnAbort(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> ()> &&_fCallback);

		NContainer::TCMap<NStr::CStr, NStr::CStr> const &f_GetParams();

		void f_SendStdOutput(NStr::CStr const& _Output);
		void f_SendStdError(NStr::CStr const& _Output);

		void f_SendStdOutput(uint8 const* _pOutput, mint _Len);
		void f_SendStdError(uint8 const* _pOutput, mint _Len);

		void f_FinishRequest();

	private:
		NConcurrency::TCActor<CFastCGIConnectionActor> mp_ConnectionActor;
		NStorage::TCSharedPointer<NContainer::TCMap<NStr::CStr, NStr::CStr>> mp_pParams;
		bool mp_bFinished = false;
	};

	class CFastCGIServer : public NConcurrency::CActor
	{
	public:
		class CInternal;

		CFastCGIServer();
		~CFastCGIServer();

		NConcurrency::TCFuture<void> f_Start
			(
				NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)> &&_fOnRequest
				, uint16 _FastCGIListenStartPort
				, uint16 _nListen
				, NNetwork::CNetAddress const &_BindAddress = NNetwork::CNetAddressTCPv4(NNetwork::CNetAddressIPv4(127, 0, 0, 1), 0)
			)
		;

	private:
		friend class CFastCGIConnectionActor;
		friend class NFastCGI::CListenActor;

		NConcurrency::TCFuture<void> fp_Destroy();
		void fp_AddConnection(NConcurrency::TCActor<CFastCGIConnectionActor> &&_Connection);
		void fp_RemoveConnection(NConcurrency::TCWeakActor<CFastCGIConnectionActor> &&_Connection);

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NWeb;
#endif
