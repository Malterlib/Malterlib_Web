// Copyright © 2020 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/Json>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/Certificate>
#include <Mib/Web/HttpClient>
#include <Mib/XML/XML>

#include "Malterlib_Web_ACME.h"

namespace NMib::NWeb
{
	using namespace NConcurrency;
	using namespace NCryptography;
	using namespace NStr;
	using namespace NContainer;
	using namespace NEncoding;
	using namespace NException;
	using namespace NTime;
	using namespace NStorage;

	struct CAcmeClientActor::CInternal : public CActorInternal
	{
		CInternal(CAcmeClientActor::CDependencies &&_Dependencies)
			: m_Dependencies(fg_Move(_Dependencies))
		{
		}

		CAcmeClientActor::CDependencies m_Dependencies;
	};

	namespace
	{
		NHTTP::CURL fg_GetDirectoryURL(CAcmeClientActor::EDefaultDirectory _Directory, NStr::CStr const &_CustomDirectory)
		{
			switch (_Directory)
			{
			case CAcmeClientActor::EDefaultDirectory_Custom: return _CustomDirectory;
			case CAcmeClientActor::EDefaultDirectory_LetsEncrypt: return NHTTP::CURL("https://acme-v02.api.letsencrypt.org/directory");
			case CAcmeClientActor::EDefaultDirectory_LetsEncryptStaging: return NHTTP::CURL("https://acme-staging-v02.api.letsencrypt.org/directory");
			}
			return {};
		}

		template <typename tf_CSource>
		CStr fg_Base64URLEncode(tf_CSource const &_Source)
		{
			return fg_Base64Encode(_Source).f_TrimRight("=").f_ReplaceChar('+', '-').f_ReplaceChar('/', '_');
		}

		CStr fg_JwkThumbPrint(CJsonSorted const &_Json)
		{
			CStr String = _Json.f_ToString(nullptr);
			auto Digest = CHash_SHA256::fs_DigestFromData(String.f_GetStr(), String.f_GetLen());
			return fg_Base64URLEncode(CByteVector(Digest.f_GetData(), Digest.mc_Size));
		}

		struct CAcmeState
		{
			CAcmeState(CAcmeClientActor::CDependencies const &_Dependencies)
				: m_Dependencies(_Dependencies)
			{
			}

			CAcmeClientActor::CDependencies const &m_Dependencies;
			CStr m_AccountLocation;
			CStr m_Algorithm;
			CStrSecure m_Nonce;
			CStrSecure m_JwkThumbPrint;
			EDigestType m_DigestType = EDigestType_None;
		};

