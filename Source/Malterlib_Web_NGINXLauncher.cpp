// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include "Malterlib_Web_NGINXLauncher.h"

namespace NMib::NWeb
{
	CNGINXLauncher::CNGINXLauncher(NStr::CStr const& _LaunchPath, NStr::CStr const& _RootDir)
		: mp_LaunchPath(_LaunchPath)
		, mp_RootDir(_RootDir)
		, mp_nFastCGIListen(1)
		, mp_FastCGIStartListen(9000)
		, mp_Listen(8080)
	{
#ifdef DPlatformFamily_Windows
		if (NFile::CFile::fs_GetExtension(mp_LaunchPath).f_CmpNoCase("exe") != 0)
		{
			mp_LaunchPath = mp_LaunchPath + ".exe";
		}
#endif
	}

	CNGINXLauncher::~CNGINXLauncher()
	{
		if (mp_pProcessLaunch)
		{
			try
			{
				mp_pProcessLaunch->f_StopProcess();
			}
			catch (NException::CException const&)
			{
			}
			mp_pProcessLaunch.f_Clear();
		}
	}

	void CNGINXLauncher::f_SetFastCGIListen(uint16 _StartPort, uint16 _nListen)
	{
		mp_FastCGIStartListen = _StartPort;
		mp_nFastCGIListen = _nListen;
	}

	void CNGINXLauncher::f_SetListen(uint16 _Port)
	{
		mp_Listen = _Port;
	}

	void CNGINXLauncher::f_SetStaticRoot(NStr::CStr const& _Root)
	{
		mp_StaticRoot = _Root;
	}

