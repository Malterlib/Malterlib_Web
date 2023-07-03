// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTPServer.h"

#include <Mib/Web/FastCGIServer>
#include <Mib/Web/NGINXLauncher>
#include <Mib/File/ExeFS>

#ifndef DPlatform_Windows
#include <netdb.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <memory.h>
#else
#define  _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#endif

namespace NMib::NWeb
{
	struct CHTTPServer::CHTTPServerInternal : public NConcurrency::CActor
	{
		using CActorHolder = NConcurrency::CSeparateThreadActorHolder;

	private:
		struct CHandlerEntry
		{
			NStr::CStr m_Path;
			CHTTPRequestHandler *m_pHandler;
			NStorage::TCSharedPointer<FActorRequestHandler> m_pHandleRequest;
			NConcurrency::TCActor<CHTTPRequestHandlerActor> m_HandlerActor;
		};

		static void fsp_ReportRequestError(NStorage::TCSharedPointer<CFastCGIRequest> const& _pRequest, uint32 _Status, NStr::CStr const& _Error)
		{
			CHTTPResponseHeader Header;
			NStr::CStr Content = NStr::CStr::CFormat("{}: {}") << _Status << _Error;
			Header.f_SetPlainMime();
			Header.m_ContentLength = Content.f_GetLen();
			Header.m_AllowMethods = "GET, HEAD";
			Header.m_Status = _Status;
			_pRequest->f_SendStdError(Content);
			_pRequest->f_SendStdOutput(Header.f_Generate());
			_pRequest->f_SendStdOutput(Content);
			_pRequest->f_FinishRequest();
		}

		class CConnection : public CHTTPConnection
		{
		private:

			CHTTPResponseHeader mp_Header;
			NStream::CBinaryStreamMemory<NMib::NStream::CBinaryStreamDefault> mp_ContentStream;

			CHTTPServerInternal& mp_Internal;

		public:

			NStorage::TCSharedPointer<CHTTPRequest> mp_pHTTPRequest = fg_Construct();
			NStorage::TCSharedPointer<CFastCGIRequest> mp_pRequest;
			bool mp_bSentAsyncHeader = false;
			DMibIfEnableSafeCheck(bool mp_bWroteNonAsync = false);

			CConnection(NStorage::TCSharedPointer<CFastCGIRequest> &&_pRequest, CHTTPServerInternal& _Internal)
				: mp_pRequest(fg_Move(_pRequest))
				, mp_ContentStream(NContainer::CByteVector())
				, mp_Internal(_Internal)
			{
			}

			virtual ~CConnection()
			{

			}

			void f_WriteStr(NStr::CStr _Str) override
			{
				DMibFastCheck(!mp_bSentAsyncHeader); // You can't mix async and non-async
				DMibIfEnableSafeCheck(mp_bWroteNonAsync = true);

				mp_ContentStream.f_FeedBytes((void const*)_Str.f_GetStr(), _Str.f_GetLen());
			}

			void f_WriteBinary(uint8 const* _pData, mint _nBytes) override
			{
				DMibFastCheck(!mp_bSentAsyncHeader); // You can't mix async and non-async
				DMibIfEnableSafeCheck(mp_bWroteNonAsync = true);

				mp_ContentStream.f_FeedBytes((void const*)_pData, _nBytes);
			}

			void f_WriteHeader(CHTTPResponseHeader const& _Header) override
			{
				DMibFastCheck(!mp_bSentAsyncHeader); // You can't mix async and non-async
				DMibIfEnableSafeCheck(mp_bWroteNonAsync = true);

				mp_Header = _Header;
			}

			NConcurrency::TCFuture<void> f_WriteAsyncStr(NStr::CStr _Str) override
			{
				DMibFastCheck(!mp_bWroteNonAsync); // You can't mix async and non-async
				DMibFastCheck(mp_bSentAsyncHeader); // You need to send the header first

				return mp_pRequest->f_SendAsyncStdOutput(fg_Move(_Str));
			}

			NConcurrency::TCFuture<void> f_WriteAsyncBinary(NContainer::CIOByteVector _Vector) override
			{
				DMibFastCheck(!mp_bWroteNonAsync); // You can't mix async and non-async
				DMibFastCheck(mp_bSentAsyncHeader); // You need to send the header first

				return mp_pRequest->f_SendAsyncStdOutput(fg_Move(_Vector));
			}

			NConcurrency::TCFuture<void> f_WriteAsyncHeader(CHTTPResponseHeader _Header) override
			{
				DMibFastCheck(!mp_bWroteNonAsync); // You can't mix async and non-async
				DMibFastCheck(!mp_bSentAsyncHeader); // You can't send header twice

				NStr::CStr HeaderStr = mp_Header.f_Generate();
				mp_bSentAsyncHeader = true;

				return mp_pRequest->f_SendAsyncStdOutput(fg_Move(HeaderStr));
			}

			void f_Send()
			{
				if (!mp_bSentAsyncHeader)
				{
					mp_Header.m_ContentLength = mp_ContentStream.f_GetLength();

					NStr::CStr HeaderStr = mp_Header.f_Generate();

					mp_pRequest->f_SendStdOutput(HeaderStr);
					mp_pRequest->f_SendStdOutput((uint8 const*)mp_ContentStream.f_GetBuffer(), mp_Header.m_ContentLength);
				}

				mp_pRequest->f_FinishRequest();
			}

