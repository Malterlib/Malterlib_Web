// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_AWS_Internal.h"

#include <Mib/Encoding/Json>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Web/Curl>
#include <Mib/Cryptography/MessageAuthentication>
#include <Mib/XML/XML>

namespace NMib::NWeb
{
	DMibImpErrorClassImplement(CExceptionAws);

	ch8 const *fg_MethodToStr(CCurlActor::EMethod _Method)
	{
		switch (_Method)
		{
			case CCurlActor::EMethod_GET: return "GET";
			case CCurlActor::EMethod_HEAD: return "HEAD";
			case CCurlActor::EMethod_POST: return "POST";
			case CCurlActor::EMethod_PUT: return "PUT";
			case CCurlActor::EMethod_DELETE: return "DELETE";
			case CCurlActor::EMethod_PATCH: return "PATCH";
		}

		return "";
	}

	TCMap<CStr, CStr> fg_SignAWSRequest
		(
			NHTTP::CURL const &_URL
			, CByteVector const &_Contents
			, CCurlActor::EMethod _Method
			, CAwsCredentials const &_Credentials
			, TCMap<CStr, CStr> const &_AWSHeaders
			, CStr const &_Service // https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html#genref-aws-service-namespaces
			, bool _bTrace
		)
	{
		NTime::CTime CurrentTime = NTime::CTime::fs_NowUTC();
		auto CurrentDateTime = NTime::CTimeConvert{CurrentTime}.f_ExtractDateTime();

		auto PayloadHash = NCryptography::CHash_SHA256::fs_DigestFromData(_Contents).f_GetString();
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

		{
			auto EncodeFlags = NHTTP::EEncodeFlag_UpperCasePercentEncode;
			if (_Service != "s3")
				EncodeFlags |= NHTTP::EEncodeFlag_DoublePercentEncode;
			CanonicalRequest += "{}\n"_f << _URL.f_GetFullPathPercentEncoded(EncodeFlags); // CanonicalURI
		}

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

				NHTTP::CURL::fs_PercentEncode(ParamName, QueryParam.m_Key, nullptr, NHTTP::EEncodeFlag_UpperCasePercentEncode);
				NHTTP::CURL::fs_PercentEncode(ParamValue, QueryParam.m_Value, nullptr, NHTTP::EEncodeFlag_UpperCasePercentEncode);

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
		StringToSign += "{}"_f << NCryptography::CHash_SHA256::fs_DigestFromData(CanonicalRequest.f_GetStr(), CanonicalRequest.f_GetLen()).f_GetString(); // CanonicalRequestHash;

		if (_bTrace)
			DMibConOut("StringToSign: {}\n", StringToSign);

		CStr Signature;
		{
			auto fHMAC = [](NCryptography::CHashDigest_SHA256 const &_Key, CStr const &_Data) -> NCryptography::CHashDigest_SHA256
				{
					NContainer::CSecureByteVector Key{_Key.f_GetData(), _Key.mc_Size};
					return NCryptography::fg_MessageAuthenication_HMAC_SHA256(NContainer::CSecureByteVector((uint8 const *)_Data.f_GetStr(), _Data.f_GetLen()), Key);
				}
			;
			NCryptography::CHashDigest_SHA256 KeyDate;
			{
				CStrSecure SecretStr = "AWS4" + _Credentials.m_SecretKey;
				CStr Date ="{}{sf0,sj2}{sf0,sj2}"_f << CurrentDateTime.m_Year << CurrentDateTime.m_Month << CurrentDateTime.m_DayOfMonth;
				NContainer::CSecureByteVector Secret((uint8 const *)SecretStr.f_GetStr(), SecretStr.f_GetLen());
				NContainer::CSecureByteVector Data((uint8 const *)Date.f_GetStr(), Date.f_GetLen());
				KeyDate = NCryptography::fg_MessageAuthenication_HMAC_SHA256(Data, Secret);
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

	NException::CExceptionPointer fg_ReportAWSErrorXML(CCurlActor::CResult &_Result, ch8 const *_pRequestDescription)
	{
		NXML::CXMLDocument ErrorReturn;
		do
		{
			if (!ErrorReturn.f_ParseString(_Result.m_Body))
				break;

			auto pErrorRoot = ErrorReturn.f_GetChildNode(ErrorReturn.f_GetRootNode(), "ErrorResponse");

			if (!pErrorRoot)
				pErrorRoot = ErrorReturn.f_GetRootNode();

			auto pErrorNode = ErrorReturn.f_GetChildNode(pErrorRoot, "Error");

			if (!pErrorNode)
				break;

			auto pCodeNode = ErrorReturn.f_GetChildNode(pErrorNode, "Code");
			auto pMessageNode = ErrorReturn.f_GetChildNode(pErrorNode, "Message");
			if (!pCodeNode || !pMessageNode)
				break;

			auto Code = ErrorReturn.f_GetNodeText(pCodeNode);
			auto Message = ErrorReturn.f_GetNodeText(pMessageNode);
			if (!Code || !Message)
				break;

			CAwsErrorData ErrorData{Code, _Result.m_StatusCode};
			return DMibErrorInstanceAws("{} request failed with status {}: {} - {}"_f << _pRequestDescription << _Result.m_StatusCode << Code << Message, ErrorData);
		}
		while (false);

		CAwsErrorData ErrorData{"Unknown", _Result.m_StatusCode};
		return DMibErrorInstanceAws("{} request failed with status {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << _Result.m_Body, ErrorData);
	}

	TCFuture<NStorage::TCTuple<NXML::CXMLDocument, CCurlActor::CResult>> fg_DoAWSRequestXML
		(
			CStr _Description
			, TCActor<CCurlActor> _CurlActor
			, uint32 _ExpectedStatus
			, NHTTP::CURL _URL
			, NStorage::TCVariant<void, CByteVector, NXML::CXMLDocument> _Contents
			, CCurlActor::EMethod _Method
			, CAwsCredentials _Credentials
			, TCMap<CStr, CStr> _AWSHeaders
			, CStr _Service
			, bool _bTrace
		)
	{
		CByteVector Contents;

		if (_Contents.f_IsOfType<CByteVector>())
			Contents = _Contents.f_GetAsType<CByteVector>();
		else if (_Contents.f_IsOfType<NXML::CXMLDocument>())
		{
			CStr ContentsStr = _Contents.f_GetAsType<NXML::CXMLDocument>().f_GetAsString(NXML::EXMLOutputDialect_Compact);
			Contents.f_Insert((uint8 const *)ContentsStr.f_GetStr(), ContentsStr.f_GetLen());
		}

		TCMap<CStr, CStr> Headers = fg_SignAWSRequest(_URL, Contents, _Method, _Credentials, _AWSHeaders, _Service, _bTrace);

		auto Result = co_await _CurlActor(&CCurlActor::f_Request, _Method, _URL.f_Encode(NHTTP::EEncodeFlag_UpperCasePercentEncode), Headers, Contents, TCMap<CStr, CStr>{});
		if (Result.m_StatusCode != _ExpectedStatus)
			co_return fg_ReportAWSErrorXML(Result, _Description);

		if (Result.m_Body.f_IsEmpty())
			co_return fg_Tuple(NXML::CXMLDocument{}, fg_Move(Result));

		NXML::CXMLDocument Results;
		if (!Results.f_ParseString(Result.m_Body))
		{
			CAwsErrorData ErrorData{"ResultParse", Result.m_StatusCode};
			co_return DMibErrorInstanceAws("{} request failed to parse result"_f << _Description, ErrorData);
		}

		co_return fg_Tuple(fg_Move(Results), fg_Move(Result));
	}

	NException::CExceptionPointer fg_ReportAWSErrorJson(CCurlActor::CResult &_Result, ch8 const *_pRequestDescription)
	{
		CJsonSorted ErrorReturn;
		do
		{
			try
			{
				ErrorReturn = CJsonSorted::fs_FromString(_Result.m_Body);
			}
			catch (NException::CException const &_Exception)
			{
				CAwsErrorData ErrorData{"ResultParse", _Result.m_StatusCode};
				return DMibErrorInstanceAws
					(
						"{} request failed with status {}. Failed to parse Json: {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << _Exception << _Result.m_Body
						, ErrorData
					)
				;
			}

			auto pCodeNode = ErrorReturn.f_GetMember("Code", EJsonType_String);
			auto pMessageNode = ErrorReturn.f_GetMember("Message", EJsonType_String);
			if (!pMessageNode)
				break;

			auto Message = pMessageNode->f_String();
			if (!pCodeNode)
			{
				CAwsErrorData ErrorData{"Unknown", _Result.m_StatusCode};
				return DMibErrorInstanceAws("{} request failed with status {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << Message, ErrorData);
			}

			auto Code = pCodeNode->f_String();
			if (!Code || !Message)
				break;

			CAwsErrorData ErrorData{Code, _Result.m_StatusCode};
			return DMibErrorInstanceAws("{} request failed with status {}: {} - {}"_f << _pRequestDescription << _Result.m_StatusCode << Code << Message, ErrorData);
		}
		while (false);

		CAwsErrorData ErrorData{"Unknown", _Result.m_StatusCode};
		return DMibErrorInstanceAws("{} request failed with status {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << _Result.m_Body, ErrorData);
	}

	TCFuture<NEncoding::CJsonSorted> fg_DoAWSRequestJson
		(
			CStr _Description
			, TCActor<CCurlActor> _CurlActor
			, uint32 _ExpectedStatus
			, NHTTP::CURL _URL
			, NStorage::TCVariant<void, CByteVector, NEncoding::CJsonSorted> _Contents
			, CCurlActor::EMethod _Method
			, CAwsCredentials _Credentials
			, TCMap<CStr, CStr> _AWSHeaders
			, CStr _Service
			, bool _bTrace
		)
	{
		CByteVector Contents;

		if (_Contents.f_IsOfType<CByteVector>())
			Contents = _Contents.f_GetAsType<CByteVector>();
		else if (_Contents.f_IsOfType<NEncoding::CJsonSorted>())
		{
			CStr ContentsStr = _Contents.f_GetAsType<NEncoding::CJsonSorted>().f_ToString(nullptr);
			Contents.f_Insert((uint8 const *)ContentsStr.f_GetStr(), ContentsStr.f_GetLen());
		}

		TCMap<CStr, CStr> AWSHeaders = _AWSHeaders;

		if (!AWSHeaders.f_FindEqual("Accept"))
			AWSHeaders["Accept"] = "application/json";

		if (!AWSHeaders.f_FindEqual("Content-Type"))
			AWSHeaders["Content-Type"] = "application/json";

		TCMap<CStr, CStr> Headers = fg_SignAWSRequest(_URL, Contents, _Method, _Credentials, AWSHeaders, _Service, _bTrace);

		auto Result = co_await _CurlActor(&CCurlActor::f_Request, _Method, _URL.f_Encode(NHTTP::EEncodeFlag_UpperCasePercentEncode), Headers, Contents, TCMap<CStr, CStr>{});

		if (Result.m_StatusCode != _ExpectedStatus)
			co_return fg_ReportAWSErrorJson(Result, _Description);

		if (Result.m_Body.f_IsEmpty())
			co_return {};

		try
		{
			co_return NEncoding::CJsonSorted::fs_FromString(Result.m_Body);
		}
		catch (NException::CException const &_Exception)
		{
			CAwsErrorData ErrorData{"ErrorParse", Result.m_StatusCode};
			co_return DMibErrorInstanceAws("{} request failed to parse result: {}"_f << _Description << _Exception, ErrorData);
		}
	}

	TCFuture<NContainer::TCMap<NStr::CStr, NStr::CStr>> fg_DoAWSRequestHEAD
		(
			CStr _Description
			, TCActor<CCurlActor> _CurlActor
			, uint32 _ExpectedStatus
			, NHTTP::CURL _URL
			, CAwsCredentials _Credentials
			, TCMap<CStr, CStr> _AWSHeaders
			, CStr _Service
			, bool _bTrace
		)
	{
		TCMap<CStr, CStr> AWSHeaders = _AWSHeaders;
		TCMap<CStr, CStr> Headers = fg_SignAWSRequest(_URL, {}, CCurlActor::EMethod_HEAD, _Credentials, AWSHeaders, _Service, _bTrace);

		auto Result = co_await _CurlActor
			(
				&CCurlActor::f_Request
				, CCurlActor::EMethod_HEAD
				, _URL.f_Encode(NHTTP::EEncodeFlag_UpperCasePercentEncode)
				, Headers
				, NContainer::CByteVector{}
				, TCMap<CStr, CStr>{}
			)
		;

		if (Result.m_StatusCode != _ExpectedStatus)
			co_return fg_ReportAWSErrorJson(Result, _Description);

		co_return fg_Move(Result.m_Headers);
	}
}
