// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Test/Exception>
#include <Mib/Web/ACME>
#include <Mib/Web/HttpClient>
#include <Mib/File/File>
#include <Mib/Process/ProcessLaunchActor>
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

	constexpr umint gc_MaxPebbleLaunchAttempts = 5;
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

	static uint16 fs_GetFreePort()
	{
		CNetAddressTCPv4 Address;
		Address.f_SetLocalhost();
		Address.m_Port = 0;
		CSocket Listener;
		Listener.f_Listen(Address, nullptr, ENetFlag_None);
		return uint16(Listener.f_GetListenPort());
	}

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

	static TCFuture<CPebble> fs_StartPebble(CStr _Directory)
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

		// The free port can be taken by someone else before Pebble listens on it. Pebble can't report the port it got for port 0, so try another port.
		for (umint iAttempt = 0; ; ++iAttempt)
		{
			auto Result = co_await fs_LaunchPebble(PebbleExecutable, _Directory, ConfigFile, CertificateAuthority).f_Wrap();
			if (Result)
				co_return fg_Move(*Result);

			CStr Error = Result.f_GetExceptionStr();
			bool bPortTaken = Error.f_Find("address already in use") >= 0 || Error.f_Find("Only one usage of each socket address") >= 0;
			if (!bPortTaken || iAttempt + 1 == gc_MaxPebbleLaunchAttempts)
				co_return Result.f_GetException();
		}
	}

	static TCFuture<CPebble> fs_LaunchPebble(CStr _PebbleExecutable, CStr _Directory, CStr _ConfigFile, CByteVector _CertificateAuthority)
	{
		CPebble Pebble;
		Pebble.m_CertificateAuthority = _CertificateAuthority;

		uint16 Port = fs_GetFreePort();
		// The IP address rather than localhost, which can resolve to ::1 where something else may listen on the same port
		Pebble.m_DirectoryURL = "https://127.0.0.1:{}/dir"_f << Port;

		CJsonSorted Config =
			{
				"pebble"_j=
				{
					"listenAddress"_j= "127.0.0.1:{}"_f << Port
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
		// Pebble rejects 5% of valid nonces with badNonce by default, which exercises the client's retries

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
			, TCSharedPointer<umint> _pnChallenges
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
		Request.m_fChallenge = g_ActorFunctor / [_pnChallenges](CAcmeClientActor::CChallenge _Challenge) -> TCFuture<bool>
			{
				if (_Challenge.m_Type != CAcmeClientActor::EChallengeType_Http01)
					co_return false;

				++*_pnChallenges;
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

			auto Pebble = co_await fs_StartPebble(TestDirectory);
			CStr DirectoryURL = Pebble.m_DirectoryURL;
			CByteVector CertificateAuthority = Pebble.m_CertificateAuthority;

			{
				DMibTestPath("Binding required");
				TCSharedPointer<umint> pnChallenges = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate(DirectoryURL, CertificateAuthority, {}, {"example.test"}, pnChallenges).f_Wrap();
				DMibExpectFalse(bool(Result));
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr().f_Find("requires an external account binding"), >=, 0);
			}
			{
				DMibTestPath("Wrong MAC key");
				TCSharedPointer<umint> pnChallenges = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						DirectoryURL
						, CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, gc_WrongMacKey}
						, {"example.test"}
						, pnChallenges
					)
					.f_Wrap()
				;
				DMibExpectFalse(bool(Result));
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr().f_Find("urn:ietf:params:acme:error:unauthorized"), >=, 0);
			}
			{
				DMibTestPath("Invalid MAC key");
				TCSharedPointer<umint> pnChallenges = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						DirectoryURL
						, CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, "not+base64url"}
						, {"example.test"}
						, pnChallenges
					)
					.f_Wrap()
				;
				DMibExpectFalse(bool(Result));
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr().f_Find("Not valid base64url"), >=, 0);
			}
			{
				DMibTestPath("Several names");
				TCSharedPointer<umint> pnChallenges = fg_Construct(0);
				auto Result = co_await fs_RequestCertificate
					(
						DirectoryURL
						, CertificateAuthority
						, CAcmeClientActor::CExternalAccountBinding{gc_PebbleKeyID, gc_PebbleMacKey}
						, {"example.test", "www.example.test", "api.example.test"}
						, pnChallenges
					)
					.f_Wrap()
				;
				if (!Result)
					DMibExpect(Result.f_GetExceptionStr(), ==, "");
				DMibAssertTrue(bool(Result));
				DMibExpect(*pnChallenges, ==, 3);
				DMibExpectTrue(Result->m_DefaultChain.m_EndEntity.f_StartsWith("-----BEGIN CERTIFICATE-----"));
			}

			co_await fs_StopPebble(fg_Move(Pebble));

			co_return {};
		};
	}
};

DMibTestRegister(CACME_Tests, Malterlib::Web);

#endif
