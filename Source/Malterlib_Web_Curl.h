// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>
#include <Mib/Encoding/EJSON>

namespace NMib::NWeb
{
	struct CCurlActor : public NConcurrency::CActor
	{
		struct CActorHolder : public NConcurrency::CSeparateThreadActorHolder
		{
			struct CInternal;

			CActorHolder
				(
					NConcurrency::CConcurrencyManager *_pConcurrencyManager
					, bool _bImmediateDelete
					, NConcurrency::EPriority _Priority
					, NStorage::TCSharedPointer<NConcurrency::ICDistributedActorData> &&_pDistributedActorData
					, NStr::CStr const &_ThreadName
				)
			;
			~CActorHolder();

			NStorage::TCUniquePointer<CInternal> m_pInternal;

		protected:
			void fp_StartQueueProcessing() override;
			void fp_DestroyThreaded() override;
			void fp_QueueProcessDestroy(NConcurrency::FActorQueueDispatch &&_Functor, NConcurrency::CConcurrencyThreadLocal &_ThreadLocal) override;
			void fp_QueueProcess(NConcurrency::FActorQueueDispatch &&_Functor, NConcurrency::CConcurrencyThreadLocal &_ThreadLocal) override;
			void fp_Wakeup();
		};

		struct CState;
		 
		struct CCertificateConfig
		{
			NContainer::CByteVector m_ClientCertificate;
			NContainer::CSecureByteVector m_ClientKey;
			NContainer::CByteVector m_CertificateAuthorities;
		};

		CCurlActor(CCertificateConfig const &_CertificateConfig = {});
		~CCurlActor();

		enum EMethod
		{
			EMethod_GET
			, EMethod_HEAD
			, EMethod_POST
			, EMethod_PATCH
			, EMethod_PUT
			, EMethod_DELETE
		};

		struct CRequest
		{
			NStr::CStr m_URL;
			EMethod m_Method = EMethod_GET;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Headers;
			NContainer::CByteVector m_Data;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Cookies;
			uint64 m_ReadDataSize = 0;
			bool m_bFollowRedirects = false;
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<NContainer::CByteVector> (mint _nBytes)> m_fReadData;
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector &&_Data)> m_fWriteData;
		};

		struct CResult
		{
			CResult(CState const &_State);
			
			uint32 m_StatusCode = 0;
			NStr::CStr m_StatusMessage;
			NContainer::TCMap<NStr::CStr, NStr::CStr, NStr::CCompareNoCase> m_Headers;
			NStr::CStr m_Body;
			
			NEncoding::CEJSONSorted f_ToJson() const;
		};
		
		NConcurrency::TCFuture<CResult> f_Request
			(
				EMethod _Method
				, NStr::CStr const &_URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Headers
				, NContainer::CByteVector const &_Data
				, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Cookies
			)
		;

		NConcurrency::TCFuture<CResult> f_ExecuteRequest(CRequest &&_Request);

	private:
		struct CInternal;

		NConcurrency::TCFuture<void> fp_Destroy() override;

		CActorHolder *fp_GetActorHolder();
		NConcurrency::TCFuture<void> fp_RequestFinished(NStr::CStr const &_RequestID, int32 _ResultCode, NException::CExceptionPointer &&_pException);

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
