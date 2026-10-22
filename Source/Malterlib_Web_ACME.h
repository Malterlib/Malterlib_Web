// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>
#include <Mib/Cryptography/PublicCrypto>
#include <Mib/Storage/Optional>
#include <Mib/Web/HTTP/URL>

namespace NMib::NWeb
{
	struct CHttpClientActor;

	struct CAcmeClientActor : public NConcurrency::CActor
	{
		enum EDefaultDirectory
		{
			EDefaultDirectory_Custom
			, EDefaultDirectory_LetsEncrypt
			, EDefaultDirectory_LetsEncryptStaging
		};

		struct CExternalAccountBinding
		{
			NStr::CStr m_KeyID;
			NStr::CStrSecure m_HmacKey; // Base64url encoded, as issued by the CA
		};

		struct CAccountInfo
		{
			NContainer::CSecureByteVector m_AccountPrivateKey;
			NContainer::TCVector<NStr::CStr> m_Emails;
			NStorage::TCOptional<CExternalAccountBinding> m_ExternalAccountBinding; // Required by CAs that only issue to accounts known to them (RFC 8555 7.3.4)
		};

		struct CDependencies
		{
			CDependencies(EDefaultDirectory _Directory, NStr::CStr const &_CustomDirectory = {});

			NConcurrency::TCActor<CHttpClientActor> m_HttpClientActor;
			NHTTP::CURL m_DirectoryURL;
			NStr::CStr m_UserAgent = "Malterlib ACME client"; // ACME servers require a User-Agent (RFC 8555 6.1)

			CAccountInfo m_AccountInfo;
		};

		struct CChain
		{
			NStr::CStr m_FullChain;
			NStr::CStr m_EndEntity;
			NStr::CStr m_Issuer;
			NStr::CStr m_Root;
			NContainer::TCVector<NStr::CStr> m_Other;
		};

		struct CCertificateChains
		{
			NStr::CStrSecure m_PrivateKey;
			CChain m_DefaultChain;
			NContainer::TCVector<CChain> m_AlternateChains;
		};

		enum EChallengeType
		{
			EChallengeType_Http01
			, EChallengeType_Dns01
			, EChallengeType_TlsAlpn01
		};

		struct CChallenge
		{
			EChallengeType m_Type = EChallengeType_Http01;
			NStr::CStrSecure m_Token;
			NStr::CStr m_DomainName;
		};

		struct CCertificateRequest
		{
			NCryptography::CPublicKeySetting m_KeySettings;
			NContainer::TCVector<NStr::CStr> m_DnsNames;
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<bool> (CChallenge _Challenge)> m_fChallenge;
			fp32 m_Timeout = 60.0;
		};

		CAcmeClientActor(CDependencies &&_Dependencies);
		~CAcmeClientActor();

		NConcurrency::TCFuture<CCertificateChains> f_RequestCertificate(CCertificateRequest _RequestCertificate);

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
