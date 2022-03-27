// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
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
			void fp_QueueProcessDestroy(NConcurrency::FActorQueueDispatch &&_Functor) override;
			void fp_QueueProcess(NConcurrency::FActorQueueDispatch &&_Functor) override;
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
		
		struct CResult
		{
			CResult(CState const &_State);
			
			uint32 m_StatusCode;
			NStr::CStr m_StatusMessage;
			NContainer::TCMap<NStr::CStr, NStr::CStr, NStr::CCompareNoCase> m_Headers;
			NStr::CStr m_Body;
			
			NEncoding::CEJSON f_ToJSON() const;
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

	private:
		struct CInternal;

		NConcurrency::TCFuture<void> fp_Destroy() override;

		CActorHolder *fp_GetActorHolder();
		NConcurrency::TCFuture<void> fp_RequestFinished(NStr::CStr const &_RequestID, int32 _ResultCode);

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