			void fp_ParseVariables(char const* _pData, mint _Len)
			{
				char const* pCurPos = _pData;
				char const *pEntryEndPos, *pEqualsPos;
				char const* pEnd = _pData + _Len;

				while (*pCurPos && pCurPos < pEnd)
				{
					pEqualsPos = nullptr;
					for (pEntryEndPos = pCurPos; *pEntryEndPos && *pEntryEndPos != '&' && pEntryEndPos < pEnd; ++pEntryEndPos)
					{
						if (*pEntryEndPos == '=')
							pEqualsPos = pEntryEndPos;
					}

					if (!pEqualsPos)
					{
						pCurPos = pEntryEndPos + 1;
						continue;
					}

					size_t StrLen;
					NStr::CStr CurKey, CurValue;

					StrLen = (pEqualsPos - pCurPos);
					fs_URLDecode(pCurPos, StrLen, CurKey.f_GetStr(StrLen), StrLen + 1, true);
					++pEqualsPos;
					StrLen = (pEntryEndPos - pEqualsPos);
					fs_URLDecode(pEqualsPos, StrLen, CurValue.f_GetStr(StrLen), StrLen + 1, true);

					mp_pHTTPRequest->m_Variables[CurKey] = CurValue;

					pCurPos = pEntryEndPos + 1;
				}
			}

			NConcurrency::TCFuture<void> f_HandleRequest(NStorage::TCSharedPointer<CConnection> const& _pThis, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Params)
			{
				NConcurrency::TCPromise<void> Promise;

				mp_pHTTPRequest->m_RequestedURI = _Params["DOCUMENT_URI"];
				auto* pRemoteIP = _Params.f_FindEqual("REMOTE_ADDR");
				if (pRemoteIP)
					mp_pHTTPRequest->m_ClientIP = *pRemoteIP;

				auto* pMethod = _Params.f_FindEqual("REQUEST_METHOD");
				if (pMethod)
					mp_pHTTPRequest->m_Method = *pMethod;

				auto* pQuery = _Params.f_FindEqual("QUERY_STRING");
				if (pQuery && !pQuery->f_IsEmpty())
					fp_ParseVariables(pQuery->f_GetStr(), pQuery->f_GetLen());

				for (auto &Param : _Params)
				{
					auto &Key = _Params.fs_GetKey(Param);

					if (!Key.f_StartsWith("HTTP_"))
						continue;

					mp_pHTTPRequest->m_Headers[Key.f_RemovePrefix("HTTP_").f_LowerCase()] = Param;
				}

				if (fg_StrCmpNoCase(*pMethod, "GET") == 0)
				{

				}
				else if (fg_StrCmpNoCase(*pMethod, "POST") == 0)
				{
					NStr::CStr ContentLengthField;// = mg_get_header(_pConnection, "CONTENT_LENGTH");
					auto* pContentLength = _Params.f_FindEqual("CONTENT_LENGTH");
					if (!pContentLength)
					{
						fsp_ReportRequestError(_pThis->mp_pRequest, 500, NStr::fg_Format("CONTENT_LENGTH not found for POST"));
						return Promise <<= g_Void;
					}

					int nPostBytes = ContentLengthField.f_ToInt();
					NStorage::TCSharedPointer<NContainer::CByteVector> pData = fg_Construct();

					mp_pRequest->f_OnStdInputRaw
						(
							NConcurrency::g_ActorFunctorWeak(NConcurrency::fg_ConcurrentActor())
							/ [nPostBytes, pData, _pThis](NContainer::CByteVector _Data, bool _bEOF) -> NConcurrency::TCFuture<void>
							{
								pData->f_Insert(fg_Move(_Data));

								if (_bEOF)
								{
									if (nPostBytes != pData->f_GetLen())
										fsp_ReportRequestError(_pThis->mp_pRequest, 500, NStr::fg_Format("Invalid CONTENT_LENGTH"));
									else
									{
										_pThis->fp_ParseVariables((ch8 const*)pData->f_GetArray(), pData->f_GetLen());
										co_await _pThis->f_HandleRequest();
									}
								}

								co_return {};
							}
						)
					;

					mp_pRequest->f_Accept();
					return Promise <<= g_Void;
				}
				mp_pRequest->f_Accept();

				return Promise <<= _pThis->f_HandleRequest();
			}