		CByteVector fg_FlattenedJwsEncode(CStr const &_Url, CJsonSorted &&_Payload, CAcmeState &_State)
		{
			CJsonSorted JwsMessage;
			CJsonSorted Protected;

			Protected["nonce"] = _State.m_Nonce;
			if (_State.m_AccountLocation)
				Protected["kid"] = _State.m_AccountLocation;
			else
			{
				auto KeySettings = CPublicCrypto::fs_PublicKeySettingsFromPrivateKey(_State.m_Dependencies.m_AccountInfo.m_AccountPrivateKey);
				auto PublicKey = CPublicCrypto::fs_GetPublicKeyFromPrivateKey(_State.m_Dependencies.m_AccountInfo.m_AccountPrivateKey);
				auto KeyParams = CPublicCrypto::fs_GetPublicKeyParameters(PublicKey);

				CJsonSorted Jwk;
				CStr Algorithm;

				switch (KeySettings.f_GetTypeID())
				{
				case EPublicKeyType::mc_RSA:
					{
						_State.m_Algorithm = "RS256";
						_State.m_DigestType = EDigestType_SHA256;

						if (!KeyParams.f_IsOfType<CPublicCrypto::CPublicKeyParameters_RSA>())
							DMibError("Public key and private key type do not match");

						auto &RsaParams = KeyParams.f_GetAsType<CPublicCrypto::CPublicKeyParameters_RSA>();

						Jwk =
							{
								"e"_j= fg_Base64URLEncode(RsaParams.m_Exponent)
								, "kty"_j= "RSA"
								, "n"_j= fg_Base64URLEncode(RsaParams.m_Modulus)
							}
						;
					}
					break;
				case EPublicKeyType::mc_EC_secp256r1:
				case EPublicKeyType::mc_EC_secp384r1:
				case EPublicKeyType::mc_EC_secp521r1:
					{
						if (!KeyParams.f_IsOfType<CPublicCrypto::CPublicKeyParameters_EC>())
							DMibError("Public key and private key type do not match");

						auto &EcParams = KeyParams.f_GetAsType<CPublicCrypto::CPublicKeyParameters_EC>();

						CStr Curve;
						switch (KeySettings.f_GetTypeID())
						{
						case EPublicKeyType::mc_EC_secp256r1:
							Curve = "P-256";
							_State.m_Algorithm = "ES256";
							_State.m_DigestType = EDigestType_SHA256;
							break;
						case EPublicKeyType::mc_EC_secp384r1:
							Curve = "P-384";
							_State.m_Algorithm = "ES384";
							_State.m_DigestType = EDigestType_SHA384;
							break;
						case EPublicKeyType::mc_EC_secp521r1:
							Curve = "P-521";
							_State.m_Algorithm = "ES512";
							_State.m_DigestType = EDigestType_SHA512;
							break;
						default: DMibNeverGetHere; break;
						}

						Jwk =
							{
								"crv"_j= Curve
								, "kty"_j= "EC"
								, "x"_j= fg_Base64URLEncode(EcParams.m_CoordinateX)
								, "y"_j= fg_Base64URLEncode(EcParams.m_CoordinateY)
							}
						;
					}
					break;
				default:
					DMibError("Unsupported public key type. Only RSA, secp256r1, secp384r1 or secp521r1 is supported");
					break;
				}

				_State.m_JwkThumbPrint = fg_JwkThumbPrint(Jwk);
				Protected["jwk"] = fg_Move(Jwk);
			}
			Protected["alg"] = _State.m_Algorithm;
			Protected["url"] = _Url;

			Protected.f_SortObjectsLexicographically();

			CStr PayloadString;
			if (_Payload.f_IsValid())
			{
				_Payload.f_SortObjectsLexicographically();
				PayloadString = fg_Base64URLEncode(_Payload.f_ToString(nullptr));
			}

			CStr ProtectedString = fg_Base64URLEncode(Protected.f_ToString(nullptr));

			JwsMessage["protected"] = ProtectedString;
			JwsMessage["payload"] = PayloadString;

			CSecureByteVector ToSign;
			ToSign.f_Insert((uint8 const *)ProtectedString.f_GetStr(), ProtectedString.f_GetLen());
			ToSign.f_Insert('.');
			ToSign.f_Insert((uint8 const *)PayloadString.f_GetStr(), PayloadString.f_GetLen());

			auto RawSignature = CPublicCrypto::fs_SignMessageRawSignature(ToSign, _State.m_Dependencies.m_AccountInfo.m_AccountPrivateKey, _State.m_DigestType);
			CStr Signature = fg_Base64URLEncode(RawSignature);
			JwsMessage["signature"] = Signature;
			JwsMessage.f_SortObjectsLexicographically();

			CStr OutputString = JwsMessage.f_ToString(nullptr);
			return CByteVector((uint8 const *)OutputString.f_GetStr(), OutputString.f_GetLen());
		}
	}

	CAcmeClientActor::CDependencies::CDependencies(CAcmeClientActor::EDefaultDirectory _Directory, NStr::CStr const &_CustomDirectory)
		: m_DirectoryURL(fg_GetDirectoryURL(_Directory, _CustomDirectory))
	{
	}

	CAcmeClientActor::CAcmeClientActor(CDependencies &&_Dependencies)
		: mp_pInternal{fg_Construct(fg_Move(_Dependencies))}
	{
	}

	CAcmeClientActor::~CAcmeClientActor() = default;

