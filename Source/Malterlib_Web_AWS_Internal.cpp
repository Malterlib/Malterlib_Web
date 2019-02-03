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
		CanonicalRequest += "{}\n"_f << _URL.f_GetFullPathPercentEncoded(true); // CanonicalURI

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
		StringToSign += "{}"_f << NCryptography::CHash_SHA256::fs_DigestFromData(CanonicalRequest.f_GetStr(), CanonicalRequest.f_GetLen()).f_GetString(); // CanonicalRequestHash;

		if (_bTrace)
			DMibConOut("StringToSign: {}\n", StringToSign);

		CStr Signature;
		{
			auto fHMAC = [](NCryptography::CHashDigest_SHA256 const &_Key, CStr const &_Data) -> NCryptography::CHashDigest_SHA256
				{
					NContainer::CSecureByteVector Key{_Key.f_GetData(), _Key.fs_GetSize()};
					return NNetwork::fg_MessageAuthenication_HMAC_SHA256(NContainer::CSecureByteVector((uint8 const *)_Data.f_GetStr(), _Data.f_GetLen()), Key);
				}
			;
			NCryptography::CHashDigest_SHA256 KeyDate;
			{
				CStrSecure SecretStr = "AWS4" + _Credentials.m_SecretKey;
				CStr Date ="{}{sf0,sj2}{sf0,sj2}"_f << CurrentDateTime.m_Year << CurrentDateTime.m_Month << CurrentDateTime.m_DayOfMonth;
				NContainer::CSecureByteVector Secret((uint8 const *)SecretStr.f_GetStr(), SecretStr.f_GetLen());
				NContainer::CSecureByteVector Data((uint8 const *)Date.f_GetStr(), Date.f_GetLen());
				KeyDate = NNetwork::fg_MessageAuthenication_HMAC_SHA256(Data, Secret);
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

	template <typename tf_CReturn>
	void fg_ReportAWSErrorXML(TCContinuation<tf_CReturn> const &_Continuation, CCurlActor::CResult &_Result, ch8 const *_pRequestDescription)
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
			_Continuation.f_SetException(DMibErrorInstanceAws("{} request failed with status {}: {} - {}"_f << _pRequestDescription << _Result.m_StatusCode << Code << Message, ErrorData));
			return;
		}
		while (false);

		CAwsErrorData ErrorData{"Unknown", _Result.m_StatusCode};
		_Continuation.f_SetException(DMibErrorInstanceAws("{} request failed with status {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << _Result.m_Body, ErrorData));
	}

	TCContinuation<NStorage::TCTuple<NXML::CXMLDocument, CCurlActor::CResult>> fg_DoAWSRequestXML
		(
		 	CStr const &_Description
		 	, TCActor<CCurlActor> const &_CurlActor
		 	, uint32 _ExpectedStatus
		 	, NHTTP::CURL const &_URL
		 	, NStorage::TCVariant<void, CByteVector, NXML::CXMLDocument> const &_Contents
		 	, CCurlActor::EMethod _Method
		 	, CAwsCredentials const &_Credentials
		 	, TCMap<CStr, CStr> const &_AWSHeaders
		 	, CStr const &_Service
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

		TCContinuation<NStorage::TCTuple<NXML::CXMLDocument, CCurlActor::CResult>> Continuation;

		_CurlActor(&CCurlActor::f_Request, _Method, _URL.f_Encode(), Headers, Contents, TCMap<CStr, CStr>{})
			> Continuation / [=](CCurlActor::CResult &&_Result)
			{
				if (_Result.m_StatusCode != _ExpectedStatus)
					return fg_ReportAWSErrorXML(Continuation, _Result, _Description);

				if (_Result.m_Body.f_IsEmpty())
					return Continuation.f_SetResult(fg_Tuple(NXML::CXMLDocument{}, fg_Move(_Result)));

				NXML::CXMLDocument Results;
				if (!Results.f_ParseString(_Result.m_Body))
				{
					CAwsErrorData ErrorData{"ResultParse", _Result.m_StatusCode};
					Continuation.f_SetException(DMibErrorInstanceAws("{} request failed to parse result"_f << _Description, ErrorData));
					return;
				}

				Continuation.f_SetResult(fg_Tuple(fg_Move(Results), fg_Move(_Result)));
			}
		;

		return Continuation;
	}

	template <typename tf_CReturn>
	void fg_ReportAWSErrorJSON(TCContinuation<tf_CReturn> const &_Continuation, CCurlActor::CResult &_Result, ch8 const *_pRequestDescription)
	{
		CJSON ErrorReturn;
		do
		{
			try
			{
				ErrorReturn = CJSON::fs_FromString(_Result.m_Body);
			}
			catch (NException::CException const &_Exception)
			{
				CAwsErrorData ErrorData{"ResultParse", _Result.m_StatusCode};
				return _Continuation.f_SetException
					(
					 	DMibErrorInstanceAws
					 	(
							"{} request failed with status {}. Failed to parse JSON: {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << _Exception << _Result.m_Body
						 	, ErrorData
						)
					)
				;
			}

			auto pCodeNode = ErrorReturn.f_GetMember("Code", EJSONType_String);
			auto pMessageNode = ErrorReturn.f_GetMember("Message", EJSONType_String);
			if (!pMessageNode)
				break;

			auto Message = pMessageNode->f_String();
			if (!pCodeNode)
			{
				CAwsErrorData ErrorData{"Unknown", _Result.m_StatusCode};
				_Continuation.f_SetException(DMibErrorInstanceAws("{} request failed with status {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << Message, ErrorData));
				return;
			}

			auto Code = pCodeNode->f_String();
			if (!Code || !Message)
				break;

			CAwsErrorData ErrorData{Code, _Result.m_StatusCode};
			_Continuation.f_SetException(DMibErrorInstanceAws("{} request failed with status {}: {} - {}"_f << _pRequestDescription << _Result.m_StatusCode << Code << Message, ErrorData));
			return;
		}
		while (false);

		CAwsErrorData ErrorData{"Unknown", _Result.m_StatusCode};
		_Continuation.f_SetException(DMibErrorInstanceAws("{} request failed with status {}: {}"_f << _pRequestDescription << _Result.m_StatusCode << _Result.m_Body, ErrorData));
	}

	TCContinuation<NEncoding::CJSON> fg_DoAWSRequestJSON
		(
		 	CStr const &_Description
		 	, TCActor<CCurlActor> const &_CurlActor
		 	, uint32 _ExpectedStatus
		 	, NHTTP::CURL const &_URL
		 	, NStorage::TCVariant<void, CByteVector, NEncoding::CJSON> const &_Contents
		 	, CCurlActor::EMethod _Method
		 	, CAwsCredentials const &_Credentials
		 	, TCMap<CStr, CStr> const &_AWSHeaders
		 	, CStr const &_Service
		 	, bool _bTrace
		)
	{
		CByteVector Contents;

		if (_Contents.f_IsOfType<CByteVector>())
			Contents = _Contents.f_GetAsType<CByteVector>();
		else if (_Contents.f_IsOfType<NEncoding::CJSON>())
		{
			CStr ContentsStr = _Contents.f_GetAsType<NEncoding::CJSON>().f_ToString(nullptr);
			Contents.f_Insert((uint8 const *)ContentsStr.f_GetStr(), ContentsStr.f_GetLen());
		}

		TCMap<CStr, CStr> AWSHeaders = _AWSHeaders;

		if (!AWSHeaders.f_FindEqual("Accept"))
			AWSHeaders["Accept"] = "application/json";

		if (!AWSHeaders.f_FindEqual("Content-Type"))
			AWSHeaders["Content-Type"] = "application/json";

		TCMap<CStr, CStr> Headers = fg_SignAWSRequest(_URL, Contents, _Method, _Credentials, AWSHeaders, _Service, _bTrace);

		TCContinuation<NEncoding::CJSON> Continuation;

		_CurlActor(&CCurlActor::f_Request, _Method, _URL.f_Encode(), Headers, Contents, TCMap<CStr, CStr>{})
			> Continuation / [=](CCurlActor::CResult &&_Result)
			{
				if (_Result.m_StatusCode != _ExpectedStatus)
					return fg_ReportAWSErrorJSON(Continuation, _Result, _Description);

				if (_Result.m_Body.f_IsEmpty())
					return Continuation.f_SetResult(fg_Default());

				try
				{
					Continuation.f_SetResult(NEncoding::CJSON::fs_FromString(_Result.m_Body));
				}
				catch (NException::CException const &_Exception)
				{
					CAwsErrorData ErrorData{"ErrorParse", _Result.m_StatusCode};
					Continuation.f_SetException(DMibErrorInstanceAws("{} request failed to parse result: {}"_f << _Description << _Exception, ErrorData));
				}
			}
		;

		return Continuation;
	}

	TCContinuation<NContainer::TCMap<NStr::CStr, NStr::CStr>> fg_DoAWSRequestHEAD
		(
		 	CStr const &_Description
		 	, TCActor<CCurlActor> const &_CurlActor
		 	, uint32 _ExpectedStatus
		 	, NHTTP::CURL const &_URL
		 	, CAwsCredentials const &_Credentials
		 	, TCMap<CStr, CStr> const &_AWSHeaders
		 	, CStr const &_Service
		 	, bool _bTrace
		)
	{
		TCMap<CStr, CStr> AWSHeaders = _AWSHeaders;
		TCMap<CStr, CStr> Headers = fg_SignAWSRequest(_URL, {}, CCurlActor::EMethod_HEAD, _Credentials, AWSHeaders, _Service, _bTrace);

		TCContinuation<NContainer::TCMap<NStr::CStr, NStr::CStr>> Continuation;

		_CurlActor(&CCurlActor::f_Request, CCurlActor::EMethod_HEAD, _URL.f_Encode(), Headers, NContainer::CByteVector{}, TCMap<CStr, CStr>{})
			> Continuation / [=](CCurlActor::CResult &&_Result)
			{
				if (_Result.m_StatusCode != _ExpectedStatus)
					return fg_ReportAWSErrorJSON(Continuation, _Result, _Description);

				return Continuation.f_SetResult(fg_Move(_Result.m_Headers));
			}
		;

		return Continuation;
	}
}
