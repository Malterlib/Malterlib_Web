// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/ActorFunctorWeak>

#include "Malterlib_Web_FastCGIServer.h"
#include "Malterlib_Web_FastCGIServer_Protocol.h"

namespace NMib::NWeb
{
	class CFastCGIConnectionActor : public NConcurrency::CActor
	{
		friend class CFastCGIRequest;
	public:
		CFastCGIConnectionActor
			(
				NConcurrency::TCActor<CFastCGIServer> const &_ServerActor
				, NStorage::TCSharedPointer<NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)>> const &_pOnRequest
			)
		;
		~CFastCGIConnectionActor();

		void f_SetSocket(NStorage::TCSharedPointer<NNetwork::CSocket> _pSocket);
		void f_StateAdded(NNetwork::ENetTCPState _StateAdded);

		NConcurrency::TCFuture<void> f_SendStdOutput(NContainer::CIOByteVector _Data);
		NConcurrency::TCFuture<void> f_SendStdError(NContainer::CIOByteVector _Data);

		void f_Accept();

		void f_FinishRequest();

		void f_OnStdInputRaw(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CIOByteVector _Data, bool _bEOF)> &&_fCallback);
		void f_OnData(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CIOByteVector _Data, bool _bEOF)> &&_fCallback);
		void f_OnStdInput(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStr::CStr _Input, bool _bEOF)> &&_fCallback);
		void f_OnAbort(NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> ()> &&_fCallback);

	private:
		NConcurrency::TCFuture<void> fp_Destroy();

		void fp_Disconnect(ch8 const* _pReason);
		void fp_ProcessState(NNetwork::ENetTCPState _ForceState = NNetwork::ENetTCPState_None);
		void fp_UpdateSend();

		void fp_ClearState();

		void fp_SendData(uint8 const* _pData, umint _Len);
		void fp_SendData(NContainer::CIOByteVector const& _Data);

		void fp_SendStdOutput(NContainer::CIOByteVector const& _Data, NFastCGI::ERequestType _Type);

		bool fp_ProcessManagementRecord(NFastCGI::CHeader const& _Header, uint8 const* _pData, umint _DataLen);
		bool fp_ProcessBeginRequest(NFastCGI::CHeader const& _Header, uint8 const* _pData, umint _DataLen);
		bool fp_ProcessStreamData(NFastCGI::CHeader const& _Header, uint8 const* _pData, umint _DataLen);

		void fp_OnStdIn(uint8 const* _pData, umint _Len);
		void fp_OnStdData(uint8 const* _pData, umint _Len);
		void fp_OnParams(NContainer::TCMap<NStr::CStr, NStr::CStr> const& _Params);
		void fp_OnParams();
		void fp_OnAbort();

		struct CRecordInfo
		{
			CRecordInfo();

			NFastCGI::ERequestType m_Type;
			uint16 m_ID;
			bool m_bBegun = false;
			bool m_bInvalid = false;
			bool m_bKeepConnection = false;
			bool m_bFinished = false;
			bool m_bSentStdOut = false;
			bool m_bSentStdErr = false;
			NFastCGI::ERequestRole m_Role;
		};
	private:
		NConcurrency::TCActor<CFastCGIServer> mp_ServerActor;
		NNetwork::CSocket mp_Socket;

		NContainer::CIOByteVector mp_IncomingData;
		umint mp_IncomingPosition;
		umint mp_NeededData;
		NFastCGI::CHeader mp_CurrentHeader;
		CRecordInfo mp_RecordInfo;
		NStorage::TCSharedPointer<NContainer::TCMap<NStr::CStr, NStr::CStr>> mp_pParams = fg_Construct();

		NContainer::CIOByteVector mp_OutgoingData;
		umint mp_OutgoingPosition;

		NStorage::TCSharedPointer<NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> _pRequest)>> mp_pOnRequest;

		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CIOByteVector _Data, bool _bEOF)> mp_fOnStdInputRaw;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NContainer::CIOByteVector _Data, bool _bEOF)> mp_fOnData;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStr::CStr _Input, bool _bEOF)> mp_fOnStdInputStr;
		NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> ()> mp_fOnAbort;

		bool mp_bConnectionRemoved = false;
		bool mp_bAcceptInput = true;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
