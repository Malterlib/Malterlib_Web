// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Test/Exception>
#include <Mib/Web/HttpClient>
#include <Mib/Web/HTTP/URL>
#include <Mib/File/ExeFS>
#include <Mib/File/VirtualFSs/MalterlibFS>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Cryptography/PublicCrypto>
#include <Mib/Cryptography/Certificate>
#include <Mib/Cryptography/UUID>
#include <Mib/Time/Time>

using namespace NMib::NWeb;
using namespace NMib::NNetwork;
using namespace NMib;
using namespace NMib::NTest;
using namespace NMib::NThread;
using namespace NMib::NContainer;
using namespace NMib::NStr;
using namespace NMib::NCryptography;
using namespace NMib::NFunction;
using namespace NMib::NProcess;
using namespace NMib::NFile;
using namespace NMib::NConcurrency;
using namespace NMib::NTime;

namespace
{
	fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;
	CUniversallyUniqueIdentifier g_SocketPathRootUUID("20D5CFF1-4CD7-4F1A-8B53-3C0252ED5817", EUniversallyUniqueIdentifierFormat_Bare);
}

class CHttpClient_Tests : public NMib::NTest::CTest
{
public:
	struct CWebServerResults
	{
		CWebServerResults() = default;
		CWebServerResults(CWebServerResults &&) = default;
		~CWebServerResults()
		{
			if (m_Subscription)
				m_Subscription->f_Destroy().f_DiscardResult();

			if (m_WebServerLaunch)
				fg_Move(m_WebServerLaunch).f_Destroy().f_DiscardResult();
		}

		TCActor<CProcessLaunchActor> m_WebServerLaunch;
		CHttpClientActor::CCertificateConfig m_CertificateConfig;
		CActorSubscription m_Subscription;
	};

	NStr::CStr f_GetLocalSocketFileName(NStr::CStr const &_Path) const
	{
		mint MaxLength = NSys::NNetwork::fg_GetMaxUnixSocketNameLength();
		if (_Path.f_GetLen() <= aint(MaxLength))
			return _Path;

		CStr ConfigHash = fg_GetHashedUuidString(_Path, g_SocketPathRootUUID, EUniversallyUniqueIdentifierFormat_AlphaNum);
		CStr TempDir = CFile::fs_GetRawTemporaryDirectory();
		CStr SocketPath = TempDir / (ConfigHash + ".socket");
		if (SocketPath.f_GetLen() <= aint(MaxLength))
			return SocketPath;

		SocketPath = fg_Format("/tmp/{}.socket", ConfigHash);
		DMibCheck(SocketPath.f_GetLen() <= aint(MaxLength))(SocketPath.f_GetLen())(MaxLength);
		return SocketPath;
	}

