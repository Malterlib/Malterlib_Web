// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>

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
				, NStorage::TCSharedPointer<NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)>> const &_pOnRequest
			)
		;
		~CFastCGIConnectionActor();

		void f_SetSocket(NStorage::TCSharedPointer<NNetwork::CSocket>const& _pSocket);
		void f_StateAdded(NNetwork::ENetTCPState _StateAdded);

		void f_SendStdOutput(NContainer::CByteVector const& _Data);
		void f_SendStdError(NContainer::CByteVector const& _Data);

		void f_FinishRequest();

		void f_OnStdInputRaw(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> &&_fCallback);
		void f_OnData(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> &&_fCallback);
		void f_OnStdInput(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStr::CStr const& _Input, bool _bEOF)> &&_fCallback);
		void f_OnAbort(NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> ()> &&_fCallback);

	private:
		NConcurrency::TCFuture<void> fp_Destroy();

		void fp_Disconnect(ch8 const* _pReason);
		void fp_ProcessState();
		void fp_UpdateSend();

		void fp_ClearState();

		void fp_SendData(uint8 const* _pData, mint _Len);
		void fp_SendData(NContainer::CByteVector const& _Data);

		void fp_SendStdOutput(NContainer::CByteVector const& _Data, NFastCGI::ERequestType _Type);

		bool fp_ProcessManagementRecord(NFastCGI::CHeader const& _Header, uint8 const* _pData, mint _DataLen);
		bool fp_ProcessBeginRequest(NFastCGI::CHeader const& _Header, uint8 const* _pData, mint _DataLen);
		bool fp_ProcessStreamData(NFastCGI::CHeader const& _Header, uint8 const* _pData, mint _DataLen);

		void fp_OnStdIn(uint8 const* _pData, mint _Len);
		void fp_OnStdData(uint8 const* _pData, mint _Len);
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

		NContainer::CByteVector mp_IncomingData;
		mint mp_IncomingPosition;
		mint mp_NeededData;
		NFastCGI::CHeader mp_CurrentHeader;
		CRecordInfo mp_RecordInfo;
		NStorage::TCSharedPointer<NContainer::TCMap<NStr::CStr, NStr::CStr>> mp_pParams = fg_Construct();

		NContainer::CByteVector mp_OutgoingData;
		mint mp_OutgoingPosition;

		NStorage::TCSharedPointer<NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest)>> mp_pOnRequest;

		NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> mp_fOnStdInputRaw;
		NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data, bool _bEOF)> mp_fOnData;
		NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NStr::CStr const& _Input, bool _bEOF)> mp_fOnStdInputStr;
		NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> ()> mp_fOnAbort;

		bool mp_bConnectionRemoved = false;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
