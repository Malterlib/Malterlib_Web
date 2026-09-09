// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Web_HttpClient.h"

#include <Mib/Cryptography/Certificate>
#include <Mib/Cryptography/RandomID>
#include <Mib/Web/HTTP/URL>
#include <Mib/Network/ResolveActor>
#include <Mib/Concurrency/WeakActor>
#include <Mib/Core/IoLoop>

#define CURL_STRICTER

#include <curl/curl.h>
#include <curl/external_resolver.h>
#include <openssl/x509.h>

#ifdef DPlatformFamily_Windows
#	include <winsock2.h>
#else
#	include <unistd.h>
#endif

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

		struct CCertificatePreparation
		{
			NThread::CMutual m_Lock;
			TCAsyncResult<void> m_Result;
			TCVector<TCPromise<void>> m_Waiters;
			bool m_bStarted = false;
			bool m_bReady = false;
		};

		constinit NStorage::TCAggregate<CCertificatePreparation, 128> g_CertificatePreparation = {DAggregateInit};
	}

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

			int64 m_ReadDataSize = -1;
			uint64 m_WriteDoneBytes = 0;
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
			TCActorFunctor<TCFuture<CByteVector> (umint _nBytes)> m_fReadData;
			TCActorFunctor<TCFuture<void> (CByteVector _Data)> m_fWriteData;
			NException::CExceptionPointer m_pWriteError;
			NException::CExceptionPointer m_pReadError;
			int m_PauseMask = 0;
			bool m_bAddedHandle = false;
			bool m_bReadEOF = false;
		};

		struct CSocket
		{
			uint64 m_Generation = 0;
			TCWeakActor<CHttpClientActor> m_Actor;
			NSys::CIoLoopRegistration *m_pRegistration = nullptr;
			curl_socket_t m_Handle = CURL_SOCKET_BAD;
			int m_Interest = CURL_POLL_NONE;
		};

		struct CDrain
		{
			// The initial reference is released after curl has closed its connection cache.
			NAtomic::TCAtomic<umint> m_Pending{1};
			TCPromise<void> m_Done;

			void f_Release()
			{
				if (m_Pending.f_FetchSub(1) == 1)
					m_Done.f_SetResult();
			}
		};

		struct CDNS
		{
			CIntrusiveRefCount m_RefCount;
			CURL *m_pEasy = nullptr;
			CActorSubscription m_Cancel;
			TCVector<curl_external_address> m_Addresses;
			int m_Status = 0;
			bool m_bCancelled = false;
		};

		static void fs_CloseSocket(curl_socket_t _Socket)
		{
#ifdef DPlatformFamily_Windows
			closesocket(_Socket);
#else
			close(_Socket);
#endif
		}

		static NSys::EIoLoopEvent fs_Interest(int _Interest)
		{
			auto Events = NSys::EIoLoopEvent::mc_None;
			if (_Interest == CURL_POLL_IN || _Interest == CURL_POLL_INOUT)
				Events |= NSys::EIoLoopEvent::mc_Read;
			if (_Interest == CURL_POLL_OUT || _Interest == CURL_POLL_INOUT)
				Events |= NSys::EIoLoopEvent::mc_Write;

			return Events;
		}

		static int fs_Socket(CURL *, curl_socket_t _Socket, int _What, void *_pUser, void *) noexcept
		{
			try
			{
				auto &Internal = *static_cast<CInternal *>(_pUser);
				auto *pFound = Internal.m_Sockets.f_FindEqual(_Socket);
				if (!pFound && _What == CURL_POLL_REMOVE)
					return 0;

				if (!pFound)
				{
					TCUniquePointer<CSocket> pSocket = fg_Construct();
					pSocket->m_Handle = _Socket;
					pSocket->m_Actor = fg_ThisActor(Internal.m_pActor).f_Weak();
					pSocket->m_Generation = ++Internal.m_NextSocketGeneration;

					pSocket->m_pRegistration = Internal.m_pLoop->f_Register
						(
							NSys::CIoLoopHandle(_Socket)
							, pSocket.f_Get()
							, NSys::EIoLoopEvent::mc_Read | NSys::EIoLoopEvent::mc_Write
							, [](void *_pToken, NSys::EIoLoopEvent _Events, int _Error)
							{
								auto &Socket = *static_cast<CSocket *>(_pToken);
								if (auto Actor = Socket.m_Actor.f_Lock())
									Actor.f_Bind<&CHttpClientActor::fp_SocketReady>(smint(Socket.m_Handle), Socket.m_Generation, _Events, _Error).f_DiscardResult();
							}
							, false
							, {.m_bReadinessOnly = true, .m_bLevelReadiness = true}
						)
					;

					Internal.m_Sockets[_Socket] = fg_Move(pSocket);
					pFound = Internal.m_Sockets.f_FindEqual(_Socket);
				}

				(*pFound)->m_Interest = _What == CURL_POLL_REMOVE ? CURL_POLL_NONE : _What;

				// Idle cached connections retain their registration until curl closes them.
				// Each direction can deliver at most one unused notification while idle.
				Internal.m_pLoop->f_RequestReadiness((*pFound)->m_pRegistration, fs_Interest((*pFound)->m_Interest));

				return 0;
			}
			catch (...)
			{
				return -1;
			}
		}

		static int fs_Close(void *_pUser, curl_socket_t _Socket)
		{
			auto &Internal = *static_cast<CInternal *>(_pUser);
			auto *pFound = Internal.m_Sockets.f_FindEqual(_Socket);
			if (!pFound)
			{
				fs_CloseSocket(_Socket);

				return 0;
			}

			auto pSocket = fg_Move(*pFound);
			Internal.m_Sockets.f_Remove(_Socket);
			auto *pRegistration = pSocket->m_pRegistration;
			Internal.m_pDrain->m_Pending.f_FetchAdd(1);
			Internal.m_pLoop->f_DeregisterAsync
				(
					pRegistration
					, [pSocket = fg_Move(pSocket), pDrain = Internal.m_pDrain]
					{
						fs_CloseSocket(pSocket->m_Handle);
						pDrain->f_Release();
					}
				)
			;

			return 0;
		}

		static int fs_Timer(CURLM *, long _Milliseconds, void *_pUser) noexcept
		{
			try
			{
				auto &Internal = *static_cast<CInternal *>(_pUser);
				auto Generation = ++Internal.m_TimerGeneration;
				fg_ThisActor(Internal.m_pActor).f_Bind<&CHttpClientActor::fp_SetTimer>(Generation, _Milliseconds).f_DiscardResult();

				return 0;
			}
			catch (...)
			{
				return -1;
			}
		}

		static void *fs_Resolve(void *_pUser, CURL *_pEasy, char const *_pHost, int _IPVersion) noexcept
		{
			try
			{
				auto &Internal = *static_cast<CInternal *>(_pUser);
				TCSharedPointer<CDNS> pDNS = fg_Construct();
				pDNS->m_pEasy = _pEasy;
				auto PreferType = _IPVersion == CURL_IPRESOLVE_V4
					? NNetwork::ENetAddressType_TCPv4
					: _IPVersion == CURL_IPRESOLVE_V6
						? NNetwork::ENetAddressType_TCPv6
						: NNetwork::ENetAddressType_None
				;

				Internal.m_Resolver.f_Bind<&NNetwork::CResolveActor::f_ResolveHost>(CStr(_pHost), PreferType).f_Call()
					> [pDNS](TCAsyncResult<NNetwork::CResolveActor::CLookup> &&_Lookup)
					{
						if (pDNS->m_bCancelled)
						{
							if (_Lookup)
								fg_Move(_Lookup->m_Result).f_DiscardResult();

							return;
						}

						if (!_Lookup)
						{
							pDNS->m_Status = -1;
							curl_external_resolver_ready(pDNS->m_pEasy);

							return;
						}

						pDNS->m_Cancel = fg_Move(_Lookup->m_Cancel);
						fg_Move(_Lookup->m_Result) > [pDNS](TCAsyncResult<NNetwork::CResolveActor::CAddresses> &&_Result)
							{
								if (pDNS->m_bCancelled)
									return;

								pDNS->m_Status = _Result ? 1 : -1;
								if (_Result)
									for (auto const &Address : *_Result)
									{
										curl_external_address Item{};
										NNetwork::CNetAddressTCPv4 IPv4;
										NNetwork::CNetAddressTCPv6 IPv6;
										if (Address.f_Get(IPv4))
											NMemory::fg_MemCopy(Item.ip, IPv4.m_IP, sizeof(IPv4.m_IP));
										else if (Address.f_Get(IPv6))
										{
											NMemory::fg_MemCopy(Item.ip, IPv6.m_IP, sizeof(IPv6.m_IP));
											Item.ipv6 = 1;
										}
										else
											continue;

										Item.scope_id = Address.f_GetScopeID();
										pDNS->m_Addresses.f_Insert(Item);
									}

								curl_external_resolver_ready(pDNS->m_pEasy);
							}
						;
					}
				;

				return pDNS.f_Detach();
			}
			catch (...)
			{
				return nullptr;
			}
		}

		static int fs_ResolvePoll(void *_pLookup, curl_external_address const **o_pAddresses, size_t *o_nAddresses)
		{
			auto &DNS = *static_cast<CDNS *>(_pLookup);
			*o_pAddresses = DNS.m_Addresses.f_GetArray();
			*o_nAddresses = DNS.m_Addresses.f_GetLen();

			return DNS.m_Status;
		}

		static void fs_ResolveCancel(void *_pLookup)
		{
			TCSharedPointer<CDNS> pDNS = fg_Attach(static_cast<CDNS *>(_pLookup));
			pDNS->m_bCancelled = true;
			pDNS->m_Cancel.f_Clear();
		}

		uint64 m_NextSocketGeneration = 0;
		uint64 m_TimerGeneration = 0;
		CHttpClientActor *m_pActor = nullptr;
		TCUniquePointer<CURLM, CCurlDeleterMulti> m_pMulti;
		NSys::ICIoLoop *m_pLoop = nullptr;
		TCActor<NNetwork::CResolveActor> m_Resolver;
		TCMap<curl_socket_t, TCUniquePointer<CSocket>> m_Sockets;
		TCSharedPointer<CDrain> m_pDrain = fg_Construct();
		CActorSubscription m_Timer;
		NException::CExceptionPointer m_pFailure;

		CCertificateConfig m_CertificateConfig;

		TCMap<CStr, CRequest> m_Requests;
		bool m_bStopping = false;
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
				NMib::NStr::CStr::CParse("HTTP/{} {} {}")
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

	auto CHttpClientActor::CRequest::f_AsyncSend() -> CAsyncReadData &
	{
		return m_SendData.f_SetAsType<CAsyncReadData>();
	}

	auto CHttpClientActor::CRequest::f_AsyncReceive() -> CAsyncWriteData &
	{
		return m_ReceiveData.f_SetAsType<CAsyncWriteData>();
	}

	CHttpClientActor::CInternal::CRequest::~CRequest()
	{
		*m_pDeleted = true;

		if (m_bAddedHandle)
		{
			curl_multi_remove_handle(m_pActor->mp_pInternal->m_pMulti.f_Get(), m_pCurl.f_Get());
		}
	}

	CHttpClientActor::CHttpClientActor(CCertificateConfig const &_CertificateConfig)
		: mp_pInternal(fg_Construct(_CertificateConfig))
	{
	}

	CHttpClientActor::~CHttpClientActor() = default;

	void CHttpClientActor::fp_Construct()
	{
		*g_CurlInit;

		auto &Internal = *mp_pInternal;
		Internal.m_pActor = this;

		auto Binding = f_ConcurrencyManager().f_PickIoLoopBinding(self.m_pThis->f_GetPriority());
		Internal.m_pLoop = Binding ? Binding.m_pLoop : NSys::fg_GetSharedIoLoop();
		if (!Internal.m_pLoop)
			DMibError("No I/O loop available for HTTP client");
		if (Binding)
			self.m_pThis->f_SetInitialQueue(Binding.m_iQueue);

		Internal.m_Resolver = fg_ConstructActor<NNetwork::CResolveActor>();
		Internal.m_pMulti = fg_Explicit(curl_multi_init());
		if (!Internal.m_pMulti)
			DMibError("Failed to initialize curl multi handle");

		auto *pMulti = Internal.m_pMulti.f_Get();
		curl_multi_setopt(pMulti, CURLMOPT_SOCKETFUNCTION, &CInternal::fs_Socket);
		curl_multi_setopt(pMulti, CURLMOPT_SOCKETDATA, &Internal);
		curl_multi_setopt(pMulti, CURLMOPT_TIMERFUNCTION, &CInternal::fs_Timer);
		curl_multi_setopt(pMulti, CURLMOPT_TIMERDATA, &Internal);

		curl_external_resolver Resolver{&Internal, &CInternal::fs_Resolve, &CInternal::fs_ResolvePoll, &CInternal::fs_ResolveCancel};
		auto ResolverResult = curl_multi_set_external_resolver(pMulti, &Resolver);
		if (ResolverResult != CURLM_OK)
			DMibError(curl_multi_strerror(ResolverResult));
	}

	TCFuture<void> CHttpClientActor::fp_PrepareCertificates()
	{
		auto &Preparation = *g_CertificatePreparation;
		TCPromise<void> Promise;
		bool bStart;

		{
			DMibLock(Preparation.m_Lock);
			if (Preparation.m_bReady)
				co_return Preparation.m_Result;

			Preparation.m_Waiters.f_Insert(Promise);
			bStart = !Preparation.m_bStarted;
			Preparation.m_bStarted = true;
		}

		if (bStart)
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					g_Dispatch(BlockingActorCheckout) / []
					{
						TCAsyncResult<void> Result;

						try
						{
							auto *pStore = X509_STORE_new();
							if (!pStore)
								DMibError("Could not allocate certificate store");

							auto Cleanup = g_OnScopeExit / [pStore]
								{
									X509_STORE_free(pStore);
								}
							;

							NCryptography::CCertificate::fs_GetSystemCertificates(pStore);
							Result.f_SetResult();
						}
						catch (...)
						{
							Result.f_SetException(NException::fg_CurrentException());
						}

						auto &Preparation = *g_CertificatePreparation;
						TCVector<TCPromise<void>> Waiters;

						{
							DMibLock(Preparation.m_Lock);
							Preparation.m_Result = fg_Move(Result);
							Preparation.m_bReady = true;
							Waiters = fg_Move(Preparation.m_Waiters);
						}

						for (auto &Waiter : Waiters)
							Waiter.f_SetResult(Preparation.m_Result);
					}
				)
			;
		}

		co_return co_await Promise.f_Future();
	}

	TCFuture<void> CHttpClientActor::fp_SetTimer(uint64 _Generation, long _Milliseconds)
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bStopping || Internal.m_pFailure || _Generation != Internal.m_TimerGeneration)
			co_return {};

		Internal.m_Timer.f_Clear();

		if (_Milliseconds < 0)
			co_return {};

		if (_Milliseconds == 0)
		{
			fp_Drive(smint(CURL_SOCKET_TIMEOUT), 0);

			co_return {};
		}

		auto Timer = co_await fg_OneshotTimerAbortable
			(
				fp64(_Milliseconds) / 1000.0
				, [this, _Generation]() -> TCFuture<void>
				{
					if (!mp_pInternal->m_bStopping && _Generation == mp_pInternal->m_TimerGeneration)
						fp_Drive(smint(CURL_SOCKET_TIMEOUT), 0);

					co_return {};
				}
			)
		;

		if (!Internal.m_bStopping && !Internal.m_pFailure && _Generation == Internal.m_TimerGeneration)
			Internal.m_Timer = fg_Move(Timer);

		co_return {};
	}

	void CHttpClientActor::fp_SocketReady(smint _Socket, uint64 _Generation, NSys::EIoLoopEvent _Events, int _Error)
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bStopping || Internal.m_pFailure)
			return;

		auto *pSocket = Internal.m_Sockets.f_FindEqual(curl_socket_t(_Socket));
		if (!pSocket || (*pSocket)->m_Generation != _Generation)
			return;

		if (_Error)
		{
			fp_Fail(DMibErrorInstance(fg_Format("HTTP I/O registration failed ({})", _Error)));

			return;
		}

		if ((*pSocket)->m_Interest == CURL_POLL_NONE)
			return;

		int Events = 0;
		if (fg_IsSet(_Events, NSys::EIoLoopEvent::mc_Read | NSys::EIoLoopEvent::mc_ReadClosed | NSys::EIoLoopEvent::mc_Hup))
			Events |= CURL_CSELECT_IN;
		if (fg_IsSet(_Events, NSys::EIoLoopEvent::mc_Write | NSys::EIoLoopEvent::mc_WriteClosed))
			Events |= CURL_CSELECT_OUT;
		if (_Error || fg_IsSet(_Events, NSys::EIoLoopEvent::mc_Error))
			Events |= CURL_CSELECT_ERR;

		fp_Drive(_Socket, Events);
	}

	void CHttpClientActor::fp_Drive(smint _Socket, int _Events)
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bStopping || Internal.m_pFailure)
			return;

		int Running;
		auto Error = curl_multi_socket_action(Internal.m_pMulti.f_Get(), curl_socket_t(_Socket), _Events, &Running);
		if (Error != CURLM_OK)
		{
			fp_Fail(DMibErrorInstance(curl_multi_strerror(Error)));

			return;
		}

		int Remaining;
		while (auto *pMessage = curl_multi_info_read(Internal.m_pMulti.f_Get(), &Remaining))
		{
			if (pMessage->msg != CURLMSG_DONE)
				continue;

			CInternal::CRequest *pRequest = nullptr;
			curl_easy_getinfo(pMessage->easy_handle, CURLINFO_PRIVATE, &pRequest);

			NException::CExceptionPointer pError;
			if (pRequest->m_pReadError && pRequest->m_pWriteError)
			{
				NException::CExceptionExceptionVectorData::CErrorCollector Errors;
				Errors.f_AddError(fg_Move(pRequest->m_pReadError));
				Errors.f_AddError(fg_Move(pRequest->m_pWriteError));

				pError = fg_Move(Errors).f_GetException();
			}
			else if (pRequest->m_pReadError)
				pError = fg_Move(pRequest->m_pReadError);
			else if (pRequest->m_pWriteError)
				pError = fg_Move(pRequest->m_pWriteError);

			fg_ThisActor(this).f_Bind<&CHttpClientActor::fp_RequestFinished>(pRequest->f_GetID(), pMessage->data.result, fg_Move(pError)).f_DiscardResult();
		}

		if (auto *pSocket = Internal.m_Sockets.f_FindEqual(curl_socket_t(_Socket)))
			Internal.m_pLoop->f_RequestReadiness((*pSocket)->m_pRegistration, CInternal::fs_Interest((*pSocket)->m_Interest));
	}

	void CHttpClientActor::fp_Fail(NException::CExceptionPointer &&_pError)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_pFailure = fg_Move(_pError);
		++Internal.m_TimerGeneration;
		Internal.m_Timer.f_Clear();

		for (auto &Request : Internal.m_Requests)
			if (!Request.m_FinishedPromise.f_IsSet())
				Request.m_FinishedPromise.f_SetException(Internal.m_pFailure);

		Internal.m_Requests.f_Clear();
		Internal.m_pMulti.f_Clear();
	}

	TCFuture<void> CHttpClientActor::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		Internal.m_bStopping = true;
		++Internal.m_TimerGeneration;
		Internal.m_Timer.f_Clear();

		for (auto &Request : Internal.m_Requests)
		{
			if (Request.m_FinishedPromise.f_IsSet())
				continue;

			Request.m_FinishedPromise.f_SetException(DMibErrorInstance("Aborted request"));
		}

		Internal.m_Requests.f_Clear();
		Internal.m_pMulti.f_Clear();

		DMibFastCheck(Internal.m_Sockets.f_IsEmpty());

		Internal.m_pDrain->f_Release();

		co_await Internal.m_pDrain->m_Done.f_Future();
		co_await fg_Move(Internal.m_Resolver).f_Destroy();

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

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_Get
		(
			NStr::CStr _URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
		)
	{
		return f_SendRequest(CRequest{.m_URL = fg_Move(_URL), .m_Headers = fg_Move(_Headers), .m_Method = EMethod_GET});
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_Head
		(
			NStr::CStr _URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
		)
	{
		return f_SendRequest(CRequest{.m_URL = fg_Move(_URL), .m_Headers = fg_Move(_Headers), .m_Method = EMethod_HEAD});
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_Post
		(
			NStr::CStr _URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
			, CRequestData _Data
		)
	{
		CRequest Request;
		Request.m_URL = fg_Move(_URL);
		Request.m_Headers = fg_Move(_Headers);
		Request.m_Method = EMethod_POST;
		_Data.f_Visit
			(
				[&]<typename tf_CType>(tf_CType &&_Value)
				{
					using CType = NTraits::TCRemoveReferenceAndQualifiers<tf_CType>;

					if constexpr (NTraits::cIsSame<CType, NContainer::CByteVector>)
						Request.m_SendData = fg_Move(_Value);
					else if constexpr (NTraits::cIsSame<CType, NStr::CStr>)
						Request.m_SendData = NContainer::CByteVector::fs_FromString(_Value);
					else
					{
						if (!Request.m_Headers.f_FindEqual("Content-Type"))
							Request.m_Headers["Content-Type"] = "application/json";
						Request.m_SendData = NContainer::CByteVector::fs_FromString(_Value.f_ToString(nullptr));
					}
				}
			)
		;
		return f_SendRequest(fg_Move(Request));
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_Patch
		(
			NStr::CStr _URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
			, CRequestData _Data
		)
	{
		CRequest Request;
		Request.m_URL = fg_Move(_URL);
		Request.m_Headers = fg_Move(_Headers);
		Request.m_Method = EMethod_PATCH;
		_Data.f_Visit
			(
				[&]<typename tf_CType>(tf_CType &&_Value)
				{
					using CType = NTraits::TCRemoveReferenceAndQualifiers<tf_CType>;

					if constexpr (NTraits::cIsSame<CType, NContainer::CByteVector>)
						Request.m_SendData = fg_Move(_Value);
					else if constexpr (NTraits::cIsSame<CType, NStr::CStr>)
						Request.m_SendData = NContainer::CByteVector::fs_FromString(_Value);
					else
					{
						if (!Request.m_Headers.f_FindEqual("Content-Type"))
							Request.m_Headers["Content-Type"] = "application/json";
						Request.m_SendData = NContainer::CByteVector::fs_FromString(_Value.f_ToString(nullptr));
					}
				}
			)
		;
		return f_SendRequest(fg_Move(Request));
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_Put
		(
			NStr::CStr _URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
			, CRequestData _Data
		)
	{
		CRequest Request;
		Request.m_URL = fg_Move(_URL);
		Request.m_Headers = fg_Move(_Headers);
		Request.m_Method = EMethod_PUT;
		_Data.f_Visit
			(
				[&]<typename tf_CType>(tf_CType &&_Value)
				{
					using CType = NTraits::TCRemoveReferenceAndQualifiers<tf_CType>;

					if constexpr (NTraits::cIsSame<CType, NContainer::CByteVector>)
						Request.m_SendData = fg_Move(_Value);
					else if constexpr (NTraits::cIsSame<CType, NStr::CStr>)
						Request.m_SendData = NContainer::CByteVector::fs_FromString(_Value);
					else
					{
						if (!Request.m_Headers.f_FindEqual("Content-Type"))
							Request.m_Headers["Content-Type"] = "application/json";
						Request.m_SendData = NContainer::CByteVector::fs_FromString(_Value.f_ToString(nullptr));
					}
				}
			)
		;
		return f_SendRequest(fg_Move(Request));
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_Delete
		(
			NStr::CStr _URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> _Headers
		)
	{
		return f_SendRequest(CRequest{.m_URL = fg_Move(_URL), .m_Headers = fg_Move(_Headers), .m_Method = EMethod_DELETE});
	}

	TCFuture<CHttpClientActor::CResult> CHttpClientActor::f_SendRequest(CRequest _Request)
	{
		if (f_IsDestroyed())
			co_return DMibErrorInstance("HTTP client actor shutting down");

		auto CaptureScope = co_await g_CaptureExceptions;

		co_await fp_PrepareCertificates();
		if (f_IsDestroyed())
			co_return DMibErrorInstance("Aborted request");

		auto &Internal = *mp_pInternal;
		if (Internal.m_pFailure)
			co_return Internal.m_pFailure;

		auto RequestID = NCryptography::fg_FastRandomID(Internal.m_Requests);
		auto &Request = Internal.m_Requests[RequestID];
		Request.m_pActor = this;
		Request.m_pCurl = fg_Explicit(curl_easy_init());
		_Request.m_SendData.f_Visit
			(
				[&]<typename tf_CData>(tf_CData &&_Data)
				{
					using CType = NTraits::TCRemoveReferenceAndQualifiers<tf_CData>;

					if constexpr (NTraits::cIsSame<CType, NContainer::CByteVector>)
						Request.m_Data = fg_Move(_Data);
					else if constexpr (NTraits::cIsSame<CType, CAsyncReadData>)
					{
						Request.m_ReadDataSize = fg_Move(_Data.m_Size);
						Request.m_fReadData = fg_Move(_Data.m_fRead);
					}
					else if constexpr (NTraits::cIsSame<CType, CVoidTag>)
						;
					else
						static_assert(false, "Internal Error");
				}
			)
		;
		Request.m_iData = fg_Const(Request.m_Data).f_GetIterator();
		Request.m_CurlErrorBuffer.f_CreateWritableBuffer(CURL_ERROR_SIZE, true);
		if (_Request.m_ReceiveData.f_IsOfType<CAsyncWriteData>())
			Request.m_fWriteData = fg_Move(_Request.m_ReceiveData.f_GetAsType<CAsyncWriteData>().m_fWrite);

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
		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_CLOSESOCKETFUNCTION>(pCurl, &CInternal::fs_Close));
		co_await fCheckResult(fg_CurlSetOpt<CURLOPT_CLOSESOCKETDATA>(pCurl, &Internal));

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

		if (auto *pCookieHeader = _Request.m_Headers.f_FindEqual("Cookie"))
		{
			if (Request.m_CookieStr)
				Request.m_CookieStr += *pCookieHeader;
			else
				Request.m_CookieStr = *pCookieHeader;
		}

		if (Request.m_CookieStr)
			co_await fCheckResult(fg_CurlSetOpt<CURLOPT_COOKIE>(pCurl, Request.m_CookieStr.f_GetStr()));

		if (_Request.m_bSetDefaultHeaders && !_Request.m_Headers.f_FindEqual("Accept"))
			pHeaders = curl_slist_append(pHeaders, "Accept: application/json");

		if (_Request.m_bSetDefaultHeaders && !_Request.m_Headers.f_FindEqual("Content-Type"))
			pHeaders = curl_slist_append(pHeaders, "Content-Type: application/json");

		if (_Request.m_bSetDefaultHeaders && !_Request.m_Headers.f_FindEqual("Expect"))
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
						if (pRequest->m_bReadEOF)
							return 0;

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
										pRequest->m_bReadEOF = pRequest->m_Data.f_IsEmpty();
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
			if (Header.f_Key().f_CmpNoCase(gc_Str<"Cookie">.m_Str) == 0)
				continue;

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
				umint nBytes = _Size * _nItems;

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

					size_t Bytes = _Size * _nItems;
					if (!Bytes)
						return 0;

					// Unpausing replays buffered data, possibly coalesced with more input or
					// split differently. Acknowledge only complete callback blocks and deliver
					// each byte to the async sink once, retaining its accepted prefix on pause.
					if (pRequest->m_WriteDoneBytes >= Bytes)
					{
						pRequest->m_WriteDoneBytes -= Bytes;
						return Bytes;
					}

					size_t NewBytes = Bytes - size_t(pRequest->m_WriteDoneBytes);
					CByteVector Data(reinterpret_cast<uint8 const *>(_pBuffer) + pRequest->m_WriteDoneBytes, NewBytes);

					pRequest->m_fWriteData(fg_Move(Data)) > [NewBytes, pDeleted = pRequest->m_pDeleted, pRequest](TCAsyncResult<void> &&_Result)
						{
							if (*pDeleted)
								return;

							if (!_Result)
								pRequest->m_pWriteError = _Result.f_GetException();
							else
								pRequest->m_WriteDoneBytes += NewBytes;

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

		auto AddResult = curl_multi_add_handle(Internal.m_pMulti.f_Get(), pCurl);
		if (AddResult != CURLM_OK)
			co_return DMibErrorInstance(curl_multi_strerror(AddResult));

		Request.m_bAddedHandle = true;

		co_return co_await Request.m_FinishedPromise.f_Future();
	}
}