			NConcurrency::TCFuture<void> f_HandleRequest()
			{
				NConcurrency::TCPromise<void> Promise;

				auto &Internal = mp_Internal;
				bool bHandled = false;
				if (Internal.mp_bActorHandlers)
				{
					struct CHandlers
					{
						NContainer::TCVector<NStorage::TCVariant<NStorage::TCSharedPointer<FActorRequestHandler>, NConcurrency::TCActor<CHTTPRequestHandlerActor>>> m_Handlers;
						mint m_iHandler = 0;

						NFunction::TCFunction
							<
								void
								(
									NStorage::TCSharedPointer<CHandlers> const &_pHandlers
									, NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection
									, NStorage::TCSharedPointer<CHTTPRequest> const &_pRequest
								)
							>
							m_fHandleNext
						;
					};

					NStorage::TCSharedPointer<CHTTPConnection> pConnection = fg_Explicit(this);

					auto pRequest = mp_pRequest;

					NStorage::TCSharedPointer<CHandlers> pHandlers = fg_Construct();
					pHandlers->m_fHandleNext = [this]
						(
							NStorage::TCSharedPointer<CHandlers> const &_pHandlers
							, NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection
							, NStorage::TCSharedPointer<CHTTPRequest> const &_pRequest
						)
						{
							if (_pHandlers->m_iHandler >= _pHandlers->m_Handlers.f_GetLen())
							{
								fsp_ReportRequestError(mp_pRequest, 404, NStr::fg_Format("URI not found: {}\n", mp_pHTTPRequest->m_RequestedURI));
								return;
							}

							auto &Handler = _pHandlers->m_Handlers[_pHandlers->m_iHandler];

							NConcurrency::TCFuture<bool> ResultFuture;
							if (Handler.f_GetTypeID() == 0)
								ResultFuture = (*Handler.f_Get<0>())(_pConnection, _pRequest);
							else
								ResultFuture = Handler.f_Get<1>()(&CHTTPRequestHandlerActor::f_HandleRequest, _pConnection, _pRequest);

							fg_Move(ResultFuture) > [this, _pHandlers, _pConnection, _pRequest](NConcurrency::TCAsyncResult<bool> &&_Result)
								{
									if (!_Result)
									{
										DMibLog(Error, "Internal error: {}", _Result.f_GetExceptionStr());
										fsp_ReportRequestError(mp_pRequest, 500, "Internal error");
										return;
									}
									if (*_Result)
									{
										f_Send();
										return;
									}
									++_pHandlers->m_iHandler;
									_pHandlers->m_fHandleNext(_pHandlers, _pConnection, _pRequest);
								}
							;
						}
					;

					for (auto &Handler : Internal.mp_Handlers)
					{
						for (auto &InnerHandler : Handler)
						{
							if (mp_pHTTPRequest->m_RequestedURI.f_FindNoCase(InnerHandler.m_Path) == 0)
							{
								if (InnerHandler.m_pHandleRequest)
									pHandlers->m_Handlers.f_Insert(InnerHandler.m_pHandleRequest);
								else
									pHandlers->m_Handlers.f_Insert(InnerHandler.m_HandlerActor);
							}
						}
					}

					pHandlers->m_fHandleNext(pHandlers, pConnection, mp_pHTTPRequest);

					return Promise.f_Future();
				}
				
				try
				{
					for (auto iHandler = Internal.mp_Handlers.f_GetIterator(); !bHandled && iHandler; ++iHandler)
					{
						for (auto iInnerHandler = iHandler->f_GetIterator(); !bHandled && iInnerHandler; ++iInnerHandler)
						{
							if (mp_pHTTPRequest->m_RequestedURI.f_FindNoCase(iInnerHandler->m_Path) == 0)
								bHandled = iInnerHandler->m_pHandler->f_HandleRequest(*this, *mp_pHTTPRequest);
						}
					}
				}
				catch (NException::CException const &_Exception)
				{
					fsp_ReportRequestError(mp_pRequest, 500, NStr::fg_Format("Internal error: {}\n", _Exception.f_GetErrorStr()));
					return Promise <<= g_Void;
				}

				if (bHandled)
					f_Send();
				else
					fsp_ReportRequestError(mp_pRequest, 404, NStr::fg_Format("URI not found: {}\n", mp_pHTTPRequest->m_RequestedURI));

				return Promise <<= g_Void;
			}
		};

		static mint fs_URLDecode(ch8 const* _pSrc, mint _SrcLen, ch8 *_pDst, mint _DstLen, bool _bFormURL)
		{
			mint j = 0;
			for (mint i = 0; i < _SrcLen && j < _DstLen - 1; i++, j++)
			{
				if
					(
						_pSrc[i] == '%'
						&& NStr::fg_CharIsHexNumber(* (uch8 *) (_pSrc + i + 1))
						&& NStr::fg_CharIsHexNumber(* (uch8 *) (_pSrc + i + 2))
					)
				{
					uch8 Upper = NStr::fg_CharLowerCase(* (uch8 *) (_pSrc + i + 1));
					uch8 Lower = NStr::fg_CharLowerCase(* (uch8 *) (_pSrc + i + 2));
					uint8 UpperValue = NStr::fg_CharIsNumber(Upper) ? (Upper - '0') : (10 + (Upper - 'a'));
					uint8 LowerValue = NStr::fg_CharIsNumber(Lower) ? (Lower - '0') : (10 + (Lower - 'a'));
					_pDst[j] = (ch8)(UpperValue << 4 | LowerValue);

					i += 2;
				}
				else if (_bFormURL && _pSrc[i] == '+')
					_pDst[j] = ' ';
				else
					_pDst[j] = _pSrc[i];
			}

			_pDst[j] = 0;

			return j;
		}


	public:
		CHTTPServerInternal()
		{
		}

		~CHTTPServerInternal()
		{
			f_Stop();
		}

		void f_AddHandlerForPath(NStr::CStr _Path, CHTTPRequestHandler *_pHandler, int _Priority)
		{
			if (mp_bActorHandlers)
				DMibError("Cannot mix actor an non-actor handlers");

			CHandlerEntry &NewEntry = mp_Handlers[_Priority].f_Insert();
			NewEntry.m_Path = _Path;
			NewEntry.m_pHandler = _pHandler;
		}

		void f_AddHandlerActorFunctorForPath(NStr::CStr _Path, FActorRequestHandler _fHandler, int _Priority)
		{
			if (!mp_bActorHandlers)
			{
				if (!mp_Handlers.f_IsEmpty())
					DMibError("Cannot mix actor an non-actor handlers");
				mp_bActorHandlers = true;
			}
			CHandlerEntry &NewEntry = mp_Handlers[_Priority].f_Insert();
			NewEntry.m_Path = _Path;
			NewEntry.m_pHandleRequest = fg_Construct(fg_Move(_fHandler));
		}

