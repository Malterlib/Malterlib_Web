// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Encoding/EJSON>
#include <Mib/Container/Registry>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>

namespace NMib::NWeb
{
	struct CHTTPRequest
	{
		NStr::CStr m_RequestedURI;
		NStr::CStr m_Method;
		NStr::CStr m_ClientIP;
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_Variables;

		NStr::CStr f_GetVariable(NStr::CStr const &_Variable, NStr::CStr const &_DefaultValue);

		static NStr::CStr fs_LookupHost(NStr::CStr const& _IP);
	};

	struct CHTTPResponseHeader
	{
		NStr::CStr m_MimeType;
		NStr::CStr m_RedirectTo;
		int m_ContentLength;
		uint32 m_Status;
		NTime::CTime m_Expires;
		NTime::CTime m_LastModified;
		NStr::CStr m_AllowMethods;		// List of one or more of GET, POST, HEAD, PUT separated by a comma and a space: e.g. "GET, HEAD"

		CHTTPResponseHeader();
		CHTTPResponseHeader(CHTTPResponseHeader const& _ToCopy);
		CHTTPResponseHeader(CHTTPResponseHeader && _ToMove);
		CHTTPResponseHeader& operator=(CHTTPResponseHeader const& _ToCopy);
		CHTTPResponseHeader& operator=(CHTTPResponseHeader && _ToMove);

		NStr::CStr f_Generate() const;
		void f_SetPlainMime();
		void f_SetMimeTypeFromFilename(NStr::CStr const& _Filename);
	};

	class CHTTPConnection : public NStorage::TCSharedPointerIntrusiveBase<>
	{
	public:
		virtual ~CHTTPConnection() {}

		virtual void f_Write(NStr::CStr _Str) = 0;
		virtual void f_Write(char const* _pStr) = 0;
		virtual void f_Write(const uint8* _pData, mint _nBytes) = 0;

		virtual void f_Write(CHTTPResponseHeader const& _Header) = 0;
	};

	class CHTTPCachedConnection : public CHTTPConnection
	{
	private:
		enum EPresent
		{
			EPresent_None		= 0,
			EPresent_Header		= DMibBit(0),
			EPresent_Content	= DMibBit(1),
		};

		EPresent mp_Present;

		NTime::CTime mp_Timestamp;
		CHTTPResponseHeader mp_Header;
		NStream::CBinaryStreamMemory<NMib::NStream::CBinaryStreamDefault> mp_ContentStream;

	public:
		CHTTPCachedConnection();
		CHTTPCachedConnection(CHTTPCachedConnection const& _ToCopy);
		CHTTPCachedConnection(CHTTPCachedConnection && _ToMove);

		~CHTTPCachedConnection() override;

		CHTTPCachedConnection& operator= (CHTTPCachedConnection const& _ToCopy);
		CHTTPCachedConnection& operator= (CHTTPCachedConnection && _ToMove);

		void f_Write(NStr::CStr _Str) override;
		void f_Write(char const* _pStr) override;
		void f_Write(uint8 const* _pData, mint _nBytes) override;
		void f_Write(CHTTPResponseHeader const& _Header) override;

		bool f_HasHeader();
		bool f_HasContent();

		void f_SetTimestamp();
		NTime::CTime f_GetTimestamp();

		bool f_IsValid(fp32 _MaxSecondsOld) ;
		void f_Forward(CHTTPConnection& _Dest);
		void f_Clear();
	};

	class CHTTPRequestHandler
	{
	public:
		virtual ~CHTTPRequestHandler() {}

		// This could be called for any thread - assume nothing.
		virtual bool f_HandleRequest(CHTTPConnection &_Connection, CHTTPRequest const& _Req) = 0;
	};

	using FActorRequestHandler = NConcurrency::TCActorFunctor
		<
			NConcurrency::TCFuture<bool> (NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection, NStorage::TCSharedPointer<CHTTPRequest> const &_pRequest)
		>
	;

	struct CHTTPServerOptions
	{
		uint32 m_nMaxThreads = 1;
		uint16 m_ListeningPort = 8080;
		uint16 m_FastCGIListenStartPort = 9000;
		NStr::CStr m_StaticRoot;
		NStr::CStr m_NGINXPath;
		NStr::CStr m_WebRoot;

