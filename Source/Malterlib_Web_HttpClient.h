// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>
#include <Mib/Encoding/EJson>

namespace NMib::NWeb
{
	struct CHttpClientActor : public NConcurrency::CActor
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
			void fp_QueueRunProcess(NConcurrency::CConcurrencyThreadLocal &_ThreadLocal) override;
			void fp_QueueProcess(NConcurrency::FActorQueueDispatch &&_Functor, NConcurrency::CConcurrencyThreadLocal &_ThreadLocal) override;
			void fp_QueueProcessEntry(NConcurrency::CConcurrentRunQueueEntryHolder &&_Entry, NConcurrency::CConcurrencyThreadLocal &_ThreadLocal) override;
			void fp_QueueJob(NConcurrency::FActorQueueDispatchNoAlloc &&_ToQueue, NConcurrency::CConcurrencyThreadLocal &_ThreadLocal);
			void fp_Wakeup();
		};

		struct CState;

		struct CCertificateConfig
		{
			NContainer::CByteVector m_ClientCertificate;
			NContainer::CSecureByteVector m_ClientKey;
			NContainer::CByteVector m_CertificateAuthorities;
		};

		enum EMethod : uint8
		{
			EMethod_GET
			, EMethod_HEAD
			, EMethod_POST
			, EMethod_PATCH
			, EMethod_PUT
			, EMethod_DELETE
		};

		struct CAsyncReadData
		{
			int64 m_Size = -1;
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<NContainer::CByteVector> (mint _nBytes)> m_fRead;
		};

		struct CAsyncWriteData
		{
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NContainer::CByteVector _Data)> m_fWrite;
		};

		struct CRequest
		{
			CAsyncReadData &f_AsyncSend();
			CAsyncWriteData &f_AsyncReceive();

			NStr::CStr m_URL;

			NContainer::TCMap<NStr::CStr, NStr::CStr, NStr::CCompareNoCase> m_Headers;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Cookies;

			NStorage::TCVariant<void, NContainer::CByteVector, CAsyncReadData> m_SendData;
			NStorage::TCVariant<void, CAsyncWriteData> m_ReceiveData;

			EMethod m_Method = EMethod_GET;
			bool m_bFollowRedirects = false;
			bool m_bSetDefaultHeaders = true;
		};

		struct CResult
		{
			CResult(CState const &_State);

			NEncoding::CEJsonSorted f_ToJson() const;

			NStr::CStr m_StatusMessage;
			NContainer::TCMap<NStr::CStr, NStr::CStr, NStr::CCompareNoCase> m_Headers;
			NStr::CStr m_Body;
			uint32 m_StatusCode = 0;
		};

		using CRequestData = NStorage::TCVariant<NContainer::CByteVector, NStr::CStr, NEncoding::CEJsonSorted, NEncoding::CEJsonOrdered, NEncoding::CJsonSorted, NEncoding::CJsonOrdered>;

		CHttpClientActor(CCertificateConfig const &_CertificateConfig = {});
		~CHttpClientActor();

		NConcurrency::TCFuture<CResult> f_Get
			(
				NStr::CStr _URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
			)
		;

		NConcurrency::TCFuture<CResult> f_Head
			(
				NStr::CStr _URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
			)
		;

		NConcurrency::TCFuture<CResult> f_Post
			(
				NStr::CStr _URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
				, CRequestData _Data
			)
		;

		NConcurrency::TCFuture<CResult> f_Patch
			(
				NStr::CStr _URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
				, CRequestData _Data
			)
		;

		NConcurrency::TCFuture<CResult> f_Put
			(
				NStr::CStr _URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
				, CRequestData _Data
			)
		;

		NConcurrency::TCFuture<CResult> f_Delete
			(
				NStr::CStr _URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
			)
		;

		NConcurrency::TCFuture<CResult> f_SendRequest(CRequest _Request);

	private:
		struct CInternal;

		void fp_Construct() override;
		NConcurrency::TCFuture<void> fp_Destroy() override;

		CActorHolder *fp_GetActorHolder();
		NConcurrency::TCFuture<void> fp_RequestFinished(NStr::CStr _RequestID, int32 _ResultCode, NException::CExceptionPointer _pException);

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};

	struct CHttpClientRequestExceptionData
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		static CHttpClientRequestExceptionData fs_FromResult(CHttpClientActor::CResult const &_Result);

		uint32 m_StatusCode = 0;
		NStr::CStr m_StatusMessage;
	};

	DMibImpErrorSpecificClassDefine(CHttpClientRequestException, NMib::NException::CException, CHttpClientRequestExceptionData);

#	define DMibErrorWebRequest(d_Description, d_Specific) DMibImpErrorSpecific(NMib::NWeb::CHttpClientRequestException, d_Description, d_Specific)
#	define DMibErrorInstanceWebRequest(d_Description, d_Specific) DMibImpExceptionInstanceSpecific(NMib::NWeb::CHttpClientRequestException, d_Description, d_Specific)
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif

#include "Malterlib_Web_HttpClient.hpp"
