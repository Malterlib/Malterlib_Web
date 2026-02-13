// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HttpClient.h"

#include <Mib/Cryptography/Certificate>
#include <Mib/Cryptography/RandomID>
#include <Mib/Web/HTTP/URL>

#define CURL_STRICTER

#include <curl/curl.h>

extern "C"
{
	void curl_ca_external_fallback(X509_STORE *_pStore)
	{
		NMib::NCryptography::CCertificate::fs_GetSystemCertificates(_pStore);
	}
}

namespace NMib::NWeb
{
	DMibImpErrorClassImplement(CHttpClientRequestException);

	// For compatibilty with actor protocol
	DMibImpErrorSpecificClassParentDefine(CWebRequestException, CHttpClientRequestException, CHttpClientRequestExceptionData);
	DMibImpErrorClassImplement(CWebRequestException);

	CHttpClientRequestExceptionData CHttpClientRequestExceptionData::fs_FromResult(CHttpClientActor::CResult const &_Result)
	{
		return CHttpClientRequestExceptionData{.m_StatusCode = _Result.m_StatusCode, .m_StatusMessage = _Result.m_StatusMessage};
	}

	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NStorage;
	using namespace NStr;

	namespace
	{
		struct CDeleterHelperMulti
		{
			void operator()(CURLM *_pObject)
			{
				curl_multi_cleanup(_pObject);
			}
		};

		struct CDeleterHelperEasy
		{
			void operator()(CURL *_pObject)
			{
				curl_easy_cleanup(_pObject);
			}
		};

		struct CDeleterHelperSList
		{
			void operator()(curl_slist *_pObject)
			{
				curl_slist_free_all(_pObject);
			}
		};

		using CCurlDeleterMulti = NMemory::TCAllocator_FunctorDeleter<CDeleterHelperMulti>;
		using CCurlDeleterEasy = NMemory::TCAllocator_FunctorDeleter<CDeleterHelperEasy>;
		using CCurlDeleterSList = NMemory::TCAllocator_FunctorDeleter<CDeleterHelperSList>;

		template <CURLoption tf_Option, typename tf_CValue>
		CURLcode fg_CurlSetOpt(CURL *_pCurl, tf_CValue _Value)
		{
			static_assert(CURLOPTTYPE_LONG == 0);

			if constexpr (tf_Option < CURLOPTTYPE_OBJECTPOINT) // CURLOPTTYPE_LONG (0-9999)
				return curl_easy_setopt(_pCurl, tf_Option, static_cast<long>(_Value));
			else if constexpr (tf_Option >= CURLOPTTYPE_OBJECTPOINT && tf_Option < CURLOPTTYPE_FUNCTIONPOINT) // CURLOPTTYPE_OBJECTPOINT (10000-19999)
				return curl_easy_setopt(_pCurl, tf_Option, static_cast<void const *>(_Value));
			else if constexpr (tf_Option >= CURLOPTTYPE_FUNCTIONPOINT && tf_Option < CURLOPTTYPE_OFF_T) // CURLOPTTYPE_FUNCTIONPOINT (20000-29999)
			{
				static_assert(NTraits::cIsFunction<NTraits::TCRemovePointer<tf_CValue>>);
				return curl_easy_setopt(_pCurl, tf_Option, _Value);
			}
			else if constexpr (tf_Option >= CURLOPTTYPE_OFF_T && tf_Option < CURLOPTTYPE_BLOB) // CURLOPTTYPE_OFF_T (30000-39999)
				return curl_easy_setopt(_pCurl, tf_Option, static_cast<curl_off_t>(_Value));
			else if constexpr (tf_Option >= CURLOPTTYPE_BLOB && tf_Option < (CURLOPTTYPE_BLOB + (CURLOPTTYPE_BLOB - CURLOPTTYPE_OFF_T))) // CURLOPTTYPE_BLOB (40000-49999)
			{
				static_assert(NTraits::cIsSame<NTraits::TCRemovePointer<tf_CValue>, struct curl_blob>);
				return curl_easy_setopt(_pCurl, tf_Option, _Value);
			}
			else
				static_assert(tf_Option >= 0, "Unknown CURLOPTTYPE");
		}

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

