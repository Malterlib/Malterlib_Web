// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_Curl.h"
#include <Mib/Cryptography/Certificate>
#include <Mib/Web/HTTP/URL>

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
	using namespace NStr;
	
	struct CCurlActor::CState
	{
		CByteVector m_Headers;
		CByteVector m_Body;
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

	namespace
	{
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
		
		constinit NStorage::TCAggregate<CCurlInit> g_CurlInit = {DAggregateInit};
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
		return NConcurrency::TCFuture<CCurlActor::CResult>::fs_RunProtected() / [&]
			{
				NContainer::CByteVector::CIteratorConst DataIterator = _Data.f_GetIterator();

				*g_CurlInit;

				::CURL *pCurl = curl_easy_init();

				if (!pCurl)
					DMibError("libcurl was not initialised");

				auto CleanupCurl = g_OnScopeExit > [&]
					{
						curl_easy_cleanup(pCurl);
					}
				;

				CStr CurlErrorBuffer;
				CurlErrorBuffer.f_CreateWritableBuffer(CURL_ERROR_SIZE, true);

				auto fCheckResult =
					[&](CURLcode _Result) -> void
					{
						if (_Result != CURLE_OK)
							DMibError(fg_Format("libcurl operation on {} failed ({}): {}", _URL, _Result, CurlErrorBuffer.f_GetStr()));
					}
				;

				curl_easy_setopt(pCurl, CURLOPT_ERRORBUFFER, CurlErrorBuffer.f_GetStr());


				curl_slist *pHeaders = NULL;
				auto CleanupHeaders = g_OnScopeExit > [&]
					{
						curl_slist_free_all(pHeaders);
					}
				;

				//fCheckResult(curl_easy_setopt(pCurl, CURLOPT_VERBOSE, 1L));
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L));

				CStr CookieStr;
				for (auto &Cookie : _Cookies)
					CookieStr += "{}={}; "_f << _Cookies.fs_GetKey(Cookie) << Cookie;

				if (CookieStr)
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_COOKIE, CookieStr.f_GetStr()));

				if (!_Headers.f_FindEqual("Accept"))
					pHeaders = curl_slist_append(pHeaders, "Accept: application/json");

				if (!_Headers.f_FindEqual("Content-Type"))
					pHeaders = curl_slist_append(pHeaders, "Content-Type: application/json");

				if (!_Headers.f_FindEqual("Expect"))
					pHeaders = curl_slist_append(pHeaders, "Expect:");

				if (!mp_CertificateConfig.m_ClientCertificate.f_IsEmpty())
				{
					struct curl_blob CertBlob;
					CertBlob.data = mp_CertificateConfig.m_ClientCertificate.f_GetArray();
					CertBlob.len = mp_CertificateConfig.m_ClientCertificate.f_GetLen();
					CertBlob.flags = CURL_BLOB_COPY;
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLCERT_BLOB, &CertBlob));
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLCERTTYPE, "PEM"));
				}

				if (!mp_CertificateConfig.m_ClientKey.f_IsEmpty())
				{
					struct curl_blob CertKeyBlob;
					CertKeyBlob.data = mp_CertificateConfig.m_ClientKey.f_GetArray();
					CertKeyBlob.len = mp_CertificateConfig.m_ClientKey.f_GetLen();
					CertKeyBlob.flags = CURL_BLOB_COPY;
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLKEY_BLOB, &CertKeyBlob));
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_SSLCERTTYPE, "PEM"));
				}

				if (!mp_CertificateConfig.m_CertificateAuthorities.f_IsEmpty())
				{
					struct curl_blob CertAuthBlob;
					CertAuthBlob.data = mp_CertificateConfig.m_CertificateAuthorities.f_GetArray();
					CertAuthBlob.len = mp_CertificateConfig.m_CertificateAuthorities.f_GetLen();
					CertAuthBlob.flags = CURL_BLOB_COPY;
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
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_READDATA, &DataIterator));
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_INFILESIZE, _Data.f_GetLen()));

					auto fReadCallback =
						[](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
						{
							size_t Bytes = _Size * _nItems;

							if (Bytes > 0)
							{
								NContainer::CByteVector::CIteratorConst &DataSource = *static_cast<NContainer::CByteVector::CIteratorConst *>(_pData);
								Bytes = fg_Min(DataSource.f_GetLen(), Bytes);
								NMemory::fg_MemCopy(_pBuffer, &*DataSource, Bytes);
								DataSource += Bytes;
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

				CState State;

				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_HEADERDATA, &State));
				auto fWriteHeaderCallback =
					[](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
					{
						CState &State = *static_cast<CState *>(_pData);
						State.m_Headers.f_Insert((uint8 const *)_pBuffer, _Size * _nItems);
						return _Size * _nItems;
					}
				;
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_HEADERFUNCTION, (curl_write_callback)fWriteHeaderCallback));

				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, &State));
				auto fWriteBodyCallback =
					[](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
					{
						CState &State = *static_cast<CState *>(_pData);
						State.m_Body.f_Insert((uint8 const *)_pBuffer, _Size * _nItems);
						return _Size * _nItems;
					}
				;
				fCheckResult(curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, (curl_write_callback)fWriteBodyCallback));

				fCheckResult(curl_easy_perform(pCurl));

				CCurlActor::CResult Result(State);
				return Result;
			}
		;
	}

	CCurlActor::CCurlActor(CCertificateConfig const &_CertificateConfig)
		: mp_CertificateConfig(_CertificateConfig)
	{
	}

}