	namespace
	{
		TCVector<CStr> fg_ParseLinks(NContainer::TCMap<NStr::CStr, NStr::CStr, NStr::CCompareNoCase> const &_Headers, CStr const &_Relation)
		{
			auto pLink = _Headers.f_FindEqual("link");
			if (!pLink)
				return {};

			TCVector<CStr> Return;

			auto *pParse = pLink->f_GetStr();
			while (*pParse)
			{
				fg_ParseWhiteSpace(pParse);
				if (*pParse != '<')
					break;
				++pParse;

				auto pParseStart = pParse;
				while (*pParse && *pParse != '>')
					++pParse;

				if (*pParse != '>')
					break;

				CStr URL(pParseStart, pParse - pParseStart);

				++pParse;

				while (*pParse && *pParse != ',')
				{
					fg_ParseWhiteSpace(pParse);

					if (*pParse != ';')
						return {}; // Error

					++pParse;

					fg_ParseWhiteSpace(pParse);

					pParseStart = pParse;

					while (*pParse && *pParse != '=')
						++pParse;

					if (*pParse != '=')
						return {};

					CStr ParamName(pParseStart, pParse - pParseStart);

					++pParse;

					CStr ParamValue;
					if (*pParse == '"')
					{
						++pParse;

						pParseStart = pParse;

						while (*pParse && *pParse != '"')
							++pParse;

						if (*pParse != '"')
							return {};

						ParamValue = CStr(pParseStart, pParse - pParseStart);
						++pParse;
					}
					else
					{
						pParseStart = pParse;

						while (*pParse && !fg_CharIsWhiteSpace(*pParse) && *pParse != ',' && *pParse != ';')
							++pParse;

						ParamValue = CStr(pParseStart, pParse - pParseStart);
					}

					if (ParamName == "rel" && ParamValue == _Relation)
						Return.f_Insert(URL);
				}
			}

			return Return;
		}
	}