	struct CHttpClientActor::CActorHolder::CInternal
	{
		TCUniquePointer<CURLM, CCurlDeleterMulti> m_pMulti;
		NThread::CEvent m_ActorCreatedEvent;
		NThread::CEvent m_ProcessingStartedEvent;
	};

	struct CHttpClientActor::CState
	{
		CByteVector m_Headers;
		CByteVector m_Body;
	};

	struct CHttpClientActor::CInternal
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

			CHttpClientActor *m_pActor = nullptr;
			TCSharedPointer<bool> m_pDeleted = fg_Construct(false);
			TCUniquePointer<CURL, CCurlDeleterEasy> m_pCurl;
			TCUniquePointer<curl_slist, CCurlDeleterSList> m_pHeaders;
			CState m_State;
			NContainer::CByteVector m_Data;
			NContainer::CByteVector::CIteratorConst m_iData;
			CStr m_CurlErrorBuffer;
			CStr m_CookieStr;
			TCPromise<CHttpClientActor::CResult> m_FinishedPromise;
			uint64 m_ReadDataSize = 0;
			TCActorFunctor<TCFuture<CByteVector> (mint _nBytes)> m_fReadData;
			TCActorFunctor<TCFuture<void> (CByteVector _Data)> m_fWriteData;
			NException::CExceptionPointer m_pWriteError;
			NException::CExceptionPointer m_pReadError;
			uint64 m_WriteDoneBytes = 0;
			int m_PauseMask = 0;
			bool m_bAddedHandle = false;
			bool m_bWriteDone = false;
		};

		CCertificateConfig m_CertificateConfig;

