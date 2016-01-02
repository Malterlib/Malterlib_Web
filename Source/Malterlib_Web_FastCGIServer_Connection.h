// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/ConcurrencyDefines>

#include "Malterlib_Web_FastCGIServer.h"
#include "Malterlib_Web_FastCGIServer_Protocol.h"

namespace NMib
{
	namespace NWeb
	{
		class CFastCGIConnectionActor : public NConcurrency::CActor
		{
			friend class CFastCGIRequest;
		public:
			CFastCGIConnectionActor(NConcurrency::TCActor<CFastCGIServer::CInternal> const& _pServer, CFastCGIServer::CInternal& _ServerInternal);
			~CFastCGIConnectionActor();
			
			void f_SetSocket(NPtr::TCSharedPointer<NNet::CSocket>const& _pSocket);
			NConcurrency::TCContinuation<void> f_Destroy();
			void f_StateAdded(NNet::ENetTCPState _StateAdded);
			
			void f_SendStdOutput(NContainer::TCVector<uint8> const& _Data);
			void f_SendStdError(NContainer::TCVector<uint8> const& _Data);
			
			void f_FinishRequest();
			
		private:
			void fp_Disconnect(ch8 const* _pReason);
			void fp_ProcessState();
			void fp_UpdateSend();
			
			void fp_ClearState();
			
			void fp_SendData(uint8 const* _pData, mint _Len);
			void fp_SendData(NContainer::TCVector<uint8> const& _Data);

			void fp_SendStdOutput(NContainer::TCVector<uint8> const& _Data, NFastCGI::ERequestType _Type);
			
			bool fp_ProcessManagementRecord(NFastCGI::CHeader const& _Header, uint8 const* _pData, mint _DataLen);
			bool fp_ProcessBeginRequest(NFastCGI::CHeader const& _Header, uint8 const* _pData, mint _DataLen);
			bool fp_ProcessStreamData(NFastCGI::CHeader const& _Header, uint8 const* _pData, mint _DataLen);
			
			void fp_OnStdIn(uint8 const* _pData, mint _Len);
			void fp_OnStdData(uint8 const* _pData, mint _Len);
			void fp_OnParams(NContainer::TCMap<NStr::CStr, NStr::CStr> const& _Params);
			void fp_OnAbort();
			
			struct CRecordInfo
			{
				CRecordInfo();
				
				NFastCGI::ERequestType m_Type;
				uint16 m_ID;
				zbool m_bBegun;
				zbool m_bInvalid;
				zbool m_bKeepConnection;
				zbool m_bFinished;
				zbool m_bSentStdOut;
				zbool m_bSentStdErr;
				NFastCGI::ERequestRole m_Role;
			};
		private:
			NConcurrency::TCActor<CFastCGIServer::CInternal> mp_pServer;
			CFastCGIServer::CInternal& mp_ServerInternal;
			NNet::CSocket mp_Socket;

			NContainer::TCVector<uint8> mp_IncomingData;
			mint mp_IncomingPosition;
			mint mp_NeededData;
			NFastCGI::CHeader mp_CurrentHeader;
			CRecordInfo mp_RecordInfo;
			NContainer::TCMap<NStr::CStr, NStr::CStr> mp_Params;
			
			NContainer::TCVector<uint8> mp_OutgoingData;
			mint mp_OutgoingPosition;
			
			NFunction::TCFunction<void (uint8 const* _pData, mint _Len, bool _bEOF)> mp_fOnStdInputRaw;
			NFunction::TCFunction<void (uint8 const* _pData, mint _Len, bool _bEOF)> mp_fOnData;
			NFunction::TCFunction<void (NStr::CStr const& _Input, bool _bEOF)> mp_fOnStdInputStr;
			NFunction::TCFunction<void ()> mp_fOnAbort;
			
			mint mp_ProcessingThread;
		};
	}
}


#ifndef DMibPNoShortCuts
using namespace NMib::NWeb;
#endif
