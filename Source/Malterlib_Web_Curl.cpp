// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_Curl.h"
#include <Mib/Cryptography/Certificate>
#include <Mib/Cryptography/RandomID>
#include <Mib/Web/HTTP/URL>

#define CURL_STRICTER

#include <curl/curl.h>

#ifndef DPlatformFamily_Linux

extern "C"
{
	void curl_ca_external_fallback(X509_STORE *_pStore)
	{
		NMib::NCryptography::CCertificate::fs_GetSystemCertificates(_pStore);
	}
}

#endif

namespace NMib::NWeb
{
	using namespace NContainer;
	using namespace NStorage;
	using namespace NStr;

	namespace
	{
		struct CDeleterHelper
		{
			void operator()(CURLM *_pObject)
			{
				curl_multi_cleanup(_pObject);
			}

			void operator()(Curl_easy *_pObject)
			{
				curl_easy_cleanup(_pObject);
			}

			void operator()(curl_slist *_pObject)
			{
				curl_slist_free_all(_pObject);
			}
		};

		using CCurlDeleter = NMemory::TCAllocator_FunctorDeleter<CDeleterHelper>;

		class CCurlInit
		{
		public:
			CCurlInit()
			{
				curl_global_init(CURL_GLOBAL_ALL);
			}

			~CCurlInit()
			{
				curl_global_cleanup();
			}
		};

		constinit NStorage::TCAggregate<CCurlInit, 129> g_CurlInit = {DAggregateInit};
	}

	struct CCurlActor::CActorHolder::CInternal
	{
		TCUniquePointer<CURLM, CCurlDeleter> m_pMulti;
	};

	struct CCurlActor::CState
	{
		CByteVector m_Headers;
		CByteVector m_Body;
	};

	struct CCurlActor::CInternal
	{
		CInternal(CCertificateConfig const &_CertificateConfig)
			: m_CertificateConfig(_CertificateConfig)
		{
		}

		struct CRequest
		{
			~CRequest();

			CStr const &f_GetID() const
			{
				return TCMap<CStr, CRequest>::fs_GetKey(*this);
			}

			CCurlActor *m_pActor = nullptr;
			TCUniquePointer<Curl_easy, CCurlDeleter> m_pCurl;
			TCUniquePointer<curl_slist , CCurlDeleter> m_pHeaders;
			CState m_State;
			NContainer::CByteVector m_Data;
			NContainer::CByteVector::CIteratorConst m_iData;
			CStr m_CurlErrorBuffer;
			CStr m_CookieStr;
			NConcurrency::TCPromise<CCurlActor::CResult> m_FinishedPromise;
			bool m_bAddedHandle = false;
		};

		CCertificateConfig m_CertificateConfig;

		TCMap<CStr, CRequest> m_Requests;
	};

	CCurlActor::CResult::CResult(CState const &_State)
		: m_Body(CStr((ch8 const *)_State.m_Body.f_GetArray(), _State.m_Body.f_GetLen()))
	{
		CStr HeaderStr((ch8 const *)_State.m_Headers.f_GetArray(), _State.m_Headers.f_GetLen());

		m_StatusCode = 300;
		m_StatusMessage = "Failed to parse status message";
		CStr Status = fg_GetStrLineSep(HeaderStr);
		CStr HttpVersion;
		aint nParsed;
		(void)
			(
				NMib::NStr::CStrPtr::CParse("HTTP/{} {} {}")
				>> HttpVersion
				>> m_StatusCode
				>> m_StatusMessage
			)
			.f_Parse(Status, nParsed)
		;

		while (!HeaderStr.f_IsEmpty())
		{
			CStr Line = fg_GetStrLineSep(HeaderStr);
			CStr Key = fg_GetStrSep(Line, ": ").f_LowerCase();
			m_Headers[Key] = Line;
		}
	}

	NEncoding::CEJSON CCurlActor::CResult::f_ToJSON() const
	{
		return NEncoding::CEJSON::fs_FromString(m_Body);
	}

	CCurlActor::CActorHolder::CActorHolder
		(
			NConcurrency::CConcurrencyManager *_pConcurrencyManager
			, bool _bImmediateDelete
			, NConcurrency::EPriority _Priority
			, NStorage::TCSharedPointer<NConcurrency::ICDistributedActorData> &&_pDistributedActorData
			, NStr::CStr const &_ThreadName
		)
		: NConcurrency::CSeparateThreadActorHolder(_pConcurrencyManager, _bImmediateDelete, _Priority, fg_Move(_pDistributedActorData), _ThreadName)
		, m_pInternal(fg_Construct())
	{
	}

