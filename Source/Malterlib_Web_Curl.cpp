// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_Curl.h"

#include <curl/curl.h>

namespace NMib::NWeb
{
	using namespace NContainer;
	using namespace NStr;
	
	struct CCurlActor::CState
	{
		TCVector<uint8> m_Headers;
		TCVector<uint8> m_Body;
	};

	CCurlActor::CResult::CResult(CState const &_State)
		: m_Body(CStr((ch8 const *)_State.m_Body.f_GetArray(), _State.m_Body.f_GetLen()))
	{
		CStr HeaderStr((ch8 const *)_State.m_Headers.f_GetArray(), _State.m_Headers.f_GetLen());
		
		CStr Status = fg_GetStrLineSep(HeaderStr);
		aint nParsed;
		(void)
			(
				NMib::NStr::CStrPtr::CParse("HTTP/1.1 {} {}")
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
		
		NAggregate::TCAggregate<CCurlInit> g_CurlInit = {DAggregateInit};
	}
	
	NConcurrency::TCContinuation<CCurlActor::CResult> CCurlActor::f_Request
		(
			EMethod _Method
			, NStr::CStr const &_URL
			, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Headers
			, NStr::CStr const &_Data
		)
	{
		return NConcurrency::TCContinuation<CCurlActor::CResult>::fs_RunProtected() > [&]
			{

				CStr Data(_Data);

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
							DMibError(fg_Format("libcurl operation on {} failed: {}", _URL, CurlErrorBuffer));
					}
				;

				curl_easy_setopt(pCurl, CURLOPT_ERRORBUFFER, CurlErrorBuffer.f_GetStr());


				curl_slist *pHeaders = NULL;
				auto CleanupHeaders = g_OnScopeExit > [&]
					{
						curl_slist_free_all(pHeaders);
					}
				;

				pHeaders = curl_slist_append(pHeaders, "Accept: application/json");
				pHeaders = curl_slist_append(pHeaders, "Content-Type: application/json");
				pHeaders = curl_slist_append(pHeaders, "Expect:");

				if (_Method == EMethod_POST)
				{
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_POST, 1L));
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_POSTFIELDS, Data.f_GetStr()));
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_POSTFIELDSIZE, Data.f_GetLen()));
				}
				else if (_Method == EMethod_PUT)
				{
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_UPLOAD, 1L));
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_READDATA, &Data));
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_INFILESIZE, Data.f_GetLen()));

					auto fReadCallback =
						[](char *_pBuffer, size_t _Size, size_t _nItems, void *_pData) -> size_t
						{
							size_t Bytes = _Size * _nItems;

							if (Bytes > 0)
							{
								CStr &DataSource = *static_cast<CStr*>(_pData);
								NMem::fg_MemCopy(_pBuffer, DataSource.f_GetStr(), Bytes);
								DataSource = DataSource.f_Extract(Bytes);
							}

							return Bytes;
						}
					;

					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_READFUNCTION, (curl_read_callback)fReadCallback));
				}
				else if (_Method == EMethod_DELETE)
				{
					fCheckResult(curl_easy_setopt(pCurl, CURLOPT_CUSTOMREQUEST, "DELETE"));
				}

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
}