	auto CAcmeClientActor::f_RequestCertificate(CCertificateRequest _RequestCertificate) -> TCFuture<CCertificateChains>
	{
		auto &Internal = *mp_pInternal;

		if (_RequestCertificate.m_DnsNames.f_IsEmpty())
			co_return DMibErrorInstance("Expected one or more dns names");

		CStr Directory = Internal.m_Dependencies.m_DirectoryURL.f_Encode();

		auto fGet = [this](CStr const &_Path)
			{
				auto &Internal = *mp_pInternal;
				return Internal.m_Dependencies.m_HttpClientActor.f_Bind<&CHttpClientActor::f_Get, EVirtualCall::mc_NotVirtual>
					(
						_Path
						, fg_Default()
					)
					.f_Call()
				;
			}
		;

		auto fHead = [this](CStr const &_Path)
			{
				auto &Internal = *mp_pInternal;
				return Internal.m_Dependencies.m_HttpClientActor.f_Bind<&CHttpClientActor::f_Head, EVirtualCall::mc_NotVirtual>
					(
						_Path
						, fg_Default()
					)
					.f_Call()
				;
			}
		;

		TCSharedPointer<CAcmeState> pState = fg_Construct(Internal.m_Dependencies);

		auto fPost = [this, pState](CStr const &_Path, CJsonSorted &&_Payload)
			{
				auto &Internal = *mp_pInternal;
				TCMap<CStr, CStr> Headers = {{"Content-Type", "application/jose+json"}};

				return Internal.m_Dependencies.m_HttpClientActor.f_Bind<&CHttpClientActor::f_Post, EVirtualCall::mc_NotVirtual>
					(
						_Path
						, fg_Move(Headers)
						, fg_FlattenedJwsEncode(_Path, fg_Move(_Payload), *pState)
					)
					.f_Call()
				;
			}
		;
		auto fPostAsGet = [this, pState](CStr const &_Path)
			{
				auto &Internal = *mp_pInternal;
				TCMap<CStr, CStr> Headers = {{"Content-Type", "application/jose+json"}};

				return Internal.m_Dependencies.m_HttpClientActor.f_Bind<&CHttpClientActor::f_Post, EVirtualCall::mc_NotVirtual>
					(
						_Path
						, fg_Move(Headers)
						, fg_FlattenedJwsEncode(_Path, {}, *pState)
					)
					.f_Call()
				;
			}
		;

		auto fUpdateNonce = [pState](CHttpClientActor::CResult const &_Result) -> TCUnsafeFuture<void>
			{
				CStr Nonce;

				if (auto *pNonce = _Result.m_Headers.f_FindEqual("Replay-Nonce"))
					Nonce = *pNonce;

				if (!Nonce)
					co_return DMibErrorInstance("No nonce in response");

				pState->m_Nonce = fg_Move(Nonce);

				co_return {};
			}
		;

		auto fGetErrorFromJson = [](CEJsonSorted const &_Json)
			{
				CStr ErrorMessage;
				if (auto *pValue = _Json.f_GetMember("type", EJsonType_String))
					fg_AddStrSep(ErrorMessage, pValue->f_String(), ". ");

				if (auto *pValue = _Json.f_GetMember("detail", EJsonType_String))
					fg_AddStrSep(ErrorMessage, pValue->f_String(), ". ");

				if (auto *pSubProblems = _Json.f_GetMember("subproblems", EJsonType_Array))
				{
					for (auto &SubProblem : pSubProblems->f_Array())
					{
						CStr SubMessage;

						if (auto *pValue = SubProblem.f_GetMember("type", EJsonType_String))
							fg_AddStrSep(SubMessage, pValue->f_String(), ". ");
						if (auto *pValue = SubProblem.f_GetMember("detail", EJsonType_String))
							fg_AddStrSep(SubMessage, pValue->f_String(), ". ");

						ErrorMessage += "\n    {}"_f << SubMessage;
					}
				}

				return ErrorMessage;
			}
		;

		auto fGetError = [fGetErrorFromJson](CHttpClientActor::CResult const &_Result, CStr const &_Context)
			{
				CStr ErrorMessage = "{}: {} {}"_f << _Context << _Result.m_StatusCode << _Result.m_StatusMessage;

				try
				{
					if (_Result.m_Body)
					{
						auto ErrorObject = _Result.f_ToJson();

						CStr Error = fGetErrorFromJson(ErrorObject);
						if (Error)
							ErrorMessage += ". {}"_f << Error;
					}
				}
				catch (CException const &)
				{
				}

				return DMibErrorInstance(ErrorMessage);
			}
		;

		auto fWaitForStatusValid = [&](CStr const &_Url, CStr const &_Message)
			{
				return self / [=, Timeout = _RequestCertificate.m_Timeout]() -> TCFuture<CEJsonSorted>
					{
						CStopwatch Stopwatch{true};

						while (true)
						{
							auto Result = co_await fPostAsGet(_Url);

							if (Result.m_StatusCode != 200)
								co_return fGetError(Result, "Error getting challenge result");

							co_await fUpdateNonce(Result);

							CEJsonSorted ResponseJson;
							{
								auto CaptureScope = co_await (g_CaptureExceptions % "Exception parsing authorization result");
								ResponseJson = Result.f_ToJson();
							}

							auto pStatus = ResponseJson.f_GetMember("status", EJsonType_String);
							if (!pStatus)
								co_return DMibErrorInstance("Authorization response missing 'status'");

							if (pStatus->f_String() == "valid")
								co_return ResponseJson;

							if (pStatus->f_String() == "invalid")
							{
								if (auto *pError = ResponseJson.f_GetMember("error", EJsonType_Object))
									co_return DMibErrorInstance("{cc} is invalid: {}"_f << _Message << fGetErrorFromJson(*pError));

								co_return DMibErrorInstance("{cc} is invalid"_f << _Message);
							}

							if (Stopwatch.f_GetTime() > Timeout)
								co_return DMibErrorInstance("Timed out waiting for {} to turn valid. Status is: '{}'"_f << _Message << ResponseJson);

							co_await fg_Timeout(3.0);
						}

						co_return {};
					}
				;
			}
		;

		auto GetResult = co_await fGet(Directory);
		if (GetResult.m_StatusCode != 200)
			co_return DMibErrorInstance("Unexpected status getting ACME directory: {} {}"_f << GetResult.m_StatusCode << GetResult.m_StatusMessage);

		CEJsonSorted DirectoryJson;
		{
			auto CaptureScope = co_await g_CaptureExceptions;
			DirectoryJson = GetResult.f_ToJson();
		}

		CStr NewAccountUrl;
		CStr NewNonceUrl;
		CStr NewOrderUrl;

		if (auto pValue = DirectoryJson.f_GetMember("newAccount", EJsonType_String))
			NewAccountUrl = pValue->f_String();
		else
			co_return DMibErrorInstance("Directory response is missing a valid 'newAccount' URL");

		if (auto pValue = DirectoryJson.f_GetMember("newNonce", EJsonType_String))
			NewNonceUrl = pValue->f_String();
		else
			co_return DMibErrorInstance("Directory response is missing a valid 'newNonce' URL");

		if (auto pValue = DirectoryJson.f_GetMember("newOrder", EJsonType_String))
			NewOrderUrl = pValue->f_String();
		else
			co_return DMibErrorInstance("Directory response is missing a valid 'newOrder' URL");

		{
			CStr Nonce;
			auto NonceResult = co_await fHead(NewNonceUrl);
			if (NonceResult.m_StatusCode != 200)
				co_return fGetError(NonceResult, "Unexpected status getting initial nonce");

			co_await fUpdateNonce(NonceResult);
		}

		{
			CJsonSorted EmailContacts = EJsonType_Array;

			for (auto &Email: Internal.m_Dependencies.m_AccountInfo.m_Emails)
				EmailContacts.f_Insert("mailto:{}"_f << Email);

			auto Result = co_await fPost
				(
					NewAccountUrl
					,
					{
						"contact"_j= fg_Move(EmailContacts)
						, "termsOfServiceAgreed"_j= true
						, "onlyReturnExisting"_j= false
					}
				)
			;
			if (Result.m_StatusCode != 200 && Result.m_StatusCode != 201)
				co_return fGetError(Result, "Error creating/getting account");

			if (auto pAccountLocation = Result.m_Headers.f_FindEqual("location"))
				pState->m_AccountLocation = *pAccountLocation;
			else
				co_return DMibErrorInstance("Missing location header in account creation/get response");

			co_await fUpdateNonce(Result);

			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Exception parsing create/get account result");

				auto ResponseJson = Result.f_ToJson();

				if (auto pStatus = ResponseJson.f_GetMember("status", EJsonType_String))
				{
					if (pStatus->f_String() != "valid")
						co_return DMibErrorInstance("Expected account status to be 'valid', got '{}'"_f << pStatus->f_String());
				}
				else
					co_return DMibErrorInstance("Missing status in create/get account response");
			}
		}

