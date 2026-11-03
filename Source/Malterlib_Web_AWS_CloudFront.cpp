// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Web_AWS_CloudFront.h"
#include "Malterlib_Web_AWS_Internal.h"

#include <Mib/XML/XML>
#include <Mib/Cryptography/RandomID>

namespace NMib::NWeb
{
	struct CAwsCloudFrontActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials)
			: m_HttpClientActor{_HttpClientActor}
			, m_Credentials{_Credentials}
		{
			m_Credentials.m_Region = "us-east-1";
		}

		CAwsCredentials m_Credentials;
		TCActor<CHttpClientActor> m_HttpClientActor;
	};

	CAwsCloudFrontActor::CAwsCloudFrontActor(TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_HttpClientActor, _Credentials)}
	{
	}

	CAwsCloudFrontActor::~CAwsCloudFrontActor() = default;

	NConcurrency::TCFuture<NStr::CStr> CAwsCloudFrontActor::f_CreateInvalidation(NStr::CStr _DistributionID, NContainer::TCVector<NStr::CStr> _Paths)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://cloudfront.amazonaws.com/2017-10-30/distribution/{}/invalidation"_f << _DistributionID};

		NXML::CXMLDocument PostDocument(false);
		{
			//auto pRoot = PostDocument.f_GetRootNode();
			auto pBatch = PostDocument.f_CreateDefaultDocument("InvalidationBatch");
			PostDocument.fs_SetAttribute(pBatch, "xmlns", "http://cloudfront.amazonaws.com/doc/2017-10-30/");

			PostDocument.fs_SetText(PostDocument.fs_CreateElement(pBatch, "CallerReference"), NCryptography::fg_RandomID());

			auto pPaths = PostDocument.fs_CreateElement(pBatch, "Paths");
			auto pItems = PostDocument.fs_CreateElement(pPaths, "Items");

			for (auto &Path : _Paths)
				PostDocument.fs_SetText(PostDocument.fs_CreateElement(pItems, "Path"), Path);

			PostDocument.fs_SetText(PostDocument.fs_CreateElement(pPaths, "Quantity"), "{}"_f << _Paths.f_GetLen());
		}

		auto Result = co_await fg_DoAWSRequestXML
			(
				"Create invalidation"
				, Internal.m_HttpClientActor
				, 201
				, AWSUrl
				, fg_Move(PostDocument)
				, CHttpClientActor::EMethod_POST
				, Internal.m_Credentials
				, {}
				, "cloudfront"
			)
		;

		auto &[Results, CurlResult] = Result;

		auto fReportInvalidXML = [&](CStr const &_Entry)
			{
				return DMibErrorInstance("Create invalidation request failed to find a valid '{}' in XML"_f << _Entry);
			}
		;

		auto pInvalidation = Results.fs_GetChildNode(Results.f_GetRootNode(), "Invalidation");
		if (!pInvalidation)
			co_return fReportInvalidXML("Invalidation");

		auto pID = Results.fs_GetChildNode(pInvalidation, "Id");
		if (!pID)
			co_return fReportInvalidXML("Invalidation.Id");

		CStr ID = Results.fs_GetNodeText(pID, "");
		if (ID.f_IsEmpty())
			co_return fReportInvalidXML("Invalidation.Id text");

		co_return ID;
	}

	NConcurrency::TCFuture<void> CAwsCloudFrontActor::f_UpdateDistributionLambdaFunctions(NStr::CStr _DistributionID, NContainer::TCMap<EFunctionEventType, NStr::CStr> _FunctionAssociations)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://cloudfront.amazonaws.com/2020-05-31/distribution/{}/config"_f << _DistributionID};

		auto Result = co_await fg_DoAWSRequestXML("Get distribution", Internal.m_HttpClientActor, 200, AWSUrl, {}, CHttpClientActor::EMethod_GET, Internal.m_Credentials, {}, "cloudfront");

		auto &Results = fg_Get<0>(Result);
		auto &CurlResult = fg_Get<1>(Result);

		auto fReportInvalidXML = [&](CStr const &_Entry)
			{
				return DMibErrorInstance("Update distribution lamba functions failed to find a valid '{}' in XML"_f << _Entry);
			}
		;

		auto pDistributionConfig = Results.fs_GetChildNode(Results.f_GetRootNode(), "DistributionConfig");
		if (!pDistributionConfig)
			co_return fReportInvalidXML("DistributionConfig");

		bool bNeedUpdate = false;

		auto fUpdateCacheBehavior = [&](NXML::CXMLNode *_pBehaviour)
			{
				auto pAssociations = Results.fs_GetChildNode(_pBehaviour, "LambdaFunctionAssociations");
				if (!pAssociations)
					pAssociations = Results.fs_CreateElement(_pBehaviour, "LambdaFunctionAssociations");

				auto pAssociationsItems = Results.fs_GetChildNode(pAssociations, "Items");
				if (!pAssociationsItems)
					pAssociationsItems = Results.fs_CreateElement(pAssociations, "Items");

				TCMap<CStr, CStr> AssociationsToUpdate;

				for (auto &Function : _FunctionAssociations)
				{
					CStr AssociationType;
					switch (_FunctionAssociations.fs_GetKey(Function))
					{
					case EFunctionEventType_ViewerRequest: AssociationType = "viewer-request"; break;
					case EFunctionEventType_ViewerResponse: AssociationType = "viewer-response"; break;
					case EFunctionEventType_OriginRequest: AssociationType = "origin-request"; break;
					case EFunctionEventType_OriginResponse: AssociationType = "origin-response"; break;
					}
					AssociationsToUpdate[AssociationType] = Function;
				}

				int64 nAssociations = 0;
				auto pQuantity = Results.fs_GetChildNode(pAssociations, "Quantity");
				if (!pQuantity)
					pQuantity = Results.fs_CreateElement(pAssociations, "Quantity");
				else
					nAssociations = Results.fs_GetNodeText(pQuantity, "0").f_ToInt(0);

				bool bChanged = false;

				for (NXML::CXMLDocument::CNodeIterator iAssociationItem(pAssociationsItems); iAssociationItem; ++iAssociationItem)
				{
					auto pEventType = Results.fs_GetChildNode(iAssociationItem, "EventType");
					if (!pEventType)
						continue;

					CStr Type = Results.fs_GetNodeText(pEventType);

					auto pFunction = AssociationsToUpdate.f_FindEqual(Type);
					if (!pFunction)
						continue;

					auto pLambdaARN = Results.fs_GetChildNode(iAssociationItem, "LambdaFunctionARN");
					if (pLambdaARN)
					{
						if (Results.fs_GetNodeText(pLambdaARN) == *pFunction)
						{
							AssociationsToUpdate.f_Remove(Type);
							continue;
						}
					}
					else
						pLambdaARN = Results.fs_CreateElement(iAssociationItem, "LambdaFunctionARN");

					bChanged = true;
					Results.fs_SetText(pLambdaARN, *pFunction);
					AssociationsToUpdate.f_Remove(Type);
				}

				for (auto &Function : AssociationsToUpdate)
				{
					auto pAssociationItem = Results.fs_CreateElement(pAssociationsItems, "LambdaFunctionAssociation");
					Results.fs_SetText(Results.fs_CreateElement(pAssociationItem, "EventType"), AssociationsToUpdate.fs_GetKey(Function));
					Results.fs_SetText(Results.fs_CreateElement(pAssociationItem, "LambdaFunctionARN"), Function);
					++nAssociations;
					bChanged = true;
				}

				if (bChanged)
				{
					bNeedUpdate = true;
					Results.fs_SetText(pQuantity, CStr::fs_ToStr(nAssociations));
				}
			}
		;

		if (auto pBehavior = Results.fs_GetChildNode(pDistributionConfig, "DefaultCacheBehavior"))
			fUpdateCacheBehavior(pBehavior);

		if (auto pCacheBehaviors = Results.fs_GetChildNode(pDistributionConfig, "CacheBehaviors"))
		{
			if (auto pCacheBehaviorsItems = Results.fs_GetChildNode(pCacheBehaviors, "Items"))
			{
				for (NXML::CXMLDocument::CNodeIterator iBehaviorItem(pCacheBehaviorsItems); iBehaviorItem; ++iBehaviorItem)
				{
					if (Results.fs_GetValue(iBehaviorItem) != "CacheBehavior")
						continue;

					fUpdateCacheBehavior(iBehaviorItem);
				}
			}
		}

		if (!bNeedUpdate)
			co_return {};

		TCMap<CStr, CStr> Headers;
		if (auto pHeader = CurlResult.m_Headers.f_FindEqual("etag"))
			Headers["If-Match"] = *pHeader;

		co_await fg_DoAWSRequestXML
			(
				"Update distribution"
				, Internal.m_HttpClientActor
				, 200
				, AWSUrl
				, fg_Move(Results)
				, CHttpClientActor::EMethod_PUT
				, Internal.m_Credentials
				, Headers
				, "cloudfront"
			)
		;

		co_return {};
	}
}