		void f_AddHandlerActorForPath(NStr::CStr _Path, NConcurrency::TCActor<CHTTPRequestHandlerActor> _HandlerActor, int _Priority)
		{
			if (!mp_bActorHandlers)
			{
				if (!mp_Handlers.f_IsEmpty())
					DMibError("Cannot mix actor an non-actor handlers");
				mp_bActorHandlers = true;
			}
			CHandlerEntry &NewEntry = mp_Handlers[_Priority].f_Insert();
			NewEntry.m_Path = _Path;
			NewEntry.m_HandlerActor = fg_Move(_HandlerActor);
		}

		bool f_Run(CHTTPServerOptions _Options)
		{
			if (f_IsRunning())
				return false;

			mp_Options = _Options;

			if (mp_Options.m_bUseNginx)
			{
				mp_pNGINXLauncher = fg_Construct(mp_Options.m_NGINXPath, mp_Options.m_WebRoot);
				mp_pNGINXLauncher->f_SetFastCGIListen(mp_Options.m_FastCGIListenStartPort, mp_Options.m_nMaxThreads);
				mp_pNGINXLauncher->f_SetListen(mp_Options.m_ListeningPort);
				mp_pNGINXLauncher->f_SetStaticRoot(mp_Options.m_StaticRoot);
				mp_pNGINXLauncher->f_Launch();
			}

			mp_pFastCGIServer = fg_Construct();
			mp_pFastCGIServer.f_Bind<&CFastCGIServer::f_StartListenAddress>
				(
					NConcurrency::g_ActorFunctorWeak(NConcurrency::fg_DynamicConcurrentActor())
					/ [this](NStorage::TCSharedPointer<CFastCGIRequest> _pRequest) -> NConcurrency::TCFuture<void>
					{
						auto& Params = _pRequest->f_GetParams();
						auto* pURI = Params.f_FindEqual("DOCUMENT_URI");

						if (!pURI)
						{
							fsp_ReportRequestError(_pRequest, 500, "No URI specified");
							co_return {};
						}

						NStr::CStr URI = *pURI;

						// Check registered handlers
						NStorage::TCSharedPointer<CConnection> pConnection = fg_Construct(fg_Move(_pRequest), *this);

						co_await pConnection->f_HandleRequest(pConnection, Params);

						co_return {};
					}
					, mp_Options.m_FastCGIListenAddresses
				)
				.f_DiscardResult()
			;
			return true;
		}

		bool f_IsRunning()
		{
			return !mp_pFastCGIServer.f_IsEmpty();
		}

		bool f_Stop()
		{
			mp_pNGINXLauncher.f_Clear();
			if (mp_pFastCGIServer)
			{
				mp_pFastCGIServer->f_BlockDestroy();
				mp_pFastCGIServer.f_Clear();
			}

			return true;
		}

	private:
		CHTTPServerOptions mp_Options;
		NStorage::TCUniquePointer<CNGINXLauncher> mp_pNGINXLauncher;
		NConcurrency::TCActor<CFastCGIServer> mp_pFastCGIServer;

		NContainer::TCMap<int, NContainer::TCVector<CHandlerEntry> > mp_Handlers;

		NFunction::TCFunction<void(NStr::CStr const &_Log, bool _bError)> mp_LogFunction;
		bool mp_bActorHandlers = false;
	};



	// CHTTPServer Public Methods

	CHTTPServer::CHTTPServer()
	{
		mp_Internal = fg_Construct(fg_Construct(), "HTTP Server");
	}

	CHTTPServer::~CHTTPServer()
	{
		mp_Internal.f_Clear();
	}

	void CHTTPServer::f_AddHandlerForPath(NStr::CStr const& _Path, CHTTPRequestHandler* _pHandler, int _Priority)
	{
		mp_Internal(&CHTTPServerInternal::f_AddHandlerForPath, _Path, _pHandler, _Priority).f_CallSync();
	}

	void CHTTPServer::f_AddHandlerActorForPath(NStr::CStr const &_Path, FActorRequestHandler &&_fHandler, int _Priority)
	{
		mp_Internal(&CHTTPServerInternal::f_AddHandlerActorFunctorForPath, _Path, fg_Move(_fHandler), _Priority).f_CallSync();
	}

	void CHTTPServer::f_AddHandlerActorForPath(NStr::CStr const &_Path, NConcurrency::TCActor<CHTTPRequestHandlerActor> &&_HandlerActor, int _Priority)
	{
		mp_Internal(&CHTTPServerInternal::f_AddHandlerActorForPath, _Path, fg_Move(_HandlerActor), _Priority).f_CallSync();
	}

	bool CHTTPServer::f_Run(CHTTPServerOptions const& _Options)
	{
		return mp_Internal(&CHTTPServerInternal::f_Run, _Options).f_CallSync();
	}

	bool CHTTPServer::f_IsRunning()
	{
		return mp_Internal(&CHTTPServerInternal::f_IsRunning).f_CallSync();
	}

	bool CHTTPServer::f_Stop()
	{
		return mp_Internal(&CHTTPServerInternal::f_Stop).f_CallSync();
	}

	NConcurrency::TCFuture<void> CHTTPServer::f_AddHandlerActorForPathAsync(NStr::CStr _Path, FActorRequestHandler _fHandler, int _Priority)
	{
		return mp_Internal(&CHTTPServerInternal::f_AddHandlerActorFunctorForPath, fg_Move(_Path), fg_Move(_fHandler), _Priority);
	}

	NConcurrency::TCFuture<void> CHTTPServer::f_AddHandlerActorForPathAsync(NStr::CStr _Path, NConcurrency::TCActor<CHTTPRequestHandlerActor> _HandlerActor, int _Priority)
	{
		return mp_Internal(&CHTTPServerInternal::f_AddHandlerActorForPath, fg_Move(_Path), fg_Move(_HandlerActor), _Priority);
	}

