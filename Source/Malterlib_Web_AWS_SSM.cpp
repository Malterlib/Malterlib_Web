// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Web/Curl>
#include <Mib/Encoding/Json>

#include "Malterlib_Web_AWS_SSM.h"
#include "Malterlib_Web_AWS_Internal.h"

namespace NMib::NWeb
{
	using namespace NStr;
	using namespace NContainer;
	using namespace NConcurrency;
	using namespace NStorage;
	using namespace NEncoding;

	struct CAwsSsmActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
			: m_CurlActor{_CurlActor}
			, m_Credentials{_Credentials}
		{
		}

		CAwsCredentials m_Credentials;
		TCActor<CCurlActor> m_CurlActor;
	};

	CAwsSsmActor::CAwsSsmActor(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_CurlActor, _Credentials)}
	{
	}

	CAwsSsmActor::~CAwsSsmActor() = default;

	TCFuture<TCMap<CStr, CStr>> CAwsSsmActor::f_GetRegionLongNames(TCVector<CStr> _RegionCodes)
	{
		auto &Internal = *mp_pInternal;

		if (_RegionCodes.f_IsEmpty())
			co_return TCMap<CStr, CStr>{};

		// AWS limits GetParameters to 10 names per request
		constexpr static mint c_MaxParametersPerRequest = 10;

		// SSM public parameters are only accessible from us-east-1 or us-west-1
		NHTTP::CURL URL{"https://ssm.us-east-1.amazonaws.com/"};

		// Create credentials for us-east-1 region
		CAwsCredentials Credentials = Internal.m_Credentials;
		Credentials.m_Region = "us-east-1";

		// Build chunks of region codes
		TCVector<TCVector<CStr>> Chunks;
		for (mint i = 0; i < _RegionCodes.f_GetLen(); i += c_MaxParametersPerRequest)
		{
			TCVector<CStr> &Chunk = Chunks.f_InsertLast();
			mint nEnd = fg_Min(i + c_MaxParametersPerRequest, _RegionCodes.f_GetLen());
			for (mint j = i; j < nEnd; ++j)
				Chunk.f_InsertLast(_RegionCodes[j]);
		}

		// Make parallel requests for each chunk
		TCFutureVector<CJsonSorted> Futures;
		for (auto const &Chunk : Chunks)
		{
			CJsonSorted Request;
			auto &Names = Request["Names"];
			for (auto const &RegionCode : Chunk)
				Names.f_Insert("/aws/service/global-infrastructure/regions/{}/longName"_f << RegionCode);

			TCMap<CStr, CStr> AWSHeaders;
			AWSHeaders["Content-Type"] = "application/x-amz-json-1.1";
			AWSHeaders["X-Amz-Target"] = "AmazonSSM.GetParameters";

			fg_DoAWSRequestJson
				(
					"GetParameters"
					, Internal.m_CurlActor
					, 200
					, URL
					, Request
					, CCurlActor::EMethod_POST
					, Credentials
					, AWSHeaders
					, "ssm"
				)
				> Futures
			;
		}

		// Wait for all requests to complete
		TCVector<CJsonSorted> Results = co_await fg_AllDone(fg_Move(Futures));

		// Merge results
		TCMap<CStr, CStr> RegionLongNames;
		for (auto const &Result : Results)
		{
			auto *pParameters = Result.f_GetMember("Parameters", EJsonType_Array);
			if (!pParameters)
				continue;

			for (auto const &Parameter : pParameters->f_Array())
			{
				auto *pName = Parameter.f_GetMember("Name", EJsonType_String);
				auto *pValue = Parameter.f_GetMember("Value", EJsonType_String);
				if (!pName || !pValue)
					continue;

				// Extract region code from path: /aws/service/global-infrastructure/regions/{REGION}/longName
				CStr const &Name = pName->f_String();
				TCVector<CStr> Parts = Name.f_Split("/");
				if (Parts.f_GetLen() >= 6)
				{
					CStr const &RegionCode = Parts[5];
					RegionLongNames[RegionCode] = pValue->f_String();
				}
			}
		}

		co_return RegionLongNames;
	}
}
