// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/HTTP/All>

using namespace NMib;
using namespace NMib::NWeb;


class CHTTPApp : public NMib::CApplication
{
public:
	aint f_Main()
	{
		NHTTP::CServer::CConfig Config;
		Config.m_Port = 8082;

		NHTTP::CServer Server(Config);

		NStr::CStr Errors;

		if (!Server.f_Start(Errors))
		{
			DConOut("Failed to start HTTP server: {}\n", Errors);
			return -1;
		}

		while (1)
		{
			NSys::fg_Thread_Sleep(100.0);
		}

		Server.f_Stop();

		return 0;
	}
};

DMibAppImplement(CHTTPApp);
