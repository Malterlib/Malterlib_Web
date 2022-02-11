// Copyright © 2017 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>

namespace NMib::NWeb
{
	struct CCurlActor : public NConcurrency::CActor
	{
		struct CCertificateConfig
		{
			NContainer::CByteVector m_ClientCertificate;
			NContainer::CSecureByteVector m_ClientKey;
			NContainer::CByteVector m_CertificateAuthorities;
		};

		CCurlActor(CCertificateConfig const &_CertificateConfig = {});

		using CActorHolder = NConcurrency::CSeparateThreadActorHolder;

		enum EMethod
		{
			EMethod_GET
			, EMethod_HEAD
			, EMethod_POST
			, EMethod_PATCH
			, EMethod_PUT
			, EMethod_DELETE
		};
		
		struct CState;
		
		struct CResult
		{
			CResult(CState const &_State);
			
			uint32 m_StatusCode;
			NStr::CStr m_StatusMessage;
			NContainer::TCMap<NStr::CStr, NStr::CStr, NStr::CCompareNoCase> m_Headers;
			NStr::CStr m_Body;
			
			NEncoding::CEJSON f_ToJSON() const;
		};
		
		NConcurrency::TCFuture<CResult> f_Request
			(
				EMethod _Method
				, NStr::CStr const &_URL
				, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Headers
				, NContainer::CByteVector const &_Data
			 	, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Cookies
			)
		;

	private:
		CCertificateConfig mp_CertificateConfig;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
