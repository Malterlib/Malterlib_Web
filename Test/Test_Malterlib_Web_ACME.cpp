// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Test/Exception>
#include <Mib/Web/ACME>
#include <Mib/Web/HttpClient>
#include <Mib/File/File>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Cryptography/PublicCrypto>
#include <Mib/Cryptography/Certificate>
#include <Mib/Encoding/JsonShortcuts>

using namespace NMib;
using namespace NMib::NWeb;
using namespace NMib::NTest;
using namespace NMib::NContainer;
using namespace NMib::NStr;
using namespace NMib::NCryptography;
using namespace NMib::NFile;
using namespace NMib::NProcess;
using namespace NMib::NNetwork;
using namespace NMib::NEncoding;
using namespace NMib::NConcurrency;
using namespace NMib::NStorage;
using namespace NMib::NTime;

#ifdef DMibWebPebbleTests

namespace
{
	fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;

	// The MAC key Pebble accepts, from the externalAccountMACKeys in the config below
	constexpr ch8 const *gc_PebbleKeyID = "kid-1";
	constexpr ch8 const *gc_PebbleMacKey = "zWNDZM6eQGHWpSRTPal5eIUYFTu7EajVIoguysqZ9wG44nMEtx3MUAsUDkMTQ12W";
	constexpr ch8 const *gc_WrongMacKey = "b10lLJs8l1GPIzsLP0s6pMt8O0XVGnfTaCeROxQM0BIt2XrJMDHJZBM5NuQmQJQH";
}

// Runs the ACME client against Pebble, Let's Encrypt's test server, which is built from External/pebble into TestApps/Pebble. Pebble accepts every
// challenge (PEBBLE_VA_ALWAYS_VALID), so nothing needs to be served for the challenges.
class CACME_Tests : public NMib::NTest::CTest
{
public:
	struct CPebble
	{
		CPebble() = default;
		CPebble(CPebble &&) = default;
		~CPebble()
		{
			if (m_Subscription)
				m_Subscription->f_Destroy().f_DiscardResult();

			if (m_Launch)
				fg_Move(m_Launch).f_Destroy().f_DiscardResult();
		}

		TCActor<CProcessLaunchActor> m_Launch;
		CActorSubscription m_Subscription;
		CStr m_DirectoryURL;
		CByteVector m_CertificateAuthority;
	};

	// The certificate Pebble serves its API with. Generated, because the one in Pebble's repository expires.
	static CByteVector fs_GenerateCertificates(CStr const &_Directory)
	{
		CPublicKeySetting KeySettings = CPublicKeySettings_EC_secp256r1{};

		CByteVector CaCertData;
		CSecureByteVector CaKeyData;
		{
			CCertificateOptions Options;
			Options.m_CommonName = "Malterlib ACME test CA";
			Options.m_KeySetting = KeySettings;
			Options.f_MakeCA();

			CCertificateSignOptions SignOptions;
			SignOptions.m_Days = 30;
			SignOptions.f_AddExtension_SubjectKeyIdentifier();

			CCertificate::fs_GenerateSelfSignedCertAndKey(Options, CaCertData, CaKeyData, SignOptions);
		}

		CCertificateOptions Options;
		Options.m_CommonName = "localhost";
		Options.m_KeySetting = KeySettings;
		// m_Hostnames only makes DNS names, and the client connects to the IP address
		CCertificateExtension SubjectAltName;
		SubjectAltName.m_Value = "DNS:localhost,IP:127.0.0.1";
		Options.m_Extensions["subjectAltName"].f_Insert(SubjectAltName);
		Options.f_AddExtension_BasicConstraints(false);
		Options.f_AddExtension_KeyUsage(EKeyUsage_KeyEncipherment | EKeyUsage_DigitalSignature);

		CCertificateSignOptions SignOptions;
		SignOptions.m_Serial = 1;
		SignOptions.m_Days = 30;
		SignOptions.f_AddExtension_AuthorityKeyIdentifier();

		CByteVector CertRequestData;
		CByteVector CertData;
		CSecureByteVector KeyData;
		CCertificate::fs_GenerateClientCertificateRequest(Options, CertRequestData, KeyData);
		CCertificate::fs_SignClientCertificate(CaCertData, CaKeyData, CertRequestData, CertData, SignOptions);

		CFile::fs_WriteFile(CertData, _Directory / "cert.pem");
		CFile::fs_WriteFileSecure(KeyData, _Directory / "key.pem");

		return CaCertData;
	}