		TCVector<CStr> AuthorizationUrls;
		CStr OrderUrl;
		CStr FinalizeUrl;
		{
			CJsonSorted Identifiers = EJsonType_Array;
			for (auto &DnsName : _RequestCertificate.m_DnsNames)
				Identifiers.f_Array().f_Insert(CJsonSorted{"type"_j= "dns", "value"_j= DnsName});

			auto Result = co_await fPost(NewOrderUrl, {"identifiers"_j= fg_Move(Identifiers)});

			if (auto pLocation = Result.m_Headers.f_FindEqual("location"))
				OrderUrl = *pLocation;
			else
				co_return DMibErrorInstance("Missing location header in new order response");

			if (Result.m_StatusCode != 201)
				co_return fGetError(Result, "Error creating new certificate order");

			co_await fUpdateNonce(Result);

			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Exception parsing new certificate order result");
				auto ResponseJson = Result.f_ToJson();

				if (auto pAuthorizations = ResponseJson.f_GetMember("authorizations", EJsonType_Array))
					AuthorizationUrls = pAuthorizations->f_StringArray();
				else
					co_return DMibErrorInstance("Missing authorizations in new order response");

				if (auto pAuthorizations = ResponseJson.f_GetMember("finalize", EJsonType_String))
					FinalizeUrl = pAuthorizations->f_String();
				else
					co_return DMibErrorInstance("Missing finalize in new order response");
			}
		}

		for (auto &Url : AuthorizationUrls)
		{
			auto Result = co_await fPostAsGet(Url);
			if (Result.m_StatusCode != 200)
				co_return fGetError(Result, "Error getting authorization");

			co_await fUpdateNonce(Result);

			CStr SuccessfulChallengeUrl;

			{
				CEJsonSorted ResponseJson;
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Exception parsing authorization result");
					ResponseJson = Result.f_ToJson();
				}

				{
					auto pStatus = ResponseJson.f_GetMember("status", EJsonType_String);
					if (!pStatus)
						co_return DMibErrorInstance("Authorization response missing 'status'");

					if (pStatus->f_String() == "valid")
						continue;
				}

				CStr Identifier;
				{
					auto pIdentifier = ResponseJson.f_GetMember("identifier", EJsonType_Object);
					if (!pIdentifier)
						co_return DMibErrorInstance("Authorization response missing 'identifier'");

					if (auto Type = pIdentifier->f_GetMemberValue("type", "").f_String(); Type != "dns")
						co_return DMibErrorInstance("Expected authorization identifier type to be 'dns', got '{}'"_f << Type);

					Identifier = pIdentifier->f_GetMemberValue("value", "").f_String();
					if (!Identifier)
						co_return DMibErrorInstance("Expected authorization identifier value to not be empty");
				}

				auto pChallenges = ResponseJson.f_GetMember("challenges", EJsonType_Array);
				if (!pChallenges)
					co_return DMibErrorInstance("Authorization response missing 'challenges'");

				for (auto &Challenge : pChallenges->f_Array())
				{
					EChallengeType ChallengeType;
					if (auto *pType = Challenge.f_GetMember("type", EJsonType_String))
					{
						if (pType->f_String() == "http-01")
							ChallengeType = EChallengeType_Http01;
						else if (pType->f_String() == "dns-01")
							ChallengeType = EChallengeType_Dns01;
						else if (pType->f_String() == "tls-alpn-01")
							ChallengeType = EChallengeType_TlsAlpn01;
						else
							continue; // Unknown unsupported challenge type
					}
					else
						co_return DMibErrorInstance("Authorization response challenge is missing 'type'");

					auto *pToken = Challenge.f_GetMember("token", EJsonType_String);
					if (!pToken || pToken->f_String().f_IsEmpty())
						co_return DMibErrorInstance("Authorization response challenge is missing a valid 'token'");

					auto *pUrl = Challenge.f_GetMember("url", EJsonType_String);
					if (!pUrl || pUrl->f_String().f_IsEmpty())
						co_return DMibErrorInstance("Authorization response challenge is missing a valid 'url'");

					CStrSecure Token = CStrSecure::CFormat("{}.{}") << pToken->f_String() << pState->m_JwkThumbPrint;

					if (ChallengeType == EChallengeType_Dns01)
					{
						auto Digest = CHash_SHA256::fs_DigestFromData(Token.f_GetStr(), Token.f_GetLen());
						Token = fg_Base64URLEncode(CByteVector(Digest.f_GetData(), Digest.mc_Size));
					}

					if (co_await _RequestCertificate.m_fChallenge(CChallenge{ChallengeType, Token, Identifier}))
					{
						SuccessfulChallengeUrl = pUrl->f_String();
						break;
					}
				}
			}

			if (!SuccessfulChallengeUrl)
				co_return DMibErrorInstance("None of the challenges were successful, aborting");

			{
				auto Result = co_await fPost(SuccessfulChallengeUrl, EJsonType_Object);

				if (Result.m_StatusCode != 200)
					co_return fGetError(Result, "Error getting challenge result");

				co_await fUpdateNonce(Result);

				CEJsonSorted ResponseJson;
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Exception parsing authorization result");
					ResponseJson = Result.f_ToJson();
				}

				auto pStatus = ResponseJson.f_GetMember("status", EJsonType_String);
				if (!pStatus)
					co_return DMibErrorInstance("Authorization response missing 'status'");

				if (pStatus->f_String() == "valid")
					break;

				auto Url = ResponseJson.f_GetMemberValue("url", "").f_String();
				if (!Url)
					co_return DMibErrorInstance("Authorization response missing valid 'url'");

				co_await fWaitForStatusValid(Url, "authorization status");
			}
		}

		CCertificateChains ReturnChains;

		CCertificateOptions Options;
		Options.m_CommonName = _RequestCertificate.m_DnsNames[0];
		Options.m_Hostnames = _RequestCertificate.m_DnsNames;
		Options.m_KeySetting = _RequestCertificate.m_KeySettings;
		Options.f_AddExtension_BasicConstraints(false);
		Options.f_AddExtension_KeyUsage(EKeyUsage_KeyEncipherment | EKeyUsage_DigitalSignature);

		CByteVector CertificateSigningRequest;
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Exception generating certificate request");
			CSecureByteVector PrivateKey;
			CCertificate::fs_GenerateClientCertificateRequest(Options, CertificateSigningRequest, PrivateKey);
			ReturnChains.m_PrivateKey = CStrSecure((ch8 const *)PrivateKey.f_GetArray(), PrivateKey.f_GetLen());
		}

		CStr Status;
		CStr CertificateUrl;
		{
			auto Result = co_await fPost(FinalizeUrl, {"csr"_j= fg_Base64URLEncode(CCertificate::fs_ConvertToDer_CertificateSigningRequest(CertificateSigningRequest))});
			if (Result.m_StatusCode != 200)
				co_return fGetError(Result, "Error finalizing order");

			co_await fUpdateNonce(Result);

			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Exception parsing order finalize result");
				auto ResponseJson = Result.f_ToJson();

				if (auto pStatus = ResponseJson.f_GetMember("status", EJsonType_String))
					Status = pStatus->f_String();
				else
					co_return DMibErrorInstance("Finalize order response is missing 'status'");

			}
		}

		auto OrderResponseJson = co_await fWaitForStatusValid(OrderUrl, "order status");

		if (auto pCertificate = OrderResponseJson.f_GetMember("certificate", EJsonType_String))
			CertificateUrl = pCertificate->f_String();
		else
			co_return DMibErrorInstance("Order response is missing 'certificate': {}"_f << OrderResponseJson);


		{
			auto Result = co_await fPostAsGet(CertificateUrl);
			if (Result.m_StatusCode != 200)
				co_return fGetError(Result, "Error getting certificate");

			co_await fUpdateNonce(Result);

			auto &DefaultChain = ReturnChains.m_DefaultChain;

			auto fParseChain = [&](CStr const &_FullChain) -> CChain
				{
					CChain Chain;

					Chain.m_FullChain = _FullChain;

					TCVector<CStr> Certificates;

					CStr *pCurrentCertificate = nullptr;

					for (auto &Line : _FullChain.f_SplitLine())
					{
						if (Line.f_IsEmpty())
						{
							pCurrentCertificate = nullptr;
							continue;
						}
						if (!pCurrentCertificate)
							pCurrentCertificate = &Certificates.f_Insert();

						*pCurrentCertificate += Line;
						*pCurrentCertificate += "\n";

						if (Line == "-----END CERTIFICATE-----")
							pCurrentCertificate = nullptr;
					}

					if (Certificates.f_GetLen() >= 1)
					{
						Chain.m_EndEntity = Certificates[0];
						Chain.m_Root = Certificates.f_GetLast();
					}

					if (Certificates.f_GetLen() >= 2)
						Chain.m_Issuer = Certificates[1];

					if (Certificates.f_GetLen() >= 3)
					{
						umint nChains = Certificates.f_GetLen();
						for (umint i = 2; i < nChains; ++i)
							Chain.m_Other.f_Insert(Certificates[i]);
					}

					return Chain;
				}
			;

			DefaultChain = fParseChain(Result.m_Body);

			TCVector<CStr> AlternateLinks = fg_ParseLinks(Result.m_Headers, "alternate");

			for (auto &Link : AlternateLinks)
			{
				auto Result = co_await fPostAsGet(Link);
				if (Result.m_StatusCode != 200)
					co_return fGetError(Result, "Error alternate chain certificate");

				co_await fUpdateNonce(Result);

				ReturnChains.m_AlternateChains.f_Insert(fParseChain(Result.m_Body));
			}
		}

		co_return fg_Move(ReturnChains);
	}
}