		CHTTPServerOptions();
		/*
		Options parsed (as if they are from the tool command line:
			-nginx_path <path>
				Path to nginx executable. ($ProgramDirectory/nginx/nginx)
			-http_webroot <filename>
				Sets the name of the root for the web server. This location will contain log files, configuration etc. ($ProgramDirectory/webroot)
			-http_root <dir>
				Sets the local root for serving static files (No Default)
			-http_port <port>
				Sets port to listen on. (Default 8080)
			-fastcgi_listen <port>
				Sets port for fast to listen on. This is the start listening port. http_threads listen ports after the specified portt will be used. (Default 9000)
			-http_threads <num>
				Sets the number of worker threads to use for the web server. (Default 1)
		*/
		void f_ParseCmdLine(NContainer::CRegistry &_Params);
		// Uses the same options (without the -) and uses key-value structure. .hrf style.
		void f_ParseSettings(NContainer::CRegistry const& _Params);
		void f_ParseSettings(NContainer::CRegistryPreserveWhitespace const& _Params);
		void f_ParseSettings(NEncoding::CEJSON const &_Params);

	private:
		template <typename tf_CRegistry>
		void fp_ParseSettings(tf_CRegistry const& _Params);
	};

	class CHTTPServer
	{
		struct CHTTPServerInternal;
		NConcurrency::TCActor<CHTTPServerInternal> mp_Internal;

	public:
		CHTTPServer();
		~CHTTPServer();

		/*
			Handlers will be called to handle any request starting with _Path.
			The HTTPServer does not take ownership of the handler.
		*/
		void f_AddHandlerForPath(NStr::CStr const& _Path, CHTTPRequestHandler* _pHandler, int _Priority);
		void f_AddHandlerActorForPath(NStr::CStr const &_Path, FActorRequestHandler &&_fHandleRequest, int _Priority);
		bool f_Run(CHTTPServerOptions const& _Options);
		bool f_IsRunning();
		bool f_Stop();
	};

	/*
	CHTMLTemplate - Poor man's PHP or something.

		1. Create a HTML file for your template.
		2. Wherever your C++ code might want to insert text add a HTML comment of the form:
			<!--USERDATA(<BLOCKNAME>)-->
		3. Create a CHTMLTemplate for the template file.
		4. In your HTTPRequestHandler call
				f_WriteTemplate(_Connection, [](CHTTPConnection& _Conn, NStr::CStr const& _BlockName)
				{
					// Depending on _BlockName write different things to the connection.
				});

		NOTE: f_WriteTemplate will send a HTTP response header for you.
	*/
	class CHTMLTemplate
	{
	public:
		enum EBlock
		{
			EBlock_HTML,
			EBlock_UserData,
		};

		struct CBlock
		{
			EBlock m_Type;
			NStr::CStr m_Text;		// HTML or user data name
		};

		typedef NContainer::TCVector<CBlock>::CIteratorConst CIteratorConst;

	public:
		CHTMLTemplate(NStr::CStr const& _Filename, bool _bExeFs);
		~CHTMLTemplate();

		bool f_IsEmpty() const { return mp_pState->m_Blocks.f_IsEmpty(); }
		CIteratorConst f_GetIterator() const { return fg_Const(mp_pState->m_Blocks).f_GetIterator(); }

		// Lambda Sig: CStr Func(CHTTPConnection & _Conn, CStr const& _BlockName);
		template <typename t_FFunc>
		void f_WriteTemplate(CHTTPConnection & _Conn, t_FFunc const& _Func)
		{
			CHTTPResponseHeader ResponseHeader;
			f_WriteTemplate(_Conn, ResponseHeader, _Func);
		}

		// Lambda Sig: CStr Func(CHTTPConnection & _Conn, CStr const& _BlockName);
		template <typename t_FFunc>
		void f_WriteTemplate(CHTTPConnection & _Conn, CHTTPResponseHeader const& _BaseHeader, t_FFunc const& _Func)
		{
			_Conn.f_Write(_BaseHeader);

			for (auto &Block : mp_pState->m_Blocks)
			{
				if (Block.m_Type == EBlock_HTML)
					_Conn.f_Write(Block.m_Text);
				else if (Block.m_Type == EBlock_UserData)
					_Func(_Conn, Block.m_Text);
			}
		}

		NConcurrency::TCFuture<void> f_WriteTemplateAsync
			(
				 NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection
				 , CHTTPResponseHeader const &_BaseHeader
				 , NFunction::TCFunction<NConcurrency::TCFuture<void> (NStr::CStr const &_BlockName)> &&_fWriteBlock
			)
		;

	private:
		struct CState
		{
			NContainer::TCVector<CBlock> m_Blocks;
		};

		struct CAsyncWriteState
		{
			NStorage::TCSharedPointer<CHTTPConnection> m_pConnection;
			NStorage::TCSharedPointer<CState> m_pState;
			NConcurrency::TCPromise<void> m_Promise;
			NFunction::TCFunction<NConcurrency::TCFuture<void> (NStr::CStr const& _BlockName)> m_fWriteBlock;
			mint m_iBlock = 0;
		};

		static void fsp_AsyncWrite(NStorage::TCSharedPointer<CAsyncWriteState> const &_pState);

		NStorage::TCSharedPointer<CState> mp_pState = fg_Construct();
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif

