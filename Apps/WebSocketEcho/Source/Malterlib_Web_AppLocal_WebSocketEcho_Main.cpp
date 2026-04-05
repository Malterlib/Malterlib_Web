// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>

#include "Malterlib_Web_App_WebSocketEcho.h"

using namespace NMib;
using namespace NMib::NWeb::NWebSocketEcho;

class CWebSocketEcho : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibWebSocketEcho"
				, "Malterlib Web Socket Echo"
				, "Web socket echo server"
				, []
				{
					return fg_ConstructActor<CWebSocketEchoActor>();
				}
			}
		;

		return Daemon.f_Run();
	}
};

DAppImplement(CWebSocketEcho);
