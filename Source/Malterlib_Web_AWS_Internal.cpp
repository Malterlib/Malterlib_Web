// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_AWS_Internal.h"

#include <Mib/Encoding/JSON>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Web/Curl>
#include <Mib/Network/SSL>
#include <Mib/XML/XML>

namespace NMib::NWeb
{
	ch8 const *fg_MethodToStr(CCurlActor::EMethod _Method)
	{
		switch (_Method)
		{
			case CCurlActor::EMethod_GET: return "GET";
			case CCurlActor::EMethod_POST: return "POST";
			case CCurlActor::EMethod_PUT: return "PUT";
			case CCurlActor::EMethod_DELETE: return "DELETE";
		}
	}

	TCMap<CStr, CStr> fg_SignAWSRequest
		(
		 	NHTTP::CURL const &_URL
		 	, TCVector<uint8> const &_Contents
		 	, CCurlActor::EMethod _Method
		 	, CAwsCredentials const &_Credentials
		 	, TCMap<CStr, CStr> const &_AWSHeaders
		 	, CStr const &_Service // https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html#genref-aws-service-namespaces
		  	, bool _bTrace
		)
	{
		NTime::CTime CurrentTime = NTime::CTime::fs_NowUTC();
		auto CurrentDateTime = NTime::CTimeConvert{CurrentTime}.f_ExtractDateTime();

		auto PayloadHash = NDataProcessing::CHash_SHA256::fs_DigestFromData(_Contents).f_GetString();
		auto CurrentTimeISO8601 = fg_GetISO8601TimeStr(CurrentTime);
		CStr Scope = "{}{sf0,sj2}{sf0,sj2}/{}/{}/aws4_request"_f
			<< CurrentDateTime.m_Year
			<< CurrentDateTime.m_Month
			<< CurrentDateTime.m_DayOfMonth
			<< _Credentials.m_Region
			<< _Service
		;

		TCMap<CStr, CStr> AWSHeaders = _AWSHeaders;
		AWSHeaders["x-amz-date"] = CurrentTimeISO8601;
		AWSHeaders["Host"] = _URL.f_GetHost();
		AWSHeaders["x-amz-content-sha256"] = PayloadHash;
		if (!AWSHeaders.f_FindEqual("Accept"))
			AWSHeaders["Accept"] = "application/xml";
		if (!AWSHeaders.f_FindEqual("Content-Type"))
			AWSHeaders["Content-Type"] = "application/xml";

		CStr CanonicalRequest = "{}\n"_f << fg_MethodToStr(_Method); // HTTPRequestMethod
		CanonicalRequest += "{}\n"_f << _URL.f_GetFullPath(); // CanonicalURI

		// CanonicalQueryString
		{
			TCSet<NHTTP::CURL::CQueryEntry> QueryParams;
			for (auto &QueryParam : _URL.f_GetQuery())
				QueryParams[QueryParam];

			CStr CannonicalQuery;
			for (auto &QueryParam : QueryParams)
			{
				CStr ParamName;
				CStr ParamValue;

				NHTTP::CURL::fs_PercentEncode(ParamName, QueryParam.m_Key, nullptr, true);
				NHTTP::CURL::fs_PercentEncode(ParamValue, QueryParam.m_Value, nullptr, true);

				fg_AddStrSep(CannonicalQuery, "{}={}"_f << ParamName << ParamValue, "&");
			}

			CanonicalRequest += "{}\n"_f << CannonicalQuery;
		}

		// CanonicalHeaders, SignedHeaders
		CStr SignedHeaders;
		{
			TCMap<CStr, CStr> CanonicalHeaders;
			for (auto &Value : AWSHeaders)
				CanonicalHeaders[AWSHeaders.fs_GetKey(Value).f_LowerCase()] = Value;

			auto fTrimHeader = [](CStr const &_Header)
				{
					CStr Trimmed = _Header.f_Trim();
					CStr WithoutConsecutiveSpace;

					for (auto *pParse = Trimmed.f_GetStr(); *pParse;)
					{
						if (*pParse == ' ')
						{
							while (*pParse == ' ')
								++pParse;
							WithoutConsecutiveSpace.f_AddChar(' ');
							continue;
						}

						WithoutConsecutiveSpace.f_AddChar(*pParse);
						++pParse;
					}

					return WithoutConsecutiveSpace;
				}
			;

			for (auto &Header : CanonicalHeaders)
			{
				auto &HeaderName = CanonicalHeaders.fs_GetKey(Header);
				CanonicalRequest += "{}:{}\n"_f << HeaderName << fTrimHeader(Header);
				fg_AddStrSep(SignedHeaders, HeaderName, ";");
			}
			CanonicalRequest += "\n";
			CanonicalRequest += "{}\n"_f << SignedHeaders;
		}

		CanonicalRequest += "{}"_f << PayloadHash; // HashedPayload

		if (_bTrace)
			DMibConOut("CanonicalRequest: {}\n", CanonicalRequest);

		CStr StringToSign = "AWS4-HMAC-SHA256\n";

		StringToSign += "{}\n"_f << CurrentTimeISO8601; // Timstamp
		StringToSign += "{}\n"_f << Scope; // Scope
		StringToSign += "{}"_f << NDataProcessing::CHash_SHA256::fs_DigestFromData(CanonicalRequest.f_GetStr(), CanonicalRequest.f_GetLen()).f_GetString(); // CanonicalRequestHash;

		if (_bTrace)
			DMibConOut("StringToSign: {}\n", StringToSign);

		CStr Signature;
		{
			auto fHMAC = [](NDataProcessing::CHashDigest_SHA256 const &_Key, CStr const &_Data) -> NDataProcessing::CHashDigest_SHA256
				{
					NContainer::CSecureByteVector Key{_Key.f_GetData(), _Key.fs_GetSize()};
					return NNet::fg_MessageAuthenication_HMAC_SHA256(NContainer::CSecureByteVector((uint8 const *)_Data.f_GetStr(), _Data.f_GetLen()), Key);
				}
			;
			NDataProcessing::CHashDigest_SHA256 KeyDate;
			{
				CStrSecure SecretStr = "AWS4" + _Credentials.m_SecretKey;
				CStr Date ="{}{sf0,sj2}{sf0,sj2}"_f << CurrentDateTime.m_Year << CurrentDateTime.m_Month << CurrentDateTime.m_DayOfMonth;
				NContainer::CSecureByteVector Secret((uint8 const *)SecretStr.f_GetStr(), SecretStr.f_GetLen());
				NContainer::CSecureByteVector Data((uint8 const *)Date.f_GetStr(), Date.f_GetLen());
				KeyDate = NNet::fg_MessageAuthenication_HMAC_SHA256(Data, Secret);
			}

			auto KeyRegion = fHMAC(KeyDate, _Credentials.m_Region);
			auto KeyService = fHMAC(KeyRegion, _Service);
			auto KeySigning = fHMAC(KeyService, "aws4_request");

			Signature = fHMAC(KeySigning, StringToSign).f_GetString();
		}

		TCMap<CStr, CStr> Headers = AWSHeaders;
		Headers["Authorization"] = "AWS4-HMAC-SHA256 Credential={}/{}, SignedHeaders={}, Signature={}"_f
			<< _Credentials.m_AccessKeyID
			<< Scope
			<< SignedHeaders
			<< Signature
		;

		return Headers;
	}
}