		TCMap<CStr, CRequest> m_Requests;
	};

	CHttpClientActor::CResult::CResult(CState const &_State)
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

	NEncoding::CEJsonSorted CHttpClientActor::CResult::f_ToJson() const
	{
		return NEncoding::CEJsonSorted::fs_FromString(m_Body);
	}

	CHttpClientActor::CActorHolder::CActorHolder
		(
			CConcurrencyManager *_pConcurrencyManager
			, bool _bImmediateDelete
			, EPriority _Priority
			, NStorage::TCSharedPointer<ICDistributedActorData> &&_pDistributedActorData
			, NStr::CStr const &_ThreadName
		)
		: CSeparateThreadActorHolder(_pConcurrencyManager, _bImmediateDelete, _Priority, fg_Move(_pDistributedActorData), _ThreadName)
		, m_pInternal(fg_Construct())
	{
	}

	CHttpClientActor::CActorHolder::~CActorHolder() = default;

	void CHttpClientActor::fp_Construct()
	{
		auto pActorHolder = fp_GetActorHolder();
		auto &HolderInternal = *pActorHolder->m_pInternal;
		HolderInternal.m_ActorCreatedEvent.f_SetSignaled();
		HolderInternal.m_ProcessingStartedEvent.f_Wait();
	}

	void CHttpClientActor::CActorHolder::fp_StartQueueProcessing()
	{
		*g_CurlInit;

		auto &Internal = *m_pInternal;
		Internal.m_pMulti = fg_Explicit(curl_multi_init());

		if (!Internal.m_pMulti)
			DMibError("Failed to initialize multi");

		DMibLock(mp_ThreadLock);
		mp_pThread = NThread::CThreadObject::fs_StartThread
			(
				[this](NThread::CThreadObject *_pThread) -> aint
				{
					auto &Internal = *m_pInternal;
					auto *pMultiHandle = Internal.m_pMulti.f_Get();

					CHttpClientActor *pHttpClientActor = nullptr;
					Internal.m_ActorCreatedEvent.f_Wait();
					{
						DMibLock(mp_ThreadLock);
						pHttpClientActor = static_cast<CHttpClientActor *>(fp_GetActorRelaxed());
					}

					auto &ThreadLocal = fg_ConcurrencyThreadLocal();

					DMibFastCheck(pHttpClientActor);
					CCurrentActorScope CurrentActorScope(ThreadLocal, this);

					Internal.m_ProcessingStartedEvent.f_SetSignaled();

					while (_pThread->f_GetState() != NThread::EThreadState_EventWantQuit)
					{
						fp_RunQueue(ThreadLocal);
						int RunningHandles;
						curl_multi_perform(pMultiHandle, &RunningHandles);

						{
							while (true)
							{
								int MessagesInQueue = 0;
								auto pMessage = curl_multi_info_read(pMultiHandle, &MessagesInQueue);
								if (!pMessage)
									break;

								if (pMessage->msg == CURLMSG_DONE)
								{
									CURL *pEasyHandle = pMessage->easy_handle;

									void *pRawRequest = nullptr;
									curl_easy_getinfo(pEasyHandle, CURLINFO_PRIVATE, &pRawRequest);

									CHttpClientActor::CInternal::CRequest *pRequest = fg_AutoStaticCast(pRawRequest);

									NException::CExceptionPointer pError;
									if (pRequest->m_pWriteError && pRequest->m_pReadError)
									{
										NException::CExceptionExceptionVectorData::CErrorCollector ErrorCollector;

										ErrorCollector.f_AddError(fg_Move(pRequest->m_pWriteError));
										ErrorCollector.f_AddError(fg_Move(pRequest->m_pReadError));

										pError = fg_Move(ErrorCollector).f_GetException();
									}
									else if (pRequest->m_pWriteError)
										pError = fg_Move(pRequest->m_pWriteError);
									else if (pRequest->m_pReadError)
										pError = fg_Move(pRequest->m_pReadError);

									fg_ThisActor(pHttpClientActor).f_Bind<&CHttpClientActor::fp_RequestFinished>(pRequest->f_GetID(), pMessage->data.result, fg_Move(pError)).f_DiscardResult();
								}
							}
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

	void CHttpClientActor::CActorHolder::fp_Wakeup()
	{
		auto &Internal = *m_pInternal;
		curl_multi_wakeup(Internal.m_pMulti.f_Get());
	}

	void CHttpClientActor::CActorHolder::fp_QueueJob(FActorQueueDispatchNoAlloc &&_ToQueue, CConcurrencyThreadLocal &_ThreadLocal)
	{
		auto pQueueEntry = CConcurrentRunQueueNonVirtualNoAlloc::fs_QueueEntry(fg_Move(_ToQueue));

		if (_ThreadLocal.m_pCurrentlyProcessingActorHolder == this && _ThreadLocal.m_bCurrentlyProcessingInActorHolder)
		{
			if (!_ThreadLocal.m_bForceNonLocal) [[likely]]
			{
				mp_JobQueue.f_AddToQueueLocal(fg_Move(pQueueEntry), mp_JobQueueLocal);
				return;
			}
		}
		mp_JobQueue.f_AddToQueue(fg_Move(pQueueEntry));

		mint Value = mp_JobQueueWorking.f_FetchAdd(1);
		if (Value == 0)
			fp_Wakeup();
	}

	void CHttpClientActor::CActorHolder::fp_QueueProcessDestroy(FActorQueueDispatch &&_Functor, CConcurrencyThreadLocal &_ThreadLocal)
	{
		// Make sure the memory isn't deallocated
		TCActorHolderWeakPointer<CActorHolder> pStayAlive = fg_Explicit(this);

		DMibLock(mp_ThreadLock);
		if (fp_AddToQueue(fg_Move(_Functor), _ThreadLocal))
		{
			fp_QueueJob
				(
					[this, pStayAlive = fg_Move(pStayAlive)](CConcurrencyThreadLocal &_ThreadLocal)
					{
						if (this->mp_Destroyed.f_Load() >= 3)
							return;

						this->fp_RunProcess(_ThreadLocal);
					}
					, _ThreadLocal
				)
			;
		}
	}

	void CHttpClientActor::CActorHolder::fp_QueueRunProcess(CConcurrencyThreadLocal &_ThreadLocal)
	{
		DMibFastCheck(m_RefCount.m_RefCount.f_Load() >= 0);
		fp_QueueJob
			(
				[pThis = TCActorHolderSharedPointer<CActorHolder>(fg_Explicit(this))](CConcurrencyThreadLocal &_ThreadLocal)
				{
					DMibFastCheck(pThis->m_RefCount.m_RefCount.f_Load() >= 0);
					pThis->fp_RunProcess(_ThreadLocal);
				}
				, _ThreadLocal
			)
		;
	}

	void CHttpClientActor::CActorHolder::fp_QueueProcess(FActorQueueDispatch &&_Functor, CConcurrencyThreadLocal &_ThreadLocal)
	{
		if (fp_AddToQueue(fg_Move(_Functor), _ThreadLocal))
		{
			DMibFastCheck(m_RefCount.m_RefCount.f_Load() >= 0);
			fp_QueueJob
				(
					[pThis = TCActorHolderSharedPointer<CActorHolder>(fg_Explicit(this))](CConcurrencyThreadLocal &_ThreadLocal)
					{
						DMibFastCheck(pThis->m_RefCount.m_RefCount.f_Load() >= 0);
						pThis->fp_RunProcess(_ThreadLocal);
					}
					, _ThreadLocal
				)
			;
		}
	}

	void CHttpClientActor::CActorHolder::fp_QueueProcessEntry(CConcurrentRunQueueEntryHolder &&_Entry, CConcurrencyThreadLocal &_ThreadLocal)
	{
		if (fp_AddToQueue(fg_Move(_Entry), _ThreadLocal))
		{
			DMibFastCheck(m_RefCount.m_RefCount.f_Load() >= 0);
			fp_QueueJob
				(
					[pThis = TCActorHolderSharedPointer<CActorHolder>(fg_Explicit(this))](CConcurrencyThreadLocal &_ThreadLocal)
					{
						DMibFastCheck(pThis->m_RefCount.m_RefCount.f_Load() >= 0);
						pThis->fp_RunProcess(_ThreadLocal);
					}
					, _ThreadLocal
				)
			;
		}
	}

	void CHttpClientActor::CActorHolder::fp_DestroyThreaded()
	{
		{
			DMibLock(mp_ThreadLock);

			auto &Internal = *m_pInternal;
			Internal.m_ActorCreatedEvent.f_SetSignaled();

			mp_pThread->f_Stop(false);
			fp_Wakeup();

			mp_pThread.f_Clear();

			Internal.m_pMulti.f_Clear();
		}

		CDefaultActorHolder::fp_DestroyThreaded();
	}

	CHttpClientActor::CInternal::CRequest::~CRequest()
	{
		*m_pDeleted = true;

		if (m_bAddedHandle)
		{
			auto pActorHolder = m_pActor->fp_GetActorHolder();
			auto &HolderInternal = *pActorHolder->m_pInternal;
			curl_multi_remove_handle(HolderInternal.m_pMulti.f_Get(), m_pCurl.f_Get());
		}
	}

	CHttpClientActor::CHttpClientActor(CCertificateConfig const &_CertificateConfig)
		: mp_pInternal(fg_Construct(_CertificateConfig))
	{
	}

	CHttpClientActor::~CHttpClientActor() = default;

	auto CHttpClientActor::fp_GetActorHolder() -> CActorHolder *
	{
		return static_cast<CActorHolder *>(self.m_pThis.f_Get());
	}

	TCFuture<void> CHttpClientActor::fp_Destroy()
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

	TCFuture<void> CHttpClientActor::fp_RequestFinished(CStr _RequestID, int32 _ResultCode, NException::CExceptionPointer _pException)
	{
		auto &Internal = *mp_pInternal;
		CURLcode ResultCode = (CURLcode)_ResultCode;

		auto *pRequest = Internal.m_Requests.f_FindEqual(_RequestID);
		if (!pRequest)
			co_return {};

		auto &Request = *pRequest;

		if (!Request.m_FinishedPromise.f_IsSet())
		{
			if (_pException)
				Request.m_FinishedPromise.f_SetException(fg_Move(_pException));
			else if (ResultCode != CURLE_OK)
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
				Request.m_FinishedPromise.f_SetResult(CHttpClientActor::CResult(Request.m_State));
		}

		Internal.m_Requests.f_Remove(pRequest);

		co_return {};
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_Request
		(
			EMethod _Method
			, NStr::CStr _URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
			, NContainer::CByteVector _Data
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Cookies
		)
	{
		return f_ExecuteRequest(CRequest{.m_URL = fg_Move(_URL), .m_Method = _Method, .m_Headers = fg_Move(_Headers), .m_Data = fg_Move(_Data), .m_Cookies = fg_Move(_Cookies)});
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_ExecuteRequest(CRequest _Request)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("HTTP client actor shutting down");

		auto CaptureScope = co_await g_CaptureExceptions;

		auto &Internal = *mp_pInternal;

		auto RequestID = NCryptography::fg_FastRandomID(Internal.m_Requests);
		auto &Request = Internal.m_Requests[RequestID];
		Request.m_pActor = this;
		Request.m_pCurl = fg_Explicit(curl_easy_init());
		Request.m_Data = fg_Move(_Request.m_Data);
		Request.m_iData = fg_Const(Request.m_Data).f_GetIterator();
		Request.m_CurlErrorBuffer.f_CreateWritableBuffer(CURL_ERROR_SIZE, true);
		Request.m_ReadDataSize = _Request.m_ReadDataSize;
		Request.m_fReadData = fg_Move(_Request.m_fReadData);
		Request.m_fWriteData = fg_Move(_Request.m_fWriteData);

		if (!Request.m_pCurl)
			co_return DMibErrorInstance("libcurl was not initialised");

		CURL *pCurl = Request.m_pCurl.f_Get();

		auto fCheckResult = [&](CURLcode _Result) -> TCFuture<void>
			{
				if (_Result != CURLE_OK)
				{
					auto pEasyError = curl_easy_strerror(_Result);
					auto pExtraError = pEasyError ? pEasyError  : "";
					CStr FullError = pExtraError;
					CStr CurlError = Request.m_CurlErrorBuffer.f_GetStr();
					if (CurlError)
						fg_AddStrSep(FullError, CurlError, ". ");
					co_return DMibErrorInstance(fg_Format("libcurl operation on {} failed ({}): {}", _Request.m_URL, _Result, FullError));
				}

				co_return {};
			}
		;

		fg_CurlSetOpt<CURLOPT_ERRORBUFFER>(pCurl, Request.m_CurlErrorBuffer.f_GetStr());

		curl_slist *pHeaders = NULL;
		auto CleanupHeaders = g_OnScopeExit / [&]
			{
				curl_slist_free_all(pHeaders);
			}
		;

		//fCheckResult(fg_CurlSetOpt<CURLOPT_VERBOSE>(pCurl, 1L));
		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_NOSIGNAL>(pCurl, 1L));
		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_PRIVATE>(pCurl, &Request));

		for (auto &Cookie : _Request.m_Cookies.f_Entries())
			Request.m_CookieStr += "{}={}; "_f << Cookie.f_Key() << Cookie.f_Value();

		if (Request.m_CookieStr)
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_COOKIE>(pCurl, Request.m_CookieStr.f_GetStr()));

		if (!_Request.m_Headers.f_FindEqual("Accept"))
			pHeaders = curl_slist_append(pHeaders, "Accept: application/json");

		if (!_Request.m_Headers.f_FindEqual("Content-Type"))
			pHeaders = curl_slist_append(pHeaders, "Content-Type: application/json");

		if (!_Request.m_Headers.f_FindEqual("Expect"))
			pHeaders = curl_slist_append(pHeaders, "Expect:");

		if (!Internal.m_CertificateConfig.m_ClientCertificate.f_IsEmpty())
		{
			struct curl_blob CertBlob;
			CertBlob.data = Internal.m_CertificateConfig.m_ClientCertificate.f_GetArray();
			CertBlob.len = Internal.m_CertificateConfig.m_ClientCertificate.f_GetLen();
			CertBlob.flags = CURL_BLOB_NOCOPY;
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_SSLCERT_BLOB>(pCurl, &CertBlob));
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_SSLCERTTYPE>(pCurl, "PEM"));
		}

		if (!Internal.m_CertificateConfig.m_ClientKey.f_IsEmpty())
		{
			struct curl_blob CertKeyBlob;
			CertKeyBlob.data = Internal.m_CertificateConfig.m_ClientKey.f_GetArray();
			CertKeyBlob.len = Internal.m_CertificateConfig.m_ClientKey.f_GetLen();
			CertKeyBlob.flags = CURL_BLOB_NOCOPY;
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_SSLKEY_BLOB>(pCurl, &CertKeyBlob));
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_SSLCERTTYPE>(pCurl, "PEM"));
		}

		if (!Internal.m_CertificateConfig.m_CertificateAuthorities.f_IsEmpty())
		{
			struct curl_blob CertAuthBlob;
			CertAuthBlob.data = Internal.m_CertificateConfig.m_CertificateAuthorities.f_GetArray();
			CertAuthBlob.len = Internal.m_CertificateConfig.m_CertificateAuthorities.f_GetLen();
			CertAuthBlob.flags = CURL_BLOB_NOCOPY;
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_CAINFO_BLOB>(pCurl, &CertAuthBlob));
		}

		if (_Request.m_bFollowRedirects)
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_FOLLOWLOCATION>(pCurl, 1L));

		if (_Request.m_Method == EMethod_POST || _Request.m_Method == EMethod_PUT || _Request.m_Method == EMethod_PATCH)
		{
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_UPLOAD>(pCurl, 1L));
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_READDATA>(pCurl, &Request));

			if (_Request.m_Method == EMethod_PATCH)
				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_CUSTOMREQUEST>(pCurl, "PATCH"));
			else if (_Request.m_Method == EMethod_POST)
			{
				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_CUSTOMREQUEST>(pCurl, "POST"));
				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_POSTREDIR>(pCurl, CURL_REDIR_POST_ALL));
			}
			else
				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_CUSTOMREQUEST>(pCurl, "PUT"));

			if (Request.m_fReadData)
			{
				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_INFILESIZE_LARGE>(pCurl, Request.m_ReadDataSize));
				auto fReadCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
					{
						CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);

						if (pRequest->m_pReadError)
							return CURL_READFUNC_ABORT;

						size_t Bytes = _Size * _nItems;

						if (Bytes <= 0)
							return Bytes;

						if (pRequest->m_Data.f_IsEmpty())
						{
							pRequest->m_fReadData(Bytes) > [pDeleted = pRequest->m_pDeleted, pRequest](TCAsyncResult<CByteVector> &&_Result)
								{
									if (*pDeleted)
										return;

									if (!_Result)
										pRequest->m_pReadError = _Result.f_GetException();
									else
									{
										pRequest->m_Data = fg_Move(*_Result);
										pRequest->m_iData = fg_Const(pRequest->m_Data).f_GetIterator();
									}
									pRequest->m_PauseMask &= ~CURLPAUSE_SEND;
									curl_easy_pause(pRequest->m_pCurl.f_Get(), pRequest->m_PauseMask);
								}
							;

							pRequest->m_PauseMask |= CURLPAUSE_SEND;
							return CURL_READFUNC_PAUSE;
						}

						NContainer::CByteVector::CIteratorConst &iData = pRequest->m_iData;
						Bytes = fg_Min(iData.f_GetLen(), Bytes);
						NMemory::fg_MemCopy(_pBuffer, &*iData, Bytes);
						iData += Bytes;

						if (iData.f_GetLen() == 0)
						{
							pRequest->m_iData = {};
							pRequest->m_Data.f_Clear();
						}

						return Bytes;
					}
				;

				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_READFUNCTION>(pCurl, (curl_read_callback)fReadCallback));
			}
			else
			{
				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_INFILESIZE_LARGE>(pCurl, Request.m_Data.f_GetLen()));

				auto fReadCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
					{
						CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);
						size_t Bytes = _Size * _nItems;

						if (Bytes > 0)
						{
							NContainer::CByteVector::CIteratorConst &iData = pRequest->m_iData;
							Bytes = fg_Min(iData.f_GetLen(), Bytes);
							if (Bytes)
							{
								NMemory::fg_MemCopy(_pBuffer, &*iData, Bytes);
								iData += Bytes;
							}
						}

						return Bytes;
					}
				;

				co_await fCheckResult(fg_CurlSetOpt<CURLOPT_READFUNCTION>(pCurl, (curl_read_callback)fReadCallback));
			}
		}
		else if (_Request.m_Method == EMethod_DELETE)
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_CUSTOMREQUEST>(pCurl, "DELETE"));
		else if (_Request.m_Method == EMethod_HEAD)
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_NOBODY>(pCurl, 1));

		NHTTP::CURL Url(_Request.m_URL);
		CStr UrlHost = Url.f_GetHost();
		if (UrlHost.f_StartsWith("UNIX:"))
		{
			auto UnixPath = UrlHost.f_RemovePrefix("UNIX:");
			Url.f_SetHost("localhost");
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_UNIX_SOCKET_PATH>(pCurl, UnixPath.f_GetStr()));
			auto NewURL = Url.f_Encode();
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_URL>(pCurl, NewURL.f_GetStr()));
		}
		else
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_URL>(pCurl, _Request.m_URL.f_GetStr()));

		for (auto &Header : _Request.m_Headers.f_Entries())
		{
			CStr HeaderStr(fg_Format("{}: {}", Header.f_Key(), Header.f_Value()));
			pHeaders = curl_slist_append(pHeaders, HeaderStr.f_GetStr());
		}
		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_HTTPHEADER>(pCurl, pHeaders));

		CleanupHeaders.f_Clear();
		if (pHeaders)
			Request.m_pHeaders = fg_Explicit(pHeaders);

		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_HEADERDATA>(pCurl, &Request));
		auto fWriteHeaderCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
			{
				mint nBytes = _Size * _nItems;

				CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);

				if (nBytes >= 5 && fg_StrFind(_pBuffer, "HTTP/", 5) == 0)
					pRequest->m_State.m_Headers.f_Clear(); // Handle redirects

				pRequest->m_State.m_Headers.f_Insert((uint8 const *)_pBuffer, _Size * _nItems);

				return _Size * _nItems;
			}
		;
		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_HEADERFUNCTION>(pCurl, (curl_write_callback)fWriteHeaderCallback));

		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_WRITEDATA>(pCurl, &Request));
		if (Request.m_fWriteData)
		{
			auto fWriteBodyCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
				{
					CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);

					if (pRequest->m_pWriteError)
						return CURL_WRITEFUNC_ERROR;
					else if (pRequest->m_bWriteDone)
					{
						pRequest->m_bWriteDone = false;
						return pRequest->m_WriteDoneBytes;
					}

					size_t Bytes = _Size * _nItems;

					if (Bytes <= 0)
						return Bytes;

					CByteVector Data((uint8 const *)_pBuffer, Bytes);

					pRequest->m_fWriteData(fg_Move(Data)) > [Bytes, pDeleted = pRequest->m_pDeleted, pRequest](TCAsyncResult<void> &&_Result)
						{
							if (*pDeleted)
								return;

							if (!_Result)
								pRequest->m_pWriteError = _Result.f_GetException();
							else
							{
								pRequest->m_bWriteDone = true;
								pRequest->m_WriteDoneBytes = Bytes;
							}

							pRequest->m_PauseMask &= ~CURLPAUSE_RECV;
							curl_easy_pause(pRequest->m_pCurl.f_Get(), pRequest->m_PauseMask);
						}
					;

					pRequest->m_PauseMask |= CURLPAUSE_RECV;
					return CURL_WRITEFUNC_PAUSE;
				}
			;
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_WRITEFUNCTION>(pCurl, (curl_write_callback)fWriteBodyCallback));
		}
		else
		{
			auto fWriteBodyCallback = [](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
				{
					CInternal::CRequest *pRequest = fg_AutoStaticCast(_pData);

					pRequest->m_State.m_Body.f_Insert((uint8 const *)_pBuffer, _Size * _nItems);

					return _Size * _nItems;
				}
			;
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_WRITEFUNCTION>(pCurl, (curl_write_callback)fWriteBodyCallback));
		}

		auto *pActorHolder = fp_GetActorHolder();

		auto &HolderInternal = *pActorHolder->m_pInternal;
		curl_multi_add_handle(HolderInternal.m_pMulti.f_Get(), pCurl);
		Request.m_bAddedHandle = true;

		co_return co_await Request.m_FinishedPromise.f_Future();
	}
}