	NConcurrency::TCFuture<bool> CHTTPServer::f_RunAsync(CHTTPServerOptions _Options)
	{
		return mp_Internal(&CHTTPServerInternal::f_Run, fg_Move(_Options));
	}

	NConcurrency::TCFuture<bool> CHTTPServer::f_IsRunningAsync()
	{
		return mp_Internal(&CHTTPServerInternal::f_IsRunning);
	}

	NConcurrency::TCFuture<bool> CHTTPServer::f_StopAsync()
	{
		return mp_Internal(&CHTTPServerInternal::f_Stop);
	}

	// CHTTPResponseHeader Public Methods

	CHTTPResponseHeader::CHTTPResponseHeader() = default;
	CHTTPResponseHeader::CHTTPResponseHeader(CHTTPResponseHeader const& _ToCopy) = default;
	CHTTPResponseHeader::CHTTPResponseHeader(CHTTPResponseHeader && _ToMove) = default;
	CHTTPResponseHeader& CHTTPResponseHeader::operator=(CHTTPResponseHeader const& _ToCopy) = default;
	CHTTPResponseHeader& CHTTPResponseHeader::operator=(CHTTPResponseHeader && _ToMove) = default;

	NStr::CStr CHTTPResponseHeader::f_Generate() const
	{
		NStr::CStr Response;
		uint32 Status = m_Status;
		if (m_RedirectTo.f_IsEmpty())
		{
			switch (Status)
			{
			case 200:
				Response = "HTTP/1.1 200 OK";
				break;
			case 304:
				Response = "HTTP/1.1 304 Not Modified";
				break;
			case 400:
				Response = "HTTP/1.1 400 Bad Request";
				break;
			case 403:
				Response = "HTTP/1.1 403 Forbidden";
				break;
			case 404:
				Response = "HTTP/1.1 404 Not Found";
				break;
			case 500:
				Response = "HTTP/1.1 500 Internal Server Error";
				break;
			case 503:
				Response = "HTTP/1.1 503 Service Unavailable";
				break;
			default:
				Response = NStr::CStr::CFormat("HTTP/1.1 {} Unknown") << Status;
				DMibNeverGetHere;
				break;
			}
		}
		else
		{
			Status = 303;
			Response = "HTTP/1.1 303 See Other";
			Response += NStr::CStr::CFormat("\r\nLocation: {}") << m_RedirectTo;
		}

		Response += NStr::CStr::CFormat("\r\nStatus: {}") << Status;

		NTime::CTime NowTime = NTime::CTime::fs_NowUTC();
		Response += NStr::CStr::CFormat("\r\nDate: {}") << NTime::fg_GetAscTimeStr(NowTime);

		if (m_CacheControl)
			Response += NStr::CStr::CFormat("\r\nCache-Control: {}") << m_CacheControl;

		if (m_ETag)
			Response += NStr::CStr::CFormat("\r\nETag: {}") << m_ETag;

		if (m_Expires.f_IsValid())
		{
			if (m_Expires > NowTime)
				Response += NStr::CStr::CFormat("\r\nExpires: {}") << NTime::fg_GetAscTimeStr(m_Expires);
			else
				Response += NStr::CStr::CFormat("\r\nExpires: {}") << NTime::fg_GetAscTimeStr(NowTime);
		}
		else if (!m_CacheControl)
			Response += "\r\nExpires: 0";

		if (m_LastModified.f_IsValid())
		{
			if (m_LastModified < NowTime)
				Response += NStr::CStr::CFormat("\r\nLast-Modified: {}") << NTime::fg_GetAscTimeStr(m_LastModified);
		}

		Response += NStr::CStr::CFormat("\r\nContent-Type: {}") << m_MimeType;
		if (m_ContentLength)
			Response += NStr::CStr::CFormat("\r\nContent-Length: {}") << m_ContentLength;

		Response += NStr::CStr::CFormat("\r\nAllow: {}") << m_AllowMethods;

//		Response += "\r\nConnection: close";

		Response += "\r\n\r\n";

		return Response;
	}

	struct CMimeTypes
	{
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_Types;
		CMimeTypes()
		{
			m_Types["html"] = "text/html";
			m_Types["htm"] = "text/html";
			m_Types["shtm"] = "text/html";
			m_Types["shtml"] = "text/html";
			m_Types["css"] = "text/css";
			m_Types["js"] = "application/x-javascript";
			m_Types["ico"] = "image/x-icon";
			m_Types["gif"] = "image/gif";
			m_Types["jpg"] = "image/jpeg";
			m_Types["jpeg"] = "image/jpeg";
			m_Types["png"] = "image/png";
			m_Types["svg"] = "image/svg+xml";
			m_Types["torrent"] = "application/x-bittorrent";
			m_Types["wav"] = "audio/x-wav";
			m_Types["mp3"] = "audio/x-mp3";
			m_Types["mid"] = "audio/mid";
			m_Types["m3u"] = "audio/x-mpegurl";
			m_Types["ram"] = "audio/x-pn-realaudio";
			m_Types["xml"] = "text/xml";
			m_Types["xslt"] = "application/xml";
			m_Types["ra"] = "audio/x-pn-realaudio";
			m_Types["doc"] = "application/msword";
			m_Types["exe"] = "application/octet-stream";
			m_Types["zip"] = "application/x-zip-compressed";
			m_Types["xls"] = "application/excel";
			m_Types["tgz"] = "application/x-tar-gz";
			m_Types["tar"] = "application/x-tar";
			m_Types["gz"] = "application/x-gunzip";
			m_Types["arj"] = "application/x-arj-compressed";
			m_Types["rar"] = "application/x-arj-compressed";
			m_Types["rtf"] = "application/rtf";
			m_Types["pdf"] = "application/pdf";
			m_Types["swf"] = "application/x-shockwave-flash";
			m_Types["mpg"] = "video/mpeg";
			m_Types["mpeg"] = "video/mpeg";
			m_Types["asf"] = "video/x-ms-asf";
			m_Types["avi"] = "video/x-msvideo";
			m_Types["bmp"] = "image/bmp";
		}
	};

	CMimeTypes g_MimeTypes;

	void CHTTPResponseHeader::f_SetMimeTypeFromFilename(NStr::CStr const& _Filename)
	{
		auto pMimeType = g_MimeTypes.m_Types.f_FindEqual(NFile::CFile::fs_GetExtension(_Filename).f_LowerCase());

		if (pMimeType)
			m_MimeType = *pMimeType;
		else
			m_MimeType = "text/plain";
	}

	void CHTTPResponseHeader::f_SetPlainMime()
	{
		m_MimeType = "text/plain";
	}


	// CHTTPServer Options Public Methods

	void CHTTPServerOptions::f_ParseCmdLine(NContainer::CRegistry &_Params)
	{
		// Process command line args.
		NStr::CStr CurArg;
		for (auto PIter = _Params.f_GetChildIterator(); PIter; ++PIter)
		{
			NContainer::CRegistry const& CurChild = *PIter;
			CurArg = CurChild.f_GetThisValue();

			if (CurArg.f_CmpNoCase("-http_port") == 0)
			{
				++PIter;
				if (!PIter)
					break;
				m_ListeningPort = (*PIter).f_GetThisValue().f_ToInt(8080);
			}
			else if (CurArg.f_CmpNoCase("-fastcgi_listen") == 0)
			{
				++PIter;
				if (!PIter)
					break;
				m_FastCGIListenStartPort = (*PIter).f_GetThisValue().f_ToInt(9000);
			}
			else if (CurArg.f_CmpNoCase("-http_threads") == 0)
			{
				++PIter;
				if (!PIter)
					break;
				m_nMaxThreads = (*PIter).f_GetThisValue().f_ToInt(1);
			}
			else if (CurArg.f_CmpNoCase("-http_root") == 0)
			{
				++PIter;
				if (!PIter)
					break;
				m_StaticRoot = (*PIter).f_GetThisValue();
			}
			else if (CurArg.f_CmpNoCase("-http_webroot") == 0)
			{
				++PIter;
				if (!PIter)
					break;
				m_WebRoot = (*PIter).f_GetThisValue();
			}
			else if (CurArg.f_CmpNoCase("-nginx_path") == 0)
			{
				++PIter;
				if (!PIter)
					break;
				m_NGINXPath = (*PIter).f_GetThisValue();
			}
		}
	}

	void CHTTPServerOptions::f_ParseSettings(NContainer::CRegistry const& _Params)
	{
		fp_ParseSettings(_Params);
	}

	void CHTTPServerOptions::f_ParseSettings(NContainer::CRegistryPreserveWhitespace const& _Params)
	{
		fp_ParseSettings(_Params);
	}

	template <typename tf_CRegistry>
	void CHTTPServerOptions::fp_ParseSettings(tf_CRegistry const& _Params)
	{
		// Process settings
		for (auto PIter = _Params.f_GetChildIterator(); PIter; ++PIter)
		{
			auto const& CurChild = *PIter;
			auto CurArg = CurChild.f_GetName();
			auto CurValue = CurChild.f_GetThisValue();

			if (CurArg.f_CmpNoCase("http_port") == 0)
			{
				m_ListeningPort = CurValue.f_ToInt(8080);
			}
			else if (CurArg.f_CmpNoCase("fastcgi_listen") == 0)
			{
				m_FastCGIListenStartPort = CurValue.f_ToInt(9000);
			}
			else if (CurArg.f_CmpNoCase("http_threads") == 0)
			{
				m_nMaxThreads = CurValue.f_ToInt(1);
			}
			else if (CurArg.f_CmpNoCase("http_root") == 0)
			{
				m_StaticRoot = CurValue;
			}
			else if (CurArg.f_CmpNoCase("http_webroot") == 0)
			{
				m_WebRoot = CurValue;
			}
			else if (CurArg.f_CmpNoCase("nginx_path") == 0)
			{
				m_NGINXPath = CurValue;
			}
		}
	}

	void CHTTPServerOptions::f_ParseSettings(NEncoding::CEJsonSorted const &_Params)
	{
		m_ListeningPort = _Params.f_GetMemberValue("Port", m_ListeningPort).f_Integer();
		m_FastCGIListenStartPort = _Params.f_GetMemberValue("FCGIPort", m_FastCGIListenStartPort).f_Integer();
		m_nMaxThreads = _Params.f_GetMemberValue("Threads", m_nMaxThreads).f_Integer();
		m_StaticRoot = _Params.f_GetMemberValue("StaticRoot", m_StaticRoot).f_String();
		m_WebRoot = _Params.f_GetMemberValue("WebRoot", m_WebRoot).f_String();
		m_NGINXPath = _Params.f_GetMemberValue("NginxPath", m_NGINXPath).f_String();
	}

	// For Development Use Only

	class CTestHTTPRequestHandler : public CHTTPRequestHandler
	{
	public:
		CTestHTTPRequestHandler()
		{

		}