	CCurlActor::CActorHolder::~CActorHolder() = default;

	void CCurlActor::CActorHolder::fp_StartQueueProcessing()
	{
		*g_CurlInit;

		auto &Internal = *m_pInternal;
		Internal.m_pMulti = fg_Explicit(curl_multi_init());

		if (!Internal.m_pMulti)
			DMibError("Failed to initialize multi");

		m_pThread = NThread::CThreadObject::fs_StartThread
			(
				[this](NThread::CThreadObject *_pThread) -> aint
				{
					auto &Internal = *m_pInternal;
					auto *pMultiHandle = Internal.m_pMulti.f_Get();

					while (_pThread->f_GetState() != NThread::EThreadState_EventWantQuit)
					{
						NTime::CCyclesClock Clock;
						Clock.f_Start();
						while (true)
						{
							fp_RunProcess();
							int RunningHandles;
							curl_multi_perform(pMultiHandle, &RunningHandles);

							{
								auto pCurlActor = static_cast<CCurlActor *>(fp_GetActorRelaxed());

								while (true)
								{
									int MessagesInQueue = 0;
									auto pMessage = curl_multi_info_read(pMultiHandle, &MessagesInQueue);
									if (!pMessage)
										break;

									if (pMessage->msg == CURLMSG_DONE)
									{
										Curl_easy *pEasyHandle = pMessage->easy_handle;

										void *pRawRequest = nullptr;
										curl_easy_getinfo(pEasyHandle, CURLINFO_PRIVATE, &pRawRequest);

										CCurlActor::CInternal::CRequest *pRequest = fg_AutoStaticCast(pRawRequest);

										fg_ThisActor(pCurlActor)(&CCurlActor::fp_RequestFinished, pRequest->f_GetID(), pMessage->data.result) > NConcurrency::fg_DiscardResult();
									}
								}
							}

							if (Clock.f_GetTime() > 0.000035) // Run for at least 35 µs
								break;
						}

						curl_multi_poll(pMultiHandle, NULL, 0, TCLimitsInt<int>::mc_Max, NULL);
					}
					return 0;
				}
				, mp_ThreadName
				, f_ConcurrencyManager().f_GetExecutionPriority(f_GetPriority())
			)
		;
	}

	void CCurlActor::CActorHolder::fp_Wakeup()
	{
		auto &Internal = *m_pInternal;
		curl_multi_wakeup(Internal.m_pMulti.f_Get());
	}

	void CCurlActor::CActorHolder::fp_QueueProcessDestroy(NConcurrency::FActorQueueDispatch &&_Functor, NConcurrency::CConcurrencyThreadLocal &_ThreadLocal)
	{
		if (fp_AddToQueue(fg_Move(_Functor), _ThreadLocal))
			fp_Wakeup();
	}

	void CCurlActor::CActorHolder::fp_QueueProcess(NConcurrency::FActorQueueDispatch &&_Functor, NConcurrency::CConcurrencyThreadLocal &_ThreadLocal)
	{
		// Reference this so it doesn't go out of scope if queue is processed before thread has been notified
		NConcurrency::TCActorHolderSharedPointer<CActorHolder> pThis = fg_Explicit(this);

		if (fp_AddToQueue(fg_Move(_Functor), _ThreadLocal))
			fp_Wakeup();
	}

	void CCurlActor::CActorHolder::fp_DestroyThreaded()
	{
		m_pThread->f_Stop(false);
		fp_Wakeup();
		m_pThread.f_Clear();

		auto &Internal = *m_pInternal;
		Internal.m_pMulti.f_Clear();

		CDefaultActorHolder::fp_DestroyThreaded();
	}

	CCurlActor::CInternal::CRequest::~CRequest()
	{
		if (m_bAddedHandle)
		{
			auto pActorHolder = m_pActor->fp_GetActorHolder();
			auto &HolderInternal = *pActorHolder->m_pInternal;
			curl_multi_remove_handle(HolderInternal.m_pMulti.f_Get(), m_pCurl.f_Get());
		}
	}

	CCurlActor::CCurlActor(CCertificateConfig const &_CertificateConfig)
		: mp_pInternal(fg_Construct(_CertificateConfig))
	{
	}

	CCurlActor::~CCurlActor() = default;

	auto CCurlActor::fp_GetActorHolder() -> CActorHolder *
	{
		return static_cast<CActorHolder *>(self.m_pThis.f_Get());
	}

	NConcurrency::TCFuture<void> CCurlActor::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		for (auto &Request : Internal.m_Requests)
		{
			if (Request.m_FinishedPromise.f_IsSet())
				continue;

			Request.m_FinishedPromise.f_SetException(DMibErrorInstance("Aborted request"));
		}

