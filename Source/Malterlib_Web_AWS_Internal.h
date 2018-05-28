// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSON>
#include <Mib/Web/Curl>
#include <Mib/Web/HTTP/URL>

#include "Malterlib_Web_AWS_Credentials.h"

namespace NMib::NWeb
{
	using namespace NEncoding;
	using namespace NContainer;
	using namespace NStr;
	using namespace NConcurrency;

	ch8 const *fg_MethodToStr(CCurlActor::EMethod _Method);

	TCMap<CStr, CStr> fg_SignAWSRequest
		(
		 	NHTTP::CURL const &_URL
		 	, TCVector<uint8> const &_Contents
		 	, CCurlActor::EMethod _Method
		 	, CAwsCredentials const &_Credentials
		 	, TCMap<CStr, CStr> const &_AWSHeaders
		 	, CStr const &_Service
		 	, bool _bTrace = false
		)
	;

	template <typename tf_CReturn>
	void fg_ReportAWSError(TCContinuation<tf_CReturn> const &_Continuation, CCurlActor::CResult &_Result, ch8 const *_pRequestDescription);
}

#include "Malterlib_Web_AWS_Internal.hpp"
