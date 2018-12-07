// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSON>
#include <Mib/Web/Curl>
#include <Mib/Web/HTTP/URL>
#include <Mib/XML/XML>

#include "Malterlib_Web_AWS_Credentials.h"

namespace NMib::NWeb
{
	using namespace NEncoding;
	using namespace NContainer;
	using namespace NStr;
	using namespace NConcurrency;
	using namespace NStorage;

	struct CAwsErrorData
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		CStr m_ErrorCode;
		uint32 m_StatusCode = 0;
	};

	DMibImpErrorSpecificClass(CExceptionAws, NMib::NException::CException, CAwsErrorData);

#	define DMibErrorAws(d_Description, d_Specific) DMibImpErrorSpecific(NMib::NWeb::CExceptionAws, d_Description, d_Specific)
#	define DMibErrorInstanceAws(d_Description, d_Specific) DMibImpExceptionInstanceSpecific(NMib::NWeb::CExceptionAws, d_Description, d_Specific)

	ch8 const *fg_MethodToStr(CCurlActor::EMethod _Method);

	TCMap<CStr, CStr> fg_SignAWSRequest
		(
		 	NHTTP::CURL const &_URL
		 	, CByteVector const &_Contents
		 	, CCurlActor::EMethod _Method
		 	, CAwsCredentials const &_Credentials
		 	, TCMap<CStr, CStr> const &_AWSHeaders
		 	, CStr const &_Service
		 	, bool _bTrace = false
		)
	;

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
		 	, bool _bTrace = false
		)
	;

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
		 	, bool _bTrace = false
		)
	;

	TCContinuation<NContainer::TCMap<NStr::CStr, NStr::CStr>> fg_DoAWSRequestHEAD
		(
		 	CStr const &_Description
		 	, TCActor<CCurlActor> const &_CurlActor
		 	, uint32 _ExpectedStatus
		 	, NHTTP::CURL const &_URL
		 	, CAwsCredentials const &_Credentials
		 	, TCMap<CStr, CStr> const &_AWSHeaders
		 	, CStr const &_Service
		 	, bool _bTrace = false
		)
	;
}

#include "Malterlib_Web_AWS_Internal.hpp"
