// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_AWS_CloudFront.h"
#include "Malterlib_Web_AWS_Internal.h"

#include <Mib/XML/XML>
#include <Mib/Cryptography/RandomID>

namespace NMib::NWeb
{
	struct CAwsCloudFrontActor::CInternal : public NConcurrency::CActorInternal
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

	NConcurrency::TCFuture<NStr::CStr> CAwsCloudFrontActor::f_CreateInvalidation(NStr::CStr _DistributionID, NContainer::TCVector<NStr::CStr> _Paths)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://cloudfront.amazonaws.com/2017-10-30/distribution/{}/invalidation"_f << _DistributionID};

		NXML::CXMLDocument PostDocument(false);
		{
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
		}

		auto Result = co_await fg_DoAWSRequestXML
			(
				"Create invalidation"
				, Internal.m_CurlActor
				, 201
				, AWSUrl
				, fg_Move(PostDocument)
				, CCurlActor::EMethod_POST
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

		auto pInvalidation = Results.f_GetChildNode(Results.f_GetRootNode(), "Invalidation");
		if (!pInvalidation)
			co_return fReportInvalidXML("Invalidation");

		auto pID = Results.f_GetChildNode(pInvalidation, "Id");
		if (!pID)
			co_return fReportInvalidXML("Invalidation.Id");

		CStr ID = Results.f_GetNodeText(pID, "");
		if (ID.f_IsEmpty())
			co_return fReportInvalidXML("Invalidation.Id text");

		co_return ID;
	}

	NConcurrency::TCFuture<void> CAwsCloudFrontActor::f_UpdateDistributionLambdaFunctions
		(
			NStr::CStr _DistributionID
			, NContainer::TCMap<EFunctionEventType, NStr::CStr> _FunctionAssociations
		)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://cloudfront.amazonaws.com/2020-05-31/distribution/{}/config"_f << _DistributionID};

		auto Result = co_await fg_DoAWSRequestXML("Get distribution", Internal.m_CurlActor, 200, AWSUrl, {}, CCurlActor::EMethod_GET, Internal.m_Credentials, {}, "cloudfront");

		auto &Results = fg_Get<0>(Result);
		auto &CurlResult = fg_Get<1>(Result);

		auto fReportInvalidXML = [&](CStr const &_Entry)
			{
				return DMibErrorInstance("Update distribution lamba functions failed to find a valid '{}' in XML"_f << _Entry);
			}
		;

		auto pDistributionConfig = Results.f_GetChildNode(Results.f_GetRootNode(), "DistributionConfig");
		if (!pDistributionConfig)
			co_return fReportInvalidXML("DistributionConfig");

		bool bNeedUpdate = false;

		auto fUpdateCacheBehavior = [&](NXML::CXMLNode *_pBehaviour)
			{
				auto pAssociations = Results.f_GetChildNode(_pBehaviour, "LambdaFunctionAssociations");
				if (!pAssociations)
					pAssociations = Results.f_CreateElement(_pBehaviour, "LambdaFunctionAssociations");

				auto pAssociationsItems = Results.f_GetChildNode(pAssociations, "Items");
				if (!pAssociationsItems)
					pAssociationsItems = Results.f_CreateElement(pAssociations, "Items");

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
				auto pQuantity = Results.f_GetChildNode(pAssociations, "Quantity");
				if (!pQuantity)
					pQuantity = Results.f_CreateElement(pAssociations, "Quantity");
				else
					nAssociations = Results.f_GetNodeText(pQuantity, "0").f_ToInt(0);

				bool bChanged = false;

				for (NXML::CXMLDocument::CNodeIterator iAssociationItem(pAssociationsItems); iAssociationItem; ++iAssociationItem)
				{
					auto pEventType = Results.f_GetChildNode(iAssociationItem, "EventType");
					if (!pEventType)
						continue;

					CStr Type = Results.f_GetNodeText(pEventType);

					auto pFunction = AssociationsToUpdate.f_FindEqual(Type);
					if (!pFunction)
						continue;

					auto pLambdaARN = Results.f_GetChildNode(iAssociationItem, "LambdaFunctionARN");
					if (pLambdaARN)
					{
						if (Results.f_GetNodeText(pLambdaARN) == *pFunction)
						{
							AssociationsToUpdate.f_Remove(Type);
							continue;
						}
					}
					else
						pLambdaARN = Results.f_CreateElement(iAssociationItem, "LambdaFunctionARN");

					bChanged = true;
					Results.f_SetText(pLambdaARN, *pFunction);
					AssociationsToUpdate.f_Remove(Type);
				}

				for (auto &Function : AssociationsToUpdate)
				{
					auto pAssociationItem = Results.f_CreateElement(pAssociationsItems, "LambdaFunctionAssociation");
					Results.f_SetText(Results.f_CreateElement(pAssociationItem, "EventType"), AssociationsToUpdate.fs_GetKey(Function));
					Results.f_SetText(Results.f_CreateElement(pAssociationItem, "LambdaFunctionARN"), Function);
					++nAssociations;
					bChanged = true;
				}

				if (bChanged)
				{
					bNeedUpdate = true;
					Results.f_SetText(pQuantity, CStr::fs_ToStr(nAssociations));
				}
			}
		;


		if (auto pBehavior = Results.f_GetChildNode(pDistributionConfig, "DefaultCacheBehavior"))
			fUpdateCacheBehavior(pBehavior);

		if (auto pCacheBehaviors = Results.f_GetChildNode(pDistributionConfig, "CacheBehaviors"))
		{
			if (auto pCacheBehaviorsItems = Results.f_GetChildNode(pCacheBehaviors, "Items"))
			{
				for (NXML::CXMLDocument::CNodeIterator iBehaviorItem(pCacheBehaviorsItems); iBehaviorItem; ++iBehaviorItem)
				{
					if (Results.f_GetValue(iBehaviorItem) != "CacheBehavior")
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

		co_await fg_DoAWSRequestXML("Update distribution", Internal.m_CurlActor, 200, AWSUrl, fg_Move(Results), CCurlActor::EMethod_PUT, Internal.m_Credentials, Headers, "cloudfront");

		co_return {};
	}
}