	void CNGINXLauncher::f_Launch()
	{
		NFile::CFile::fs_CreateDirectory(mp_RootDir);
		NFile::CFile::fs_CreateDirectory(mp_RootDir + "/logs");
		NStr::CStr ConfigFile = NFile::CFile::fs_AppendPath(mp_RootDir, "nginx.conf");
		NStr::CStr PidFile = NFile::CFile::fs_AppendPath(mp_RootDir, "nginx.pid");
		NStr::CStr FastCGIFile = NFile::CFile::fs_AppendPath(mp_RootDir, "fastcgi.conf");

		{
			NStr::CStr Contents;

			Contents += "fastcgi_param  SCRIPT_FILENAME    $document_root$fastcgi_script_name;\n";
			Contents += "fastcgi_param  QUERY_STRING       $query_string;\n";
			Contents += "fastcgi_param  REQUEST_METHOD     $request_method;\n";
			Contents += "fastcgi_param  CONTENT_TYPE       $content_type;\n";
			Contents += "fastcgi_param  CONTENT_LENGTH     $content_length;\n";
			Contents += "fastcgi_param  SCRIPT_NAME        $fastcgi_script_name;\n";
			Contents += "fastcgi_param  REQUEST_URI        $request_uri;\n";
			Contents += "fastcgi_param  DOCUMENT_URI       $document_uri;\n";
			Contents += "fastcgi_param  DOCUMENT_ROOT      $document_root;\n";
			Contents += "fastcgi_param  SERVER_PROTOCOL    $server_protocol;\n";
			Contents += "fastcgi_param  GATEWAY_INTERFACE  CGI/1.1;\n";
			Contents += "fastcgi_param  SERVER_SOFTWARE    nginx/$nginx_version;\n";
			Contents += "fastcgi_param  REMOTE_ADDR        $remote_addr;\n";
			Contents += "fastcgi_param  REMOTE_PORT        $remote_port;\n";
			Contents += "fastcgi_param  SERVER_ADDR        $server_addr;\n";
			Contents += "fastcgi_param  SERVER_PORT        $server_port;\n";
			Contents += "fastcgi_param  SERVER_NAME        $server_name;\n";

//				Contents += "fastcgi_index  index.php;\n";

			Contents += "fastcgi_param  REDIRECT_STATUS    200;\n";

			NFile::CFile::fs_WriteStringToFile(NStr::CStr(FastCGIFile), Contents, false);
		}

		{
			NStr::CStr Contents;

			Contents += NStr::CStr::CFormat("daemon off;\n") << PidFile;
#ifdef DMibDebug
//				Contents += NStr::CStr::CFormat("master_process off;\n") << PidFile;
			Contents += NStr::CStr::CFormat("error_log logs/error.log_ debug;\n") << PidFile;
#endif
			Contents += NStr::CStr::CFormat("pid {};\n") << PidFile;
//#ifdef DPlatformFamily_Windows
//				Contents += NStr::CStr::CFormat("worker_threads {};\n") << mp_nListen;
//#else
			Contents += NStr::CStr::CFormat("worker_processes {};\n") << mp_nFastCGIListen;
//#endif

			Contents += "events {\n";
			Contents += "	worker_connections  4096;\n";
			Contents += "}\n";

			Contents += "http {\n";
			if (mp_nFastCGIListen)
			{
				Contents += "	upstream fastcgipool {\n";
				for (uint16 i = mp_FastCGIStartListen; i < mp_FastCGIStartListen + mp_nFastCGIListen; ++i)
					Contents += NStr::CStr::CFormat("		server 127.0.0.1:{};\n") << i;
				Contents += NStr::CStr::CFormat("		keepalive {};\n") << mp_nFastCGIListen;
				Contents += "	}\n";
			}
			Contents += "	server {\n";
			Contents += NStr::CStr::CFormat("		listen {};\n") << mp_Listen;
			if (mp_nFastCGIListen)
			{
				Contents += NStr::CStr::CFormat("		location / {{\n") << mp_RootDir;

				Contents += NStr::CStr::CFormat("			include {};\n") << FastCGIFile;
				Contents += "			fastcgi_pass fastcgipool;\n";
				Contents += "			fastcgi_keep_conn on;\n";

				//Contents += NStr::CStr::CFormat("	root  {};\n") << mp_RootDir;
				Contents += NStr::CStr::CFormat("			gzip off;\n") << mp_RootDir;
				Contents += "		}\n";
			}
			if (!mp_StaticRoot.f_IsEmpty())
			{
				Contents += "		location /Static/ {\n";
				Contents += NStr::CStr::CFormat("			root {};\n") << mp_StaticRoot;
				Contents += "		}\n";
			}

			Contents += "	}\n";
			Contents += "}\n";

			NFile::CFile::fs_WriteStringToFile(NStr::CStr(ConfigFile), Contents, false);
		}

		NContainer::TCVector<NStr::CStr> Params;
		Params.f_Insert("-c");
		Params.f_Insert(ConfigFile);
		Params.f_Insert("-p");
		Params.f_Insert(mp_RootDir);

		auto LaunchParams = NProcess::CProcessLaunchParams::fs_LaunchExecutable
			(
				mp_LaunchPath
				, Params
				, mp_RootDir
				, [](NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						{
							DMibLogWithCategory(Malterlib/Web/NGINXLauncher, Info, "nginx launched: {}", _State.f_Get<NProcess::EProcessLaunchState_Launched>());
						}
						break;
					case NProcess::EProcessLaunchState_LaunchFailed:
						{
							DMibLogWithCategory(Malterlib/Web/NGINXLauncher, Error, "nginx launch failed: {}", _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>());
						}
						break;
					case NProcess::EProcessLaunchState_Exited:
						{
							if (_State.f_Get<NProcess::EProcessLaunchState_Exited>())
								DMibLogWithCategory(Malterlib/Web/NGINXLauncher, Error, "nginx exited: {}", _State.f_Get<NProcess::EProcessLaunchState_Exited>());
							else
								DMibLogWithCategory(Malterlib/Web/NGINXLauncher, Info, "nginx exited: {}", _State.f_Get<NProcess::EProcessLaunchState_Exited>());
						}
						break;
					}
				}
			)
		;

		LaunchParams.m_fOnOutput
			= [](NProcess::EProcessLaunchOutputType _OutputType, NStr::CStr const &_Output)
			{
				DMibLogWithCategory(Malterlib/Web/nginx, Info, "{}", _Output.f_TrimRight());
			}
		;

		LaunchParams.m_bShowLaunched = false;
		LaunchParams.m_bCreateNewProcessGroup = true;

		mp_pProcessLaunch = fg_Construct(LaunchParams, NProcess::EProcessLaunchCloseFlag_BlockOnExit);
	}
}
