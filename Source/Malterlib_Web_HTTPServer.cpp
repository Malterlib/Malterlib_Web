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

namespace NMib
{
	namespace NWeb
	{
		// CHTTPServerInternal

		class CHTTPServerInternal
		{
		private:

			CHTTPServerOptions mp_Options;
			NStorage::TCUniquePointer<CNGINXLauncher> mp_pNGINXLauncher;
			NConcurrency::TCActor<CFastCGIServer> mp_pFastCGIServer;

			struct CHandlerEntry
			{
				NStr::CStr m_Path;
				CHTTPRequestHandler *m_pHandler;
			};

			NContainer::TCMap<int, NContainer::TCVector<CHandlerEntry> > mp_Handlers;

			NFunction::TCFunction<void(NStr::CStr const &_Log, bool _bError)> mp_LogFunction;

			static void fp_ReportRequestError(NStorage::TCSharedPointer<CFastCGIRequest> const& _pRequest, uint32 _Status, NStr::CStr const& _Error)
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

		private:

			class CConnection : public CHTTPConnection
			{
			private:

				CHTTPResponseHeader mp_Header;
				NStream::CBinaryStreamMemory<NMib::NStream::CBinaryStreamDefault> mp_ContentStream;

				CHTTPServerInternal& mp_Internal;

			public:

				CHTTPRequest mp_Request;
				NStorage::TCSharedPointer<CFastCGIRequest> mp_pRequest;

				CConnection(NStorage::TCSharedPointer<CFastCGIRequest> const& _pRequest, CHTTPServerInternal& _Internal)
					: mp_pRequest(_pRequest)
					, mp_ContentStream(NContainer::CByteVector())
					, mp_Internal(_Internal)
				{
				}

				virtual ~CConnection()
				{

				}

				void f_Write(NStr::CStr _Str)
				{
					mp_ContentStream.f_FeedBytes((void const*)_Str.f_GetStr(), _Str.f_GetLen());
				}

				void f_Write(char const* _pStr)
				{
					mp_ContentStream.f_FeedBytes((void const*)_pStr, NStr::fg_StrLen(_pStr));
				}


				void f_Write(uint8 const* _pData, mint _nBytes)
				{
					mp_ContentStream.f_FeedBytes((void const*)_pData, _nBytes);
				}

				void f_Write(CHTTPResponseHeader const& _Header)
				{
					mp_Header = _Header;
				}

				void f_Send()
				{
					mp_Header.m_ContentLength = mp_ContentStream.f_GetLength();

					NStr::CStr HeaderStr = mp_Header.f_Generate();

					mp_pRequest->f_SendStdOutput(HeaderStr);
					mp_pRequest->f_SendStdOutput((uint8 const*)mp_ContentStream.f_GetBuffer(), mp_Header.m_ContentLength);
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

						mp_Request.m_Variables[CurKey] = CurValue;

						pCurPos = pEntryEndPos + 1;
					}
				}