	// _NonceRejectFirst is how many of the first good nonces Pebble rejects with badNonce. It doesn't reject any others.
	static TCFuture<CPebble> fs_StartPebble(CStr _Directory, uint32 _NonceRejectFirst)
	{
		CStr PebbleExecutable = CFile::fs_GetProgramDirectory() / "TestApps/Pebble/pebble";
#ifdef DPlatformFamily_Windows
		PebbleExecutable += ".exe";
#endif
		if (!CFile::fs_FileExists(PebbleExecutable))
			co_return DMibErrorInstance("Pebble was not built: {}"_f << PebbleExecutable);

		CStr ConfigFile = _Directory / "config.json";

		CProcessLaunch::fs_KillProcesses
			(
				[ConfigFile](CProcessInfo const &_ProcessInfo) -> bool
				{
					return CFile::fs_GetFileNoExt(_ProcessInfo.m_FileName) == "pebble" && _ProcessInfo.m_Args.f_Contains(ConfigFile) >= 0;
				}
				, EProcessInfoFlag_FileName | EProcessInfoFlag_Args
			)
		;

		if (CFile::fs_FileExists(_Directory))
			CFile::fs_DeleteDirectoryRecursive(_Directory);
		CFile::fs_CreateDirectory(_Directory);

		CByteVector CertificateAuthority = fs_GenerateCertificates(_Directory);

		co_return co_await fs_LaunchPebble(PebbleExecutable, _Directory, ConfigFile, CertificateAuthority, _NonceRejectFirst);
	}