		virtual ~CTestHTTPRequestHandler()
		{

		}

		// This could be called for any thread - assume nothing.
		bool f_HandleRequest(CHTTPConnection &_Connection, CHTTPRequest const& _Req)
		{
			CHTTPResponseHeader ResponseHeader;
			_Connection.f_WriteHeader(ResponseHeader);

			NStr::CStr Msg = "Hello from the handler!<BR/>";

			_Connection.f_WriteStr(Msg);

			return true;
		}
	};

	NStr::CStr CHTTPRequest::f_GetVariable(NStr::CStr const &_Variable, NStr::CStr const &_DefaultValue)
	{
		if (auto pVariable = m_Variables.f_FindEqual(_Variable))
			return *pVariable;
		return _DefaultValue;
	}

	NStr::CStr CHTTPRequest::fs_LookupHost(NStr::CStr const& _IP)
	{
		uint8 IP0 = 0;
		uint8 IP1 = 0;
		uint8 IP2 = 0;
		uint8 IP3 = 0;
		aint nParsed = 0;
		(NStr::CStr::CParse("{}.{}.{}.{}") >> IP0 >> IP1 >> IP2 >> IP3).f_Parse(_IP, nParsed);
		if (nParsed == 4)
		{
			uint32 ClientIP = uint32(IP0) << 24 | uint32(IP1) << 16 | uint32(IP2) << 8 | uint32(IP3);

			auto Addr = htonl(ClientIP);
			auto pHost = gethostbyaddr((const char*)&Addr, 4, AF_INET);
			if (pHost)
				return pHost->h_name;
		}
		return _IP;
	}


	// CHTMLTemplate

	CHTMLTemplate::CHTMLTemplate(NStr::CStr const& _Filename, bool _bExeFs)
	{
		NStr::CStr Template;
		if (!_bExeFs)
		{
			try
			{
				Template = NFile::CFile::fs_ReadStringFromFile(NStr::CStr(_Filename));
			}
			catch(NFile::CExceptionFile)
			{
			}
		}
		else
		{
			NContainer::CByteVector lFileBytes;
			if (NMib::NFile::fg_ReadExeFSFile(_Filename, lFileBytes))
			{
				char* pBuf = Template.f_GetStr(lFileBytes.f_GetLen() + 1);
				memcpy(pBuf, lFileBytes.f_GetArray(), lFileBytes.f_GetLen());
				pBuf[lFileBytes.f_GetLen()] = 0;
			}
		}

		// <!--USERDATA(CONTENT)-->

		const NStr::CStr TagStart = "<!--USERDATA(";
		const NStr::CStr TagEnd = ")-->";

		auto &Blocks = mp_pState->m_Blocks;

		int iTagPos, iEndTagPos;
		CBlock NewBlock;
		while ((iTagPos = Template.f_Find(TagStart)) != -1)
		{
			if (iTagPos > 0)
			{
				CBlock &NewBlock = Blocks.f_Insert();
				NewBlock.m_Type = EBlock_HTML;
				NewBlock.m_Text = Template.f_Extract(0, iTagPos);
				Template = Template.f_Extract(iTagPos + TagStart.f_GetLen());
			}

			iEndTagPos = Template.f_Find(TagEnd);

			if (iEndTagPos == -1)
				break;

			CBlock &NewBlock = Blocks.f_Insert();
			NewBlock.m_Type = EBlock_UserData;
			NewBlock.m_Text = Template.f_Extract(0, iEndTagPos);

			Template = Template.f_Extract(iEndTagPos + TagEnd.f_GetLen());
		}

		if (!Template.f_IsEmpty())
		{
			CBlock &NewBlock = Blocks.f_Insert();
			NewBlock.m_Type = EBlock_HTML;
			NewBlock.m_Text = Template;
		}
	}

	CHTMLTemplate::~CHTMLTemplate()
	{

	}

	///
	/// CHTTPServerOptions
	/// ==================

	CHTTPServerOptions::CHTTPServerOptions()
		: m_nMaxThreads(NSys::fg_Thread_GetVirtualCores())
		, m_ListeningPort(8080)
		, m_FastCGIListenStartPort(9000)
		, m_NGINXPath(NFile::CFile::fs_AppendPath(NFile::CFile::fs_GetProgramDirectory(), "nginx/nginx"))
		, m_WebRoot(NFile::CFile::fs_AppendPath(NFile::CFile::fs_GetProgramDirectory(), "webroot"))
	{
	}


	///
	/// CHTTPCachedConnection
	/// =====================

	CHTTPCachedConnection::CHTTPCachedConnection() : mp_ContentStream(NContainer::CByteVector())
	{
		f_Clear();
	}

	CHTTPCachedConnection::CHTTPCachedConnection(CHTTPCachedConnection const& _ToCopy)
	{
		*this = _ToCopy;
	}

	CHTTPCachedConnection::CHTTPCachedConnection(CHTTPCachedConnection && _ToMove)
	{
		*this = fg_Move(_ToMove);
	}

	CHTTPCachedConnection::~CHTTPCachedConnection()
	{

	}

	CHTTPCachedConnection& CHTTPCachedConnection::operator= (CHTTPCachedConnection const& _ToCopy)
	{
		mp_Present = _ToCopy.mp_Present;
		mp_Timestamp = _ToCopy.mp_Timestamp;
		mp_Header = _ToCopy.mp_Header;
		mp_ContentStream.f_Clear();
		mp_ContentStream.f_FeedBytes(_ToCopy.mp_ContentStream.f_GetBufferConst(), _ToCopy.mp_ContentStream.f_GetLength());
		return *this;
	}

