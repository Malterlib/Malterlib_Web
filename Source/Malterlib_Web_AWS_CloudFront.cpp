// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_AWS_CloudFront.h"
#include "Malterlib_Web_AWS_Internal.h"

#include <Mib/XML/XML>
#include <Mib/Cryptography/RandomID>

namespace NMib::NWeb
{
	struct CAwsCloudFrontActor::CInternal
	{
		CInternal(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
			: m_CurlActor{_CurlActor}
			, m_Credentials{_Credentials}
		{
			m_Credentials.m_Region = "us-east-1";
		}

		CAwsCredentials m_Credentials;
		TCActor<CCurlActor> m_CurlActor;
	};

	CAwsCloudFrontActor::CAwsCloudFrontActor(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_CurlActor, _Credentials)}
	{
	}

	CAwsCloudFrontActor::~CAwsCloudFrontActor() = default;

	NConcurrency::TCContinuation<NStr::CStr> CAwsCloudFrontActor::f_CreateInvalidation(NStr::CStr const &_DistributionID, NContainer::TCVector<NStr::CStr> const &_Paths)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://cloudfront.amazonaws.com/2017-10-30/distribution/{}/invalidation"_f << _DistributionID};

		TCVector<uint8> Contents;
		{
			NXML::CXMLDocument PostDocument(false);
			//auto pRoot = PostDocument.f_GetRootNode();
			auto pBatch = PostDocument.f_CreateDefaultDocument("InvalidationBatch");
			PostDocument.f_SetAttribute(pBatch, "xmlns", "http://cloudfront.amazonaws.com/doc/2017-10-30/");

			PostDocument.f_SetText(PostDocument.f_CreateElement(pBatch, "CallerReference"), NCryptography::fg_RandomID());

			auto pPaths = PostDocument.f_CreateElement(pBatch, "Paths");
			auto pItems = PostDocument.f_CreateElement(pPaths, "Items");

			for (auto &Path : _Paths)
			{
				PostDocument.f_SetText(PostDocument.f_CreateElement(pItems, "Path"), Path);
			}

			PostDocument.f_SetText(PostDocument.f_CreateElement(pPaths, "Quantity"), "{}"_f << _Paths.f_GetLen());

			CStr ContentsStr = PostDocument.f_GetAsString(NXML::EXMLOutputDialect_Compact);
			DMibConOut("{}\n", ContentsStr);
			Contents.f_Insert((uint8 const *)ContentsStr.f_GetStr(), ContentsStr.f_GetLen());
		}

		TCContinuation<NStr::CStr> Continuation;

		TCMap<CStr, CStr> Headers = fg_SignAWSRequest(AWSUrl, Contents, CCurlActor::EMethod_POST, Internal.m_Credentials, {}, "cloudfront");

		Internal.m_CurlActor(&CCurlActor::f_Request, CCurlActor::EMethod_POST, AWSUrl.f_Encode(), Headers, Contents)
			> Continuation / [=](CCurlActor::CResult &&_Result)
			{
				if (_Result.m_StatusCode != 201)
					return fg_ReportAWSError(Continuation, _Result, "Create invalidation");

				NXML::CXMLDocument Results;
				if (!Results.f_ParseString(_Result.m_Body))
				{
					Continuation.f_SetException(DMibErrorInstance("Create invalidation request failed to parse result"));
					return;
				}

				auto fReportInvalidXML = [&](CStr const &_Entry)
					{
						Continuation.f_SetException(DMibErrorInstance("Create invalidation request failed to find a valid '{}' in XML"_f << _Entry));
					}
				;

				auto pInvalidation = Results.f_GetChildNode(Results.f_GetRootNode(), "Invalidation");
				if (!pInvalidation)
					return fReportInvalidXML("Invalidation");

				auto pID = Results.f_GetChildNode(pInvalidation, "Id");
				if (!pID)
					return fReportInvalidXML("Invalidation.Id");

				CStr ID = Results.f_GetNodeText(pID, "");
				if (ID.f_IsEmpty())
					return fReportInvalidXML("Invalidation.Id text");

				Continuation.f_SetResult(ID);
			}
		;

		return Continuation;
	}
}