	TCFuture<CWebServerResults> f_SetupWebServer(CStr _DestinationDirectory)
	{
		CProcessLaunch::fs_KillProcesses
			(
				[_DestinationDirectory](CProcessInfo const &_ProcessInfo) -> bool
				{
					if (CFile::fs_GetFileNoExt(_ProcessInfo.m_FileName) == "node" && _ProcessInfo.m_Args.f_Contains(_DestinationDirectory / "index.ts") >= 0)
						return true;

					return false;
				}
				, EProcessInfoFlag_FileName | EProcessInfoFlag_Args
			)
		;

		CStr HttpSocket = f_GetLocalSocketFileName(_DestinationDirectory / "http.sock");
		CStr HttpsSocket = f_GetLocalSocketFileName(_DestinationDirectory / "https.sock");

		CWebServerResults WebServerResults;

		CExeFS SourceFS;
		if (!fg_OpenExeFS(SourceFS))
			DMibError("Could not open ExeFS");

		if (CFile::fs_FileExists(_DestinationDirectory))
			CFile::fs_DeleteDirectoryRecursive(_DestinationDirectory);

		for (auto &File : {HttpSocket, HttpsSocket})
		{
			if (CFile::fs_FileExists(File))
				CFile::fs_DeleteDirectoryRecursive(File);
		}

		CFile::fs_CreateDirectory(_DestinationDirectory);

		CFileSystemInterface_VirtualFS SourceVirtualFS(SourceFS.m_FileSystem);
		CFileSystemInterface_Disk DestinationFS;

		SourceVirtualFS.f_CopyFiles("NodeWebServer/*", DestinationFS, _DestinationDirectory);
#ifdef DPlatformFamily_macOS
		auto CurrentPath = fg_GetSys()->f_GetEnvironmentVariable("PATH", "").f_Split(":");
		for (auto ExpectedPath : {"/opt/local/bin", "/opt/homebrew/bin"})
		{
			if (CurrentPath.f_Contains(ExpectedPath) < 0)
				CurrentPath.f_InsertFirst(ExpectedPath);
		}
		fg_GetSys()->f_SetEnvironmentVariable("PATH", CStr::fs_Join(CurrentPath, ":"));
#endif

		co_await CProcessLaunchActor::fs_LaunchSimple({"npm", {"ci"}, _DestinationDirectory, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode});

		CStr CertificateFile = _DestinationDirectory + "/web.pem";
		CStr CertificateKeyFile = _DestinationDirectory + "/web.key";

		CStr CaCertificateFile = _DestinationDirectory + "/web_ca.pem";
		CStr CaCertificateKeyFile = _DestinationDirectory + "/web_ca.key";

		{
			CPublicKeySetting KeySettings = CPublicKeySettings_EC_secp384r1{};

			TCMap<CStr, CStr> RelativeDistinguishedNames;
			RelativeDistinguishedNames["O"] = "malterlib.org";

			CByteVector CertData;
			CByteVector CertRequestData;
			CSecureByteVector KeyData;
			CByteVector CaCertData;
			CSecureByteVector CaKeyData;

			{
				CCertificateOptions Options;
				Options.m_CommonName = fg_Format("Malterlib Web CA {nfh,sj16,sf0}", NMisc::fg_GetHighEntropyRandomInteger<uint64>());
				Options.m_RelativeDistinguishedNames = RelativeDistinguishedNames;
				Options.m_KeySetting = KeySettings;
				Options.f_MakeCA();

				CCertificateSignOptions SignOptions;
				SignOptions.m_Days = 365*20;
				SignOptions.f_AddExtension_SubjectKeyIdentifier();

				CCertificate::fs_GenerateSelfSignedCertAndKey
					(
						Options
						, CaCertData
						, CaKeyData
						, SignOptions
					)
				;
				CFile::fs_WriteFile(CaCertData, CaCertificateFile);
				CFile::fs_WriteFileSecure(CaKeyData, CaCertificateKeyFile);
			}

			CCertificateOptions Options;
			Options.m_CommonName = "localhost";
			Options.m_RelativeDistinguishedNames = RelativeDistinguishedNames;
			Options.m_Hostnames = {"localhost"};
			Options.m_KeySetting = KeySettings;

			CCertificateSignOptions SignOptions;
			SignOptions.m_Serial = 1;
			SignOptions.m_Days = 824;
			SignOptions.f_AddExtension_AuthorityKeyIdentifier();
			Options.f_AddExtension_BasicConstraints(false);
			Options.f_AddExtension_KeyUsage(EKeyUsage_KeyEncipherment | EKeyUsage_DigitalSignature);

			CCertificate::fs_GenerateClientCertificateRequest(Options, CertRequestData, KeyData);
			CCertificate::fs_SignClientCertificate(CaCertData, CaKeyData, CertRequestData, CertData, SignOptions);

			CFile::fs_WriteFile(CertData, CertificateFile);
			CFile::fs_WriteFileSecure(KeyData, CertificateKeyFile);

			WebServerResults.m_CertificateConfig.m_ClientKey = KeyData;
			WebServerResults.m_CertificateConfig.m_ClientCertificate = CertData;
			WebServerResults.m_CertificateConfig.m_CertificateAuthorities = CaCertData;
		}

		WebServerResults.m_WebServerLaunch = fg_Construct();
		CProcessLaunchActor::CLaunch WebServerLaunch
			(
				CProcessLaunchParams::fs_LaunchExecutable
				(
					"npx"
					, {"ts-node", _DestinationDirectory / "index.ts"}
					, _DestinationDirectory
					, nullptr
				)
			)
		;

		WebServerLaunch.m_Params.m_bAllowExecutableLocate = true;
		WebServerLaunch.m_Params.m_bCreateNewProcessGroup = true;
		WebServerLaunch.m_Params.m_Environment["MalterlibWebTestWebKey"] = CertificateKeyFile;
		WebServerLaunch.m_Params.m_Environment["MalterlibWebTestWebCert"] = CertificateFile;
		WebServerLaunch.m_Params.m_Environment["MalterlibWebTestWebHttpSocket"] = HttpSocket;
		WebServerLaunch.m_Params.m_Environment["MalterlibWebTestWebHttpsSocket"] = HttpsSocket;

		TCPromise<void> LaunchedPromise;
		TCPromise<void> FinishedStartupPromise;
		WebServerLaunch.m_Params.m_fOnStateChange = [LaunchedPromise, FinishedStartupPromise](NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
			{
				switch (_State.f_GetTypeID())
				{
				case NProcess::EProcessLaunchState_LaunchFailed:
					{
						if (!LaunchedPromise.f_IsSet())
							LaunchedPromise.f_SetException(DMibErrorInstance(NStr::fg_Format("Launch failed: {}", _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>())));
						break;
					}
				case NProcess::EProcessLaunchState_Exited:
					{
						auto ExitCode = _State.f_Get<NProcess::EProcessLaunchState_Exited>();
						if (!LaunchedPromise.f_IsSet())
							LaunchedPromise.f_SetException(DMibErrorInstance(NStr::fg_Format("Launch exited unexpectedly: {}", ExitCode)));

						if (!FinishedStartupPromise.f_IsSet())
							FinishedStartupPromise.f_SetException(DMibErrorInstance(NStr::fg_Format("Launch exited unexpectedly: {}", ExitCode)));
						break;
					}
				case NProcess::EProcessLaunchState_Launched:
					if (!LaunchedPromise.f_IsSet())
						LaunchedPromise.f_SetResult();
					break;
				}
			}
		;

		struct CState
		{
			bool m_bHttpDone = false;
			bool m_bHttpsDone = false;
		};

		NStorage::TCSharedPointer<CState> pState = fg_Construct();

		WebServerLaunch.m_Params.m_fOnOutput = [FinishedStartupPromise, pState](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
			{
				if (_OutputType != EProcessLaunchOutputType_StdOut)
				{
					if (!_Output.f_StartsWith("Process terminated due to signal"))
						DMibConErrOut2("{}", _Output);
					return;
				}

				for (auto &Line : _Output.f_SplitLine())
				{
					if (Line.f_StartsWith("ERROR: "))
					{
						if (!FinishedStartupPromise.f_IsSet())
							FinishedStartupPromise.f_SetException(DMibErrorInstance(Line));
					}

					if (Line.f_StartsWith("http listen: "))
						pState->m_bHttpDone = true;
					if (Line.f_StartsWith("https listen: "))
						pState->m_bHttpsDone = true;
				}

				if (pState->m_bHttpDone && pState->m_bHttpsDone && !FinishedStartupPromise.f_IsSet())
					FinishedStartupPromise.f_SetResult();
			}
		;

		auto LaunchSubscription = co_await WebServerResults.m_WebServerLaunch(&CProcessLaunchActor::f_Launch, WebServerLaunch, fg_CurrentActor());

		co_await LaunchedPromise.f_MoveFuture().f_Timeout(g_Timeout, "Timeouted out waiting for launch");

		co_await FinishedStartupPromise.f_MoveFuture().f_Timeout(g_Timeout, "Timeout out waiting for startup");

		WebServerResults.m_Subscription = fg_Move(LaunchSubscription);

		co_return fg_Move(WebServerResults);
	}

	void f_DoTests()
	{
#ifdef DPlatformFamily_Windows
		return; // Node.js doesn't support unix domain sockets yet
#endif
		DMibTestSuite("General") -> TCFuture<void>
		{
			DMibTestPath("Path1");
			CStr TestDirectory = CFile::fs_GetProgramDirectory() / "TestWebHttpClientGeneral";
			fg_TestAddCleanupPath(TestDirectory);

			auto WebServerResults = co_await f_SetupWebServer(TestDirectory);

			NHTTP::CURL HttpUrlTemplate = CStr("http://[UNIX:{}]/"_f << f_GetLocalSocketFileName(TestDirectory / "http.sock"));
			NHTTP::CURL HttpsUrlTemplate = CStr("https://[UNIX:{}]/"_f << f_GetLocalSocketFileName(TestDirectory / "https.sock"));
			TCMap<CStr, CStr> Headers;
			TCMap<CStr, CStr> Cookies;
			CByteVector Data;

			{
				NHTTP::CURL HttpUrl = HttpUrlTemplate;
				NHTTP::CURL HttpsUrl = HttpsUrlTemplate;
				DMibTestPath("Simple Request");
				TCActor<CHttpClientActor> HttpClientActor(fg_Construct(WebServerResults.m_CertificateConfig), "HTTP Client");
				{
					DMibTestPath("HTTP");
					auto Result = co_await HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpUrl.f_Encode(), Headers, Data, Cookies);
					DMibExpect(Result.m_Body, ==, "Root Reply");
				}
				{
					DMibTestPath("HTTPS");
					auto Result = co_await HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpsUrl.f_Encode(), Headers, Data, Cookies);
					DMibExpect(Result.m_Body, ==, "Root Reply");
				}

				co_await fg_Move(HttpClientActor).f_Destroy();
			}
			{
				NHTTP::CURL HttpUrl = HttpUrlTemplate;
				NHTTP::CURL HttpsUrl = HttpsUrlTemplate;
				DMibTestPath("Multiple Requests");
				TCActor<CHttpClientActor> HttpClientActor(fg_Construct(WebServerResults.m_CertificateConfig), "HTTP Client");

				CStr ExpectedResultsText;

				TCFutureVector<CHttpClientActor::CResult> AsyncResults;
				for (mint i = 0; i < 100; ++i)
				{
					HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpsUrl.f_Encode(), Headers, Data, Cookies) > AsyncResults;
					fg_AddStrSep(ExpectedResultsText, "Root Reply", "\n");
				}

				CStr ExceptionText;
				CStr ResultsText;

				auto Results = co_await fg_AllDoneWrapped(AsyncResults);
				for (auto &Result : Results)
				{
					if (Result)
						fg_AddStrSep(ResultsText, Result->m_Body, "\n");
					else
						fg_AddStrSep(ExceptionText, Result.f_GetExceptionStr(), "\n");
				}

				co_await fg_Move(HttpClientActor).f_Destroy();

				DMibExpect(ExceptionText, ==, "");
				DMibExpect(ResultsText, ==, ExpectedResultsText);
			}
			{
				NHTTP::CURL HttpUrl = HttpUrlTemplate;
				NHTTP::CURL HttpsUrl = HttpsUrlTemplate;
				DMibTestPath("Multiple Actors");
				TCVector<TCActor<CHttpClientActor>> HttpClientActors;

				CStr ExpectedResultsText;

				TCFutureVector<CHttpClientActor::CResult> AsyncResults;
				for (mint i = 0; i < 100; ++i)
				{
					TCActor<CHttpClientActor> HttpClientActor(fg_Construct(WebServerResults.m_CertificateConfig), "HTTP Client");
					HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpsUrl.f_Encode(), Headers, Data, Cookies) > AsyncResults;
					fg_AddStrSep(ExpectedResultsText, "Root Reply", "\n");
					HttpClientActors.f_Insert(fg_Move(HttpClientActor));
				}

				CStr ExceptionText;
				CStr ResultsText;

				auto Results = co_await fg_AllDoneWrapped(AsyncResults);
				for (auto &Result : Results)
				{
					if (Result)
						fg_AddStrSep(ResultsText, Result->m_Body, "\n");
					else
						fg_AddStrSep(ExceptionText, Result.f_GetExceptionStr(), "\n");
				}

				for (auto &Actor : HttpClientActors)
					co_await fg_Move(Actor).f_Destroy();

				DMibExpect(ExceptionText, ==, "");
				DMibExpect(ResultsText, ==, ExpectedResultsText);
			}
			{
				NHTTP::CURL HttpsUrl = HttpsUrlTemplate;
				DMibTestPath("Untrusted Certificate");
				TCActor<CHttpClientActor> HttpClientActor(fg_Construct(), "HTTP Client");
				{
					DMibTestPath("HTTPS");
					auto HttpClientResult = co_await HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpsUrl.f_Encode(), Headers, Data, Cookies).f_Wrap();
					DMibExpectException
						(
							HttpClientResult.f_Access()
							, DMibErrorInstance
							(
								"libcurl failed (60): SSL peer certificate or SSH remote key was not OK. SSL certificate OpenSSL verify result: unable to get local issuer certificate (20)"
							)
						)
					;
				}
				co_await fg_Move(HttpClientActor).f_Destroy();
			}
			{
				NHTTP::CURL HttpsUrl("https://www.google.com/");
				DMibTestPath("Public Certificate");
				TCActor<CHttpClientActor> HttpClientActor(fg_Construct(), "HTTP Client");
				{
					DMibTestPath("HTTPS");
					auto HttpClientResult = co_await HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpsUrl.f_Encode(), Headers, Data, Cookies).f_Wrap();
					DMibExpectNoException(HttpClientResult.f_Access());
				}
				co_await fg_Move(HttpClientActor).f_Destroy();
			}
			{
				DMibTestPath("Abort Request");
				NHTTP::CURL HttpUrl = HttpUrlTemplate;
				NHTTP::CURL HttpsUrl = HttpsUrlTemplate;
				HttpUrl.f_SetPath({"slow-request"});
				HttpsUrl.f_SetPath({"slow-request"});

				{
					DMibTestPath("HTTP");
					TCActor<CHttpClientActor> HttpClientActor(fg_Construct(WebServerResults.m_CertificateConfig), "HTTP Client");
					TCFuture<CHttpClientActor::CResult> RequestFuture = HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpUrl.f_Encode(), Headers, Data, Cookies).f_Call();
					CClock Clock{true};
					co_await fg_Move(HttpClientActor).f_Destroy();
					DMibExpect(Clock.f_GetTime(), <, 1.0);

					auto RequestResult = co_await fg_Move(RequestFuture).f_Wrap();
					DMibExpectTrue(!RequestResult);
					DMibExpect(RequestResult.f_GetExceptionStr(), ==, "Aborted request");
				}
				{
					DMibTestPath("HTTPS");
					TCActor<CHttpClientActor> HttpClientActor(fg_Construct(WebServerResults.m_CertificateConfig), "HTTP Client");
					TCFuture<CHttpClientActor::CResult> RequestFuture = HttpClientActor(&CHttpClientActor::f_Request, CHttpClientActor::EMethod_GET, HttpsUrl.f_Encode(), Headers, Data, Cookies).f_Call();
					CClock Clock{true};
					co_await fg_Move(HttpClientActor).f_Destroy();
					DMibExpect(Clock.f_GetTime(), <, 1.0);

					auto RequestResult = co_await fg_Move(RequestFuture).f_Wrap();
					DMibExpectTrue(!RequestResult);
					DMibExpect(RequestResult.f_GetExceptionStr(), ==, "Aborted request");
				}
			}

			co_await WebServerResults.m_WebServerLaunch(&CProcessLaunchActor::f_StopProcessGroup);
			co_await WebServerResults.m_Subscription->f_Destroy();
			WebServerResults.m_Subscription.f_Clear();

			co_return {};
		};
	}
};

DMibTestRegister(CHttpClient_Tests, Malterlib::Web);
