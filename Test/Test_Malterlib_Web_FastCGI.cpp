// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Web/FastCGIServer>
#include <Mib/Web/HTTPServer>

using namespace NMib;
using namespace NMib::NWeb;
using namespace NMib::NNetwork;
using namespace NMib::NTest;
using namespace NMib::NThread;
using namespace NMib::NContainer;
using namespace NMib::NStr;
using namespace NMib::NCryptography;
using namespace NMib::NFile;

class CFastCGI_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestCategory("General")
		{
			CProcessLaunch::fs_KillProcessesInDirectory("*", {}, ProgramDirectory / "MalterlibWeb", 10.0);

			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CHTTPServer Server;
			CHTTPServerOptions Options;
			Options.m_ListeningPort = 9050;
			Options.m_FastCGIListenStartPort = 10050;
			Options.m_NGINXPath = ProgramDirectory / "MalterlibWeb/bin/nginx";
			Options.m_WebRoot = ProgramDirectory / "MalterlibWeb/webroot";
			Options.m_StaticRoot = ProgramDirectory / "MalterlibWeb/staticroot";

			TCActor<CSeparateThreadActor> HelperActor{fg_Construct(), "Test actor"};
			auto CleanupTestActor = g_OnScopeExit > [&]
				{
					HelperActor->f_BlockDestroy();
				}
			;
			CCurrentlyProcessingActorScope CurrentActor{HelperActor};

			Server.f_AddHandlerActorForPath
				(
					"/Bounce"
					, g_ActorFunctor / [](NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection, NStorage::TCSharedPointer<CHTTPRequest> const &_pRequest)
					{
						_pRequest->f_
					}
					, 0
				)
			;

			Server.f_Run(Options);

			auto Cleanup = g_OnScopeExit > [&]
				{
					Server.f_Stop();
				}
			;

		};
	}
};

DMibTestRegister(CFastCGI_Tests, Malterlib::Web);