	CHTTPCachedConnection& CHTTPCachedConnection::operator= (CHTTPCachedConnection && _ToMove)
	{
		mp_Present = _ToMove.mp_Present;
		_ToMove.mp_Present = EPresent_None;

		mp_Timestamp = fg_Move(_ToMove.mp_Timestamp);
		mp_Header = fg_Move(_ToMove.mp_Header);
		mp_ContentStream.f_Open(fg_Move(_ToMove.mp_ContentStream.f_MoveVector()));
		return *this;
	}

	void CHTTPCachedConnection::f_WriteStr(NStr::CStr _Str)
	{
		mp_ContentStream.f_FeedBytes((void const*)_Str.f_GetStr(), _Str.f_GetLen());
		mp_Present |= EPresent_Content;
	}

	void CHTTPCachedConnection::f_WriteBinary(uint8 const* _pData, mint _nBytes)
	{
		mp_ContentStream.f_FeedBytes((void const*)_pData, _nBytes);
		mp_Present |= EPresent_Content;
	}

	void CHTTPCachedConnection::f_WriteHeader(CHTTPResponseHeader const& _Header)
	{
		mp_Header = _Header;
		mp_Present |= EPresent_Header;
	}

	NConcurrency::TCFuture<void> CHTTPCachedConnection::f_WriteAsyncStr(NStr::CStr _Str)
	{
		mp_ContentStream.f_FeedBytes((void const*)_Str.f_GetStr(), _Str.f_GetLen());
		mp_Present |= EPresent_Content;

		co_return {};
	}

	NConcurrency::TCFuture<void> CHTTPCachedConnection::f_WriteAsyncBinary(NContainer::CIOByteVector _Vector)
	{
		mp_ContentStream.f_FeedBytes(_Vector.f_GetArray(), _Vector.f_GetLen());
		mp_Present |= EPresent_Content;

		co_return {};
	}

	NConcurrency::TCFuture<void> CHTTPCachedConnection::f_WriteAsyncHeader(CHTTPResponseHeader _Header)
	{
		mp_Header = _Header;
		mp_Present |= EPresent_Header;

		co_return {};
	}

	bool CHTTPCachedConnection::f_HasHeader()
	{
		return mp_Present & EPresent_Header;
	}
	bool CHTTPCachedConnection::f_HasContent()
	{
		return mp_Present & EPresent_Content;
	}

	void CHTTPCachedConnection::f_SetTimestamp()
	{
		mp_Timestamp = NTime::CTime::fs_NowUTC();
	}
	NTime::CTime CHTTPCachedConnection::f_GetTimestamp()
	{
		return mp_Timestamp;
	}

	bool CHTTPCachedConnection::f_IsValid(fp32 _MaxSecondsOld)
	{
		if (!mp_Timestamp.f_IsValid())
			return false;

		if ( (NTime::CTime::fs_NowUTC() - mp_Timestamp).f_GetSeconds() > _MaxSecondsOld )
			return false;
		else
			return true;
	}

	void CHTTPCachedConnection::f_Forward(CHTTPConnection& _Dest)
	{
		if (mp_Present & EPresent_Header)
			_Dest.f_WriteHeader(mp_Header);

		if (mp_Present & EPresent_Content)
			_Dest.f_WriteBinary((uint8 const*)mp_ContentStream.f_GetBuffer(), mp_ContentStream.f_GetLength());
	}

	void CHTTPCachedConnection::f_Clear()
	{
		mp_Present = EPresent_None;
		mp_ContentStream.f_Clear();
	}

	void CHTMLTemplate::fsp_AsyncWrite(NStorage::TCSharedPointer<CAsyncWriteState> const &_pState)
	{
		auto &WriteState = *_pState;
		auto &State = *WriteState.m_pState;

		mint nBlocks = State.m_Blocks.f_GetLen();

		if (WriteState.m_iBlock == nBlocks)
		{
			WriteState.m_Promise.f_SetResult();
			return;
		}

		for (; WriteState.m_iBlock < nBlocks; ++WriteState.m_iBlock)
		{
			auto &Block = State.m_Blocks[WriteState.m_iBlock];

			if (Block.m_Type == EBlock_HTML)
				WriteState.m_pConnection->f_WriteStr(Block.m_Text);
			else if (Block.m_Type == EBlock_UserData)
			{
				fg_CallSafe(WriteState.m_fWriteBlock, Block.m_Text) > WriteState.m_Promise / [_pState]
					{
						++_pState->m_iBlock;
						fsp_AsyncWrite(_pState);
					}
				;
				return;
			}
		}

		if (WriteState.m_iBlock == nBlocks)
			WriteState.m_Promise.f_SetResult();
	}

	NConcurrency::TCFuture<void> CHTMLTemplate::f_WriteTemplateAsync
		(
			 NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection
			 , CHTTPResponseHeader const &_BaseHeader
			 , NFunction::TCFunction<NConcurrency::TCFuture<void> (NStr::CStr const &_BlockName)> &&_fWriteBlock
		)
	{
		_pConnection->f_WriteHeader(_BaseHeader);

		NStorage::TCSharedPointer<CAsyncWriteState> pWriteState = fg_Construct();

		pWriteState->m_pState = mp_pState;
		pWriteState->m_pConnection = _pConnection;
		pWriteState->m_fWriteBlock = fg_Move(_fWriteBlock);

		fsp_AsyncWrite(pWriteState);

		return pWriteState->m_Promise.f_Future();
	}
}