		Internal.m_Requests.f_Clear();

		co_return {};
	}

	NConcurrency::TCFuture<void> CCurlActor::fp_RequestFinished(CStr const &_RequestID, int32 _ResultCode)
	{
		auto &Internal = *mp_pInternal;
		CURLcode ResultCode = (CURLcode)_ResultCode;

		auto *pRequest = Internal.m_Requests.f_FindEqual(_RequestID);
		if (!pRequest)
			co_return {};

		auto &Request = *pRequest;

		if (!Request.m_FinishedPromise.f_IsSet())
		{
			if (ResultCode != CURLE_OK)
			{
				auto pEasyError = curl_easy_strerror(ResultCode);
				auto pExtraError = pEasyError ? pEasyError : "";
				CStr FullError = pExtraError;
				CStr CurlError = Request.m_CurlErrorBuffer.f_GetStr();
				if (CurlError)
					fg_AddStrSep(FullError, CurlError, ". ");
				Request.m_FinishedPromise.f_SetException(DMibErrorInstance(fg_Format("libcurl failed ({}): {}", ResultCode, FullError)));
			}
			else
				Request.m_FinishedPromise.f_SetResult(CCurlActor::CResult(Request.m_State));
		}

		Internal.m_Requests.f_Remove(pRequest);

		co_return {};
	}

	NConcurrency::TCFuture<CCurlActor::CResult> CCurlActor::f_Request
		(
			EMethod _Method
			, NStr::CStr const &_URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Headers
			, NContainer::CByteVector const &_Data
			, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Cookies
		)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Curl actor shutting down");

		try
		{
			auto &Internal = *mp_pInternal;

			auto RequestID = NCryptography::fg_RandomID(Internal.m_Requests);
			auto &Request = Internal.m_Requests[RequestID];
			Request.m_pActor = this;
			Request.m_pCurl = fg_Explicit(curl_easy_init());
			Request.m_Data = _Data;
			Request.m_iData = fg_Const(Request.m_Data).f_GetIterator();
			Request.m_CurlErrorBuffer.f_CreateWritableBuffer(CURL_ERROR_SIZE, true);

			auto Cleanup = g_OnScopeExit / [&]
				{
				}
			;

			if (!Request.m_pCurl)
				DMibError("libcurl was not initialised");

			Curl_easy *pCurl = Request.m_pCurl.f_Get();

			auto fCheckResult = [&](CURLcode _Result) -> void
				{
					if (_Result != CURLE_OK)
					{
						auto pEasyError = curl_easy_strerror(_Result);
						auto pExtraError = pEasyError ? pEasyError  : "";
						CStr FullError = pExtraError;
						CStr CurlError = Request.m_CurlErrorBuffer.f_GetStr();
						if (CurlError)
							fg_AddStrSep(FullError, CurlError, ". ");
						DMibError(fg_Format("libcurl operation on {} failed ({}): {}", _URL, _Result, FullError));
					}
				}
			;

			curl_easy_setopt(pCurl, CURLOPT_ERRORBUFFER, Request.m_CurlErrorBuffer.f_GetStr());

			curl_slist *pHeaders = NULL;
			auto CleanupHeaders = g_OnScopeExit / [&]
				{
					curl_slist_free_all(pHeaders);
				}
			;

			//fCheckResult(curl_easy_setopt(pCurl, CURLOPT_VERBOSE, 1L));
			fCheckResult(curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L));
			fCheckResult(curl_easy_setopt(pCurl, CURLOPT_PRIVATE, &Request));

			for (auto &Cookie : _Cookies)
				Request.m_CookieStr += "{}={}; "_f << _Cookies.fs_GetKey(Cookie) << Cookie;

			if (Request.m_CookieStr)
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_COOKIE, Request.m_CookieStr.f_GetStr()));

			if (!_Headers.f_FindEqual("Accept"))
				pHeaders = curl_slist_append(pHeaders, "Accept: application/json");

			if (!_Headers.f_FindEqual("Content-Type"))
				pHeaders = curl_slist_append(pHeaders, "Content-Type: application/json");

			if (!_Headers.f_FindEqual("Expect"))
				pHeaders = curl_slist_append(pHeaders, "Expect:");

			if (!Internal.m_CertificateConfig.m_ClientCertificate.f_IsEmpty())
			{
				struct curl_blob CertBlob;
				CertBlob.data = Internal.m_CertificateConfig.m_ClientCertificate.f_GetArray();
				CertBlob.len = Internal.m_CertificateConfig.m_ClientCertificate.f_GetLen();
				CertBlob.flags = CURL_BLOB_NOCOPY;
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLCERT_BLOB, &CertBlob));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLCERTTYPE, "PEM"));
			}

			if (!Internal.m_CertificateConfig.m_ClientKey.f_IsEmpty())
			{
				struct curl_blob CertKeyBlob;
				CertKeyBlob.data = Internal.m_CertificateConfig.m_ClientKey.f_GetArray();
				CertKeyBlob.len = Internal.m_CertificateConfig.m_ClientKey.f_GetLen();
				CertKeyBlob.flags = CURL_BLOB_NOCOPY;
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLKEY_BLOB, &CertKeyBlob));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLCERTTYPE, "PEM"));
			}

			if (!Internal.m_CertificateConfig.m_CertificateAuthorities.f_IsEmpty())
			{
				struct curl_blob CertAuthBlob;
				CertAuthBlob.data = Internal.m_CertificateConfig.m_CertificateAuthorities.f_GetArray();
				CertAuthBlob.len = Internal.m_CertificateConfig.m_CertificateAuthorities.f_GetLen();
				CertAuthBlob.flags = CURL_BLOB_NOCOPY;
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_CAINFO_BLOB, &CertAuthBlob));
			}

			if (_Method == EMethod_POST)
			{
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_POST, 1L));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_POSTFIELDS, _Data.f_GetArray()));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_POSTFIELDSIZE, _Data.f_GetLen()));
			}
			else if (_Method == EMethod_PUT || _Method == EMethod_PATCH)
			{
				if (_Method == EMethod_PATCH)
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_CUSTOMREQUEST, "PATCH"));

				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_UPLOAD, 1L));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_READDATA, &Request));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_INFILESIZE, _Data.f_GetLen()));

				auto fReadCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
					{
						CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);
						size_t Bytes = _Size * _nItems;

						if (Bytes > 0)
						{
							NContainer::CByteVector::CIteratorConst &iData = pRequest->m_iData;
							Bytes = fg_Min(iData.f_GetLen(), Bytes);
							NMemory::fg_MemCopy(_pBuffer, &*iData, Bytes);
							iData += Bytes;
						}

						return Bytes;
					}
				;

				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_READFUNCTION, (curl_read_callback)fReadCallback));
			}
			else if (_Method == EMethod_DELETE)
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_CUSTOMREQUEST, "DELETE"));
			else if (_Method == EMethod_HEAD)
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_NOBODY, 1));

			NHTTP::CURL Url(_URL);
			CStr UrlHost = Url.f_GetHost();
			if (UrlHost.f_StartsWith("UNIX:"))
			{
				auto UnixPath = UrlHost.f_RemovePrefix("UNIX:");
				Url.f_SetHost("localhost");
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_UNIX_SOCKET_PATH, UnixPath.f_GetStr()));
				auto NewURL = Url.f_Encode();
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_URL, NewURL.f_GetStr()));
			}
			else
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_URL, _URL.f_GetStr()));

			for (auto &Header : _Headers)
			{
				CStr HeaderStr(fg_Format("{}: {}", _Headers.fs_GetKey(Header), Header));
				pHeaders = curl_slist_append(pHeaders, HeaderStr.f_GetStr());
			}
			fCheckResult(curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pHeaders));

			CleanupHeaders.f_Clear();
			if (pHeaders)
				Request.m_pHeaders = fg_Explicit(pHeaders);

			fCheckResult(curl_easy_setopt(pCurl, CURLOPT_HEADERDATA, &Request));
			auto fWriteHeaderCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
				{
					CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);

					pRequest->m_State.m_Headers.f_Insert((uint8 const *)_pBuffer, _Size * _nItems);

					return _Size * _nItems;
				}
			;
			fCheckResult(curl_easy_setopt(pCurl, CURLOPT_HEADERFUNCTION, (curl_write_callback)fWriteHeaderCallback));

			fCheckResult(curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, &Request));
			auto fWriteBodyCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
				{
					CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);

					pRequest->m_State.m_Body.f_Insert((uint8 const *)_pBuffer, _Size * _nItems);

					return _Size * _nItems;
				}
			;
			fCheckResult(curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, (curl_write_callback)fWriteBodyCallback));

			auto *pActorHolder = fp_GetActorHolder();

			auto &HolderInternal = *pActorHolder->m_pInternal;
			curl_multi_add_handle(HolderInternal.m_pMulti.f_Get(), pCurl);
			Request.m_bAddedHandle = true;

			co_return co_await Request.m_FinishedPromise.f_Future();
		}
		catch (NException::CException const &)
		{
			co_return NException::fg_CurrentException();
		}
	}
}