	static TCFuture<CPebble> fs_LaunchPebble
		(
			CStr _PebbleExecutable
			, CStr _Directory
			, CStr _ConfigFile
			, CByteVector _CertificateAuthority
			, uint32 _NonceRejectFirst
		)
	{
		CPebble Pebble;
		Pebble.m_CertificateAuthority = _CertificateAuthority;

		// A Unix socket rather than a TCP port, which someone else could take before Pebble listens on it. The client connects through the socket for
		// a [UNIX:<path>] host, and Pebble puts that host in its URLs.
		CStr SocketPath = fg_GetSafeUnixSocketPath(_Directory / "pebble.socket");
		Pebble.m_DirectoryURL = "https://[UNIX:{}]/dir"_f << SocketPath;

		CJsonSorted Config =
			{
				"pebble"_j=
				{
					"listenAddress"_j= "unix:" + SocketPath
					, "managementListenAddress"_j= ""
					, "certificate"_j= _Directory / "cert.pem"
					, "privateKey"_j= _Directory / "key.pem"
					, "httpPort"_j= 5002
					, "tlsPort"_j= 5001
					, "ocspResponderURL"_j= ""
					, "retryAfter"_j= {"authz"_j= 1, "order"_j= 1}
					, "externalAccountBindingRequired"_j= true
				}
			}
		;
		Config["pebble"]["externalAccountMACKeys"][gc_PebbleKeyID] = gc_PebbleMacKey;
		CFile::fs_WriteStringToFile(_ConfigFile, Config.f_ToString(), false);

		Pebble.m_Launch = fg_Construct();
		CProcessLaunchActor::CLaunch Launch(CProcessLaunchParams::fs_LaunchExecutable(_PebbleExecutable, {"-config", _ConfigFile}, _Directory, nullptr));
#ifdef DPlatformFamily_Windows
		// Needed on Windows to stop it with CTRL_BREAK. Elsewhere Pebble stays in the test's process group, so it goes when the test is killed.
		Launch.m_Params.m_bCreateNewProcessGroup = true;
#endif
		Launch.m_Params.m_Environment["PEBBLE_VA_ALWAYS_VALID"] = "1";
		Launch.m_Params.m_Environment["PEBBLE_VA_NOSLEEP"] = "1";
		// Pebble rejects 5% of valid nonces with badNonce by default. Several in a row would exhaust the client's retries, so the retries are tested
		// with a known number of rejections instead.
		Launch.m_Params.m_Environment["PEBBLE_WFE_NONCEREJECT"] = "0";
		if (_NonceRejectFirst)
			Launch.m_Params.m_Environment["PEBBLE_WFE_NONCEREJECT_FIRST"] = "{}"_f << _NonceRejectFirst;

		struct CLaunchState
		{
			CStr m_Output;
			bool m_bExited = false;
		};
		TCSharedPointer<CLaunchState> pLaunchState = fg_Construct();

		Launch.m_Params.m_fOnOutput = [pLaunchState](EProcessLaunchOutputType _OutputType, CStr const &_Output)
			{
				pLaunchState->m_Output += _Output;
			}
		;
		Launch.m_Params.m_fOnStateChange = [pLaunchState](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
			{
				if (_State.f_GetTypeID() == EProcessLaunchState_Exited || _State.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
					pLaunchState->m_bExited = true;
			}
		;

		Pebble.m_Subscription = co_await Pebble.m_Launch(&CProcessLaunchActor::f_Launch, Launch, fg_CurrentActor());

		// Pebble logs its address before it listens, so wait for the directory to answer instead
		CHttpClientActor::CCertificateConfig CertificateConfig;
		CertificateConfig.m_CertificateAuthorities = _CertificateAuthority;
		TCActor<CHttpClientActor> HttpClient = fg_ConstructActor<CHttpClientActor>(CertificateConfig);

		CStr LastError;
		CStopwatch Stopwatch{true};
		while (true)
		{
			auto Result = co_await HttpClient(&CHttpClientActor::f_Get, Pebble.m_DirectoryURL, TCMap<CStr, CStr>{{"User-Agent", "Malterlib ACME test"}})
				.f_Timeout(g_Timeout, "Timed out getting the Pebble directory")
				.f_Wrap()
			;
			if (Result && Result->m_StatusCode == 200)
				break;

			if (Result)
				LastError = "Status {}"_f << Result->m_StatusCode;
			else
				LastError = Result.f_GetExceptionStr();

			if (pLaunchState->m_bExited || Stopwatch.f_GetTime() > g_Timeout)
			{
				co_await fg_Move(HttpClient).f_Destroy();
				co_return DMibErrorInstance("Pebble didn't start: {}. Output:\n{}"_f << LastError << pLaunchState->m_Output);
			}

			co_await fg_Timeout(0.1);
		}

		co_await fg_Move(HttpClient).f_Destroy();

		co_return fg_Move(Pebble);
	}

	static TCFuture<void> fs_StopPebble(CPebble _Pebble)
	{
#ifdef DPlatformFamily_Windows
		co_await _Pebble.m_Launch(&CProcessLaunchActor::f_StopProcessGroup);
#else
		co_await _Pebble.m_Launch(&CProcessLaunchActor::f_StopProcess);
#endif
		co_await _Pebble.m_Subscription->f_Destroy();
		_Pebble.m_Subscription.f_Clear();
		co_await fg_Move(_Pebble.m_Launch).f_Destroy();

		co_return {};
	}

	static TCFuture<CAcmeClientActor::CCertificateChains> fs_RequestCertificate
		(
			CStr _DirectoryURL
			, CByteVector _CertificateAuthority
			, TCOptional<CAcmeClientActor::CExternalAccountBinding> _Binding
			, TCVector<CStr> _DnsNames
			, TCSharedPointer<umint> _pChallengeCount
		)
	{
		CHttpClientActor::CCertificateConfig CertificateConfig;
		CertificateConfig.m_CertificateAuthorities = _CertificateAuthority;

		CAcmeClientActor::CDependencies Dependencies(CAcmeClientActor::EDefaultDirectory_Custom, _DirectoryURL);
		Dependencies.m_HttpClientActor = fg_ConstructActor<CHttpClientActor>(CertificateConfig);

		CSecureByteVector PublicKey;
		CPublicCrypto::fs_GenerateKeys(Dependencies.m_AccountInfo.m_AccountPrivateKey, PublicKey, CPublicKeySettings_EC_secp256r1{});
		Dependencies.m_AccountInfo.m_ExternalAccountBinding = fg_Move(_Binding);

		auto HttpClient = Dependencies.m_HttpClientActor;
		auto Acme = fg_ConstructActor<CAcmeClientActor>(fg_Move(Dependencies));

		CAcmeClientActor::CCertificateRequest Request;
		Request.m_DnsNames = fg_Move(_DnsNames);
		Request.m_KeySettings = CPublicKeySettings_EC_secp256r1{};
		Request.m_Timeout = g_Timeout;
		Request.m_fChallenge = g_ActorFunctor / [_pChallengeCount](CAcmeClientActor::CChallenge _Challenge) -> TCFuture<bool>
			{
				if (_Challenge.m_Type != CAcmeClientActor::EChallengeType_Http01)
					co_return false;

				++*_pChallengeCount;
				co_return true;
			}
		;

		// m_Timeout only limits the waits for status changes, so the whole request is limited too, in case Pebble stops answering
		auto Result = co_await Acme(&CAcmeClientActor::f_RequestCertificate, fg_Move(Request))
			.f_Timeout(g_Timeout * 2.0, "Timed out requesting the certificate")
			.f_Wrap()
		;

		co_await fg_Move(Acme).f_Destroy();
		co_await fg_Move(HttpClient).f_Destroy();

		co_return fg_Move(Result);
	}

	void f_DoTests()
	{
		DMibTestSuite("Pebble") -> TCFuture<void>
		{
			CStr TestDirectory = CFile::fs_GetProgramDirectory() / "TestWebACME";
			fg_TestAddCleanupPath(TestDirectory);

			auto Pebble = co_await fs_StartPebble(TestDirectory, 0);
			// Also stopped when a check throws
			auto StopPebble = co_await fg_AsyncDestroy
				(
					[&]() -> TCFuture<void>
					{
						co_await fs_StopPebble(fg_Move(Pebble));
						co_return {};
					}
				)
			;
			CStr DirectoryURL = Pebble.m_DirectoryURL;
			CByteVector CertificateAuthority = Pebble.m_CertificateAuthority;

			{
				DMibTestPath("Binding required");
				TCSharedPointer<umint> pChallengeCount = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate(DirectoryURL, CertificateAuthority, {}, {"example.test"}, pChallengeCount).f_Wrap();
				DMibExpectFalse(bool(Result));
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr().f_Find("requires an external account binding"), >=, 0);
			}
			{
				DMibTestPath("Wrong MAC key");
				TCSharedPointer<umint> pChallengeCount = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						DirectoryURL
						, CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, gc_WrongMacKey}
						, {"example.test"}
						, pChallengeCount
					)
					.f_Wrap()
				;
				DMibExpectFalse(bool(Result));
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr().f_Find("urn:ietf:params:acme:error:unauthorized"), >=, 0);
			}
			{
				DMibTestPath("Invalid MAC key");
				TCSharedPointer<umint> pChallengeCount = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						DirectoryURL
						, CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, "not+base64url"}
						, {"example.test"}
						, pChallengeCount
					)
					.f_Wrap()
				;
				DMibExpectFalse(bool(Result));
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr().f_Find("Not valid base64url"), >=, 0);
			}
			{
				DMibTestPath("Several names");
				TCSharedPointer<umint> pChallengeCount = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						DirectoryURL
						, CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, gc_PebbleMacKey}
						, {"example.test", "www.example.test", "api.example.test"}
						, pChallengeCount
					)
					.f_Wrap()
				;
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr(), ==, "");
				DMibAssertTrue(bool(Result));
				DMibExpect(*pChallengeCount, ==, 3);
				DMibExpectTrue(Result->m_DefaultChain.m_EndEntity.f_StartsWith("-----BEGIN CERTIFICATE-----"));
			}

			co_await fg_Move(StopPebble);

			// The client resends a request with a fresh nonce up to three times when the server rejects the nonce
			{
				DMibTestPath("Nonce rejected twice");
				auto RejectingPebble = co_await fs_StartPebble(TestDirectory, 2);
				auto StopRejectingPebble = co_await fg_AsyncDestroy
					(
						[&]() -> TCFuture<void>
						{
							co_await fs_StopPebble(fg_Move(RejectingPebble));
							co_return {};
						}
					)
				;
				TCSharedPointer<umint> pChallengeCount = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						RejectingPebble.m_DirectoryURL
						, RejectingPebble.m_CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, gc_PebbleMacKey}
						, {"example.test"}
						, pChallengeCount
					)
					.f_Wrap()
				;
				co_await fg_Move(StopRejectingPebble);
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr(), ==, "");
				DMibExpectTrue(bool(Result));
			}
			{
				DMibTestPath("Nonce rejected four times");
				auto RejectingPebble = co_await fs_StartPebble(TestDirectory, 4);
				auto StopRejectingPebble = co_await fg_AsyncDestroy
					(
						[&]() -> TCFuture<void>
						{
							co_await fs_StopPebble(fg_Move(RejectingPebble));
							co_return {};
						}
					)
				;
				TCSharedPointer<umint> pChallengeCount = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						RejectingPebble.m_DirectoryURL
						, RejectingPebble.m_CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, gc_PebbleMacKey}
						, {"example.test"}
						, pChallengeCount
					)
					.f_Wrap()
				;
				co_await fg_Move(StopRejectingPebble);
				DMibExpectFalse(bool(Result));
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr().f_Find("urn:ietf:params:acme:error:badNonce"), >=, 0);
			}

			co_return {};
		};
	}
};

DMibTestRegister(CACME_Tests, Malterlib::Web);

#endif