				void f_HandleRequest(NStorage::TCSharedPointer<CConnection> const& _pThis, NContainer::TCMap<NStr::CStr, NStr::CStr> const& _Params)
				{
					mp_Request.m_RequestedURI = _Params["DOCUMENT_URI"];
					auto* pRemoteIP = _Params.f_FindEqual("REMOTE_ADDR");
					if (pRemoteIP)
						mp_Request.m_ClientIP = *pRemoteIP;

					auto* pMethod = _Params.f_FindEqual("REQUEST_METHOD");
					if (pMethod)
						mp_Request.m_Method = *pMethod;

					if (fg_StrCmpNoCase(*pMethod, "GET") == 0)
					{
						auto* pQuery = _Params.f_FindEqual("QUERY_STRING");
						if (pQuery && !pQuery->f_IsEmpty())
							fp_ParseVariables(pQuery->f_GetStr(), pQuery->f_GetLen());

					}
					else if (fg_StrCmpNoCase(*pMethod, "POST") == 0)
					{
						NStr::CStr ContentLengthField;// = mg_get_header(_pConnection, "CONTENT_LENGTH");
						auto* pContentLength = _Params.f_FindEqual("CONTENT_LENGTH");
						if (!pContentLength)
						{
							fp_ReportRequestError(_pThis->mp_pRequest, 500, NStr::fg_Format("CONTENT_LENGTH not found for POST"));
							return;
						}

						int nPostBytes = ContentLengthField.f_ToInt();
						NStorage::TCSharedPointer<NContainer::CByteVector> pData = fg_Construct();

						mp_pRequest->f_OnStdInputRaw
							(
								NConcurrency::g_ActorFunctor(NConcurrency::fg_ConcurrentActor())
							 	/ [nPostBytes, pData, _pThis](NContainer::CByteVector &&_Data, bool _bEOF) -> NConcurrency::TCFuture<void>
								{
									pData->f_Insert(_Data);

									if (_bEOF)
									{
										if (nPostBytes != pData->f_GetLen())
										{
											fp_ReportRequestError(_pThis->mp_pRequest, 500, NStr::fg_Format("Invalid CONTENT_LENGTH"));
										}
										else
										{
											_pThis->fp_ParseVariables((ch8 const*)pData->f_GetArray(), pData->f_GetLen());
											_pThis->f_HandleRequest();
										}
									}
									return fg_Explicit();
								}
							)
						;
						return;
					}

					_pThis->f_HandleRequest();
				}

				void f_HandleRequest()
				{
					bool bHandled = false;
					try
					{
						for (auto iHandler = mp_Internal.mp_Handlers.f_GetIterator(); !bHandled && iHandler; ++iHandler)
						{
							for (auto iInnerHandler = iHandler->f_GetIterator(); !bHandled && iInnerHandler; ++iInnerHandler)
							{
								if (mp_Request.m_RequestedURI.f_FindNoCase(iInnerHandler->m_Path) == 0)
									bHandled = iInnerHandler->m_pHandler->f_HandleRequest(*this, mp_Request);
							}
						}
					}
					catch (NException::CException const &_Exception)
					{
						fp_ReportRequestError(mp_pRequest, 500, NStr::fg_Format("Internal error: {}\n", _Exception.f_GetErrorStr()));
						return;
					}

					if (bHandled)
						f_Send();
					else
						fp_ReportRequestError(mp_pRequest, 404, NStr::fg_Format("URI not found: {}\n", mp_Request.m_RequestedURI));
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

			void f_AddHandlerForPath(NStr::CStr const& _Path, CHTTPRequestHandler* _pHandler, int _Priority)
			{
				CHandlerEntry &NewEntry = mp_Handlers[_Priority].f_Insert();
				NewEntry.m_Path = _Path;
				NewEntry.m_pHandler = _pHandler;
			}

			bool f_Run(CHTTPServerOptions const& _Options)
			{
				if (f_IsRunning())
					return false;

				mp_Options = _Options;

				mp_pNGINXLauncher = fg_Construct(mp_Options.m_NGINXPath, mp_Options.m_WebRoot);
				mp_pNGINXLauncher->f_SetFastCGIListen(mp_Options.m_FastCGIListenStartPort, mp_Options.m_nMaxThreads);
				mp_pNGINXLauncher->f_SetListen(mp_Options.m_ListeningPort);
				mp_pNGINXLauncher->f_SetStaticRoot(mp_Options.m_StaticRoot);
				mp_pNGINXLauncher->f_Launch();

				mp_pFastCGIServer = fg_Construct();
				mp_pFastCGIServer
					(
					 	&CFastCGIServer::f_Start
						, NConcurrency::g_ActorFunctor(NConcurrency::fg_ConcurrentActor())
					 	/ [this](NStorage::TCSharedPointer<CFastCGIRequest> const &_pRequest) -> NConcurrency::TCFuture<void>
						{
							auto& Params = _pRequest->f_GetParams();
							auto* pURI = Params.f_FindEqual("DOCUMENT_URI");

							if (!pURI)
							{
								fp_ReportRequestError(_pRequest, 500, "No URI specified");
								return fg_Explicit();
							}

							NStr::CStr URI = *pURI;
	#if DMibEnableDTrace > 0
							NTime::CClock Clock;
							Clock.f_Start();
	#endif

							// Check registered handlers
							NStorage::TCSharedPointer<CConnection> pConnection = fg_Construct(_pRequest, *this);

							pConnection->f_HandleRequest(pConnection, Params);

							DMibDTrace("HTTPServer: URI Served ({fe3} s): {}\n", Clock.f_GetTime() << URI);
							//DMibTrace("Handled in {} s\n", Clock.f_GetTime());
							return fg_Explicit();
						}
					 	, mp_Options.m_FastCGIListenStartPort
					 	, mp_Options.m_nMaxThreads
					 	, NNetwork::CNetAddressTCPv4(NNetwork::CNetAddressIPv4(127, 0, 0, 1), 0)
					)
					> NConcurrency::fg_DiscardResult()
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

		};



		// CHTTPServer Public Methods

		CHTTPServer::CHTTPServer()
		{
			mp_pInternal = fg_Construct();
		}

		CHTTPServer::~CHTTPServer()
		{
			mp_pInternal = nullptr;
		}

		void CHTTPServer::f_AddHandlerForPath(NStr::CStr const& _Path, CHTTPRequestHandler* _pHandler, int _Priority)
		{
			mp_pInternal->f_AddHandlerForPath(_Path, _pHandler, _Priority);
		}

		bool CHTTPServer::f_Run(CHTTPServerOptions const& _Options)
		{
			return mp_pInternal->f_Run(_Options);
		}

		bool CHTTPServer::f_IsRunning()
		{
			return mp_pInternal->f_IsRunning();
		}

		bool CHTTPServer::f_Stop()
		{
			return mp_pInternal->f_Stop();
		}

		// CHTTPResponseHeader Public Methods


		CHTTPResponseHeader::CHTTPResponseHeader()
			: m_MimeType("text/html; charset=UTF-8")
			, m_ContentLength(0)
			, m_AllowMethods("GET")
			, m_Status(200)
		{
		}
		CHTTPResponseHeader::CHTTPResponseHeader(CHTTPResponseHeader const& _ToCopy)
			: m_MimeType(_ToCopy.m_MimeType)
			, m_RedirectTo(_ToCopy.m_RedirectTo)
			, m_ContentLength(_ToCopy.m_ContentLength)
			, m_Expires(_ToCopy.m_Expires)
			, m_LastModified(_ToCopy.m_LastModified)
			, m_AllowMethods(_ToCopy.m_AllowMethods)
			, m_Status(_ToCopy.m_Status)
		{
		}
		CHTTPResponseHeader::CHTTPResponseHeader(CHTTPResponseHeader && _ToMove)
			: m_MimeType(fg_Move(_ToMove.m_MimeType))
			, m_RedirectTo(fg_Move(_ToMove.m_RedirectTo))
			, m_ContentLength(fg_Move(_ToMove.m_ContentLength))
			, m_Expires(fg_Move(_ToMove.m_Expires))
			, m_LastModified(fg_Move(_ToMove.m_LastModified))
			, m_AllowMethods(fg_Move(_ToMove.m_AllowMethods))
			, m_Status(_ToMove.m_Status)
		{
		}

		CHTTPResponseHeader& CHTTPResponseHeader::operator=(CHTTPResponseHeader const& _ToCopy)
		{
			m_MimeType = _ToCopy.m_MimeType;
			m_RedirectTo = _ToCopy.m_RedirectTo;
			m_ContentLength = _ToCopy.m_ContentLength;
			m_Expires = _ToCopy.m_Expires;
			m_LastModified = _ToCopy.m_LastModified;
			m_AllowMethods = _ToCopy.m_AllowMethods;
			m_Status = _ToCopy.m_Status;
			return *this;
		}

		CHTTPResponseHeader& CHTTPResponseHeader::operator=(CHTTPResponseHeader && _ToMove)
		{
			m_MimeType = fg_Move(_ToMove.m_MimeType);
			m_RedirectTo = fg_Move(_ToMove.m_RedirectTo);
			m_ContentLength = fg_Move(_ToMove.m_ContentLength);
			m_Expires = fg_Move(_ToMove.m_Expires);
			m_LastModified = fg_Move(_ToMove.m_LastModified);
			m_AllowMethods = fg_Move(_ToMove.m_AllowMethods);
			m_Status = _ToMove.m_Status;
			return *this;
		}

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

			if (m_Expires.f_IsValid())
			{
				if (m_Expires > NowTime)
					Response += NStr::CStr::CFormat("\r\nExpires: {}") << NTime::fg_GetAscTimeStr(m_Expires);
				else
					Response += NStr::CStr::CFormat("\r\nExpires: {}") << NTime::fg_GetAscTimeStr(NowTime);
			}
			else
			{
				Response += "\r\nExpires: 0";
			}

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

		void CHTTPServerOptions::f_ParseSettings(NEncoding::CEJSON const &_Params)
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
				_Connection.f_Write(ResponseHeader);

				NStr::CStr Msg = "Hello from the handler!<BR/>";

				_Connection.f_Write(Msg);

				return true;
			}
		};

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

			int iTagPos, iEndTagPos;
			CBlock NewBlock;
			while ((iTagPos = Template.f_Find(TagStart)) != -1)
			{
				if (iTagPos > 0)
				{
					CBlock &NewBlock = mp_lBlocks.f_Insert();
					NewBlock.m_Type = EBlock_HTML;
					NewBlock.m_Text = Template.f_Extract(0, iTagPos);
					Template = Template.f_Extract(iTagPos + TagStart.f_GetLen());
					iTagPos = 0;
				}

				iEndTagPos = Template.f_Find(TagEnd);

				if (iEndTagPos == -1)
					break;

				CBlock &NewBlock = mp_lBlocks.f_Insert();
				NewBlock.m_Type = EBlock_UserData;
				NewBlock.m_Text = Template.f_Extract(0, iEndTagPos);

				Template = Template.f_Extract(iEndTagPos + TagEnd.f_GetLen());
			}

			if (!Template.f_IsEmpty())
			{
				CBlock &NewBlock = mp_lBlocks.f_Insert();
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

		void CHTTPCachedConnection::f_Write(NStr::CStr _Str)
		{
			mp_ContentStream.f_FeedBytes((void const*)_Str.f_GetStr(), _Str.f_GetLen());
			mp_Present |= EPresent_Content;
		}

		void CHTTPCachedConnection::f_Write(char const* _pStr)
		{
			mp_ContentStream.f_FeedBytes((void const*)_pStr, NStr::fg_StrLen(_pStr));
			mp_Present |= EPresent_Content;
		}

		void CHTTPCachedConnection::f_Write(uint8 const* _pData, mint _nBytes)
		{
			mp_ContentStream.f_FeedBytes((void const*)_pData, _nBytes);
			mp_Present |= EPresent_Content;
		}

		void CHTTPCachedConnection::f_Write(CHTTPResponseHeader const& _Header)
		{
			mp_Header = _Header;
			mp_Present |= EPresent_Header;
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
				_Dest.f_Write(mp_Header);

			if (mp_Present & EPresent_Content)
				_Dest.f_Write((uint8 const*)mp_ContentStream.f_GetBuffer(), mp_ContentStream.f_GetLength());
		}

		void CHTTPCachedConnection::f_Clear()
		{
			mp_Present = EPresent_None;
			mp_ContentStream.f_Clear();
		}
	}
}
