// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/Json>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Web/HttpClient>
#include <Mib/XML/XML>

#include "Malterlib_Web_AWS_Route53.h"
#include "Malterlib_Web_AWS_Internal.h"

namespace NMib::NWeb
{
	static constexpr ch8 gc_Route53ApiUrl[] = "https://route53.amazonaws.com/2013-04-01";
	static constexpr ch8 gc_Route53XmlNamespace[] = "https://route53.amazonaws.com/doc/2013-04-01/";

	struct CAwsRoute53Actor::CInternal : public NConcurrency::CActorInternal
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

	CAwsRoute53Actor::CAwsRoute53Actor(TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_HttpClientActor, _Credentials)}
	{
	}

	CAwsRoute53Actor::~CAwsRoute53Actor() = default;

	NStr::CStr CAwsRoute53Actor::fs_ResourceRecordTypeToStr(CAwsRoute53Actor::EResourceRecordType _Type)
	{
		switch (_Type)
		{
		case CAwsRoute53Actor::EResourceRecordType_A: return "A";
		case CAwsRoute53Actor::EResourceRecordType_AAAA: return "AAAA";
		case CAwsRoute53Actor::EResourceRecordType_CAA: return "CAA";
		case CAwsRoute53Actor::EResourceRecordType_CNAME: return "CNAME";
		case CAwsRoute53Actor::EResourceRecordType_MX: return "MX";
		case CAwsRoute53Actor::EResourceRecordType_NAPTR: return "NAPTR";
		case CAwsRoute53Actor::EResourceRecordType_NS: return "NS";
		case CAwsRoute53Actor::EResourceRecordType_PTR: return "PTR";
		case CAwsRoute53Actor::EResourceRecordType_SOA: return "SOA";
		case CAwsRoute53Actor::EResourceRecordType_SPF: return "SPF";
		case CAwsRoute53Actor::EResourceRecordType_SRV: return "SRV";
		case CAwsRoute53Actor::EResourceRecordType_TXT: return "TXT";
		}

		DMibNeverGetHere;
		return "Unknown";
	}

	NConcurrency::TCWrapped<CAwsRoute53Actor::EResourceRecordType> CAwsRoute53Actor::fs_ResourceRecordTypeFromStr(NStr::CStr const &_Type)
	{
		if (_Type == "A")
			return CAwsRoute53Actor::EResourceRecordType_A;
		if (_Type == "AAAA")
			return CAwsRoute53Actor::EResourceRecordType_AAAA;
		if (_Type == "CAA")
			return CAwsRoute53Actor::EResourceRecordType_CAA;
		if (_Type == "CNAME")
			return CAwsRoute53Actor::EResourceRecordType_CNAME;
		if (_Type == "MX")
			return CAwsRoute53Actor::EResourceRecordType_MX;
		if (_Type == "NAPTR")
			return CAwsRoute53Actor::EResourceRecordType_NAPTR;
		if (_Type == "NS")
			return CAwsRoute53Actor::EResourceRecordType_NS;
		if (_Type == "PTR")
			return CAwsRoute53Actor::EResourceRecordType_PTR;
		if (_Type == "SOA")
			return CAwsRoute53Actor::EResourceRecordType_SOA;
		if (_Type == "SPF")
			return CAwsRoute53Actor::EResourceRecordType_SPF;
		if (_Type == "SRV")
			return CAwsRoute53Actor::EResourceRecordType_SRV;
		if (_Type == "TXT")
			return CAwsRoute53Actor::EResourceRecordType_TXT;

		return DMibErrorInstance("Unknown resource type: {}"_f << _Type);
	}

	auto CAwsRoute53Actor::f_ListResourceRecordSets(NStr::CStr _HostedZoneID, CListResourceRecordSetsParams _Params) -> NConcurrency::TCFuture<NContainer::TCVector<CResourceRecordSet>>
	{
		auto &Internal = *mp_pInternal;

		NHTTP::CURL AWSUrl = CStr{"{}/hostedzone/{}/rrset"_f << gc_Route53ApiUrl << _HostedZoneID};
		AWSUrl.f_AddQueryEntry({"maxitems", "{}"_f << 1});
		if (_Params.m_MaxItems)
			AWSUrl.f_AddQueryEntry({"maxitems", "{}"_f << fg_Min(*_Params.m_MaxItems, 100u)});
		if (_Params.m_Name)
			AWSUrl.f_AddQueryEntry({"name", *_Params.m_Name});
		if (_Params.m_Type)
			AWSUrl.f_AddQueryEntry({"type", fs_ResourceRecordTypeToStr(*_Params.m_Type)});

		auto CurrentURL = AWSUrl;

		NContainer::TCVector<CResourceRecordSet> Return;
		while (true)
		{
			auto Results = co_await fg_DoAWSRequestXML
				(
					"List resource record sets"
					, Internal.m_HttpClientActor
					, 200
					, CurrentURL
					, {}
					, CHttpClientActor::EMethod_GET
					, Internal.m_Credentials
					, {}
					, "route53"
				)
			;

			auto &ResultsXML = fg_Get<0>(Results);

			auto fReportInvalidXML = [&](CStr const &_Entry)
				{
					return DMibErrorInstance("List resource record sets failed to find a valid '{}' in XML"_f << _Entry);
				}
			;

			auto pListResourceRecordSetsResponse = ResultsXML.fs_GetChildNode(ResultsXML.f_GetRootNode(), "ListResourceRecordSetsResponse");
			if (!pListResourceRecordSetsResponse)
				co_return fReportInvalidXML("ListResourceRecordSetsResponse");

			auto pResourceRecordSets = ResultsXML.fs_GetChildNode(pListResourceRecordSetsResponse, "ResourceRecordSets");
			if (!pResourceRecordSets)
				co_return fReportInvalidXML("ListResourceRecordSetsResponse.ResourceRecordSets");

			bool bFinished = false;

			for (NXML::CXMLDocument::CConstNodeIterator iNode = pResourceRecordSets; iNode; ++iNode)
			{
				if (ResultsXML.fs_GetValue(iNode) != "ResourceRecordSet")
					continue;

				CAwsRoute53Actor::CResourceRecordSet ReturnedEntry;

				ReturnedEntry.m_Name = ResultsXML.fs_GetChildValue(iNode, "Name", CStr{});

				auto Type = fs_ResourceRecordTypeFromStr(ResultsXML.fs_GetChildValue(iNode, "Type", CStr{}));

				// A record type this client doesn't know, for example HTTPS or TLSA, can't be returned. It's never the type that was asked for, and the
				// records are listed in name order, so it ends a list for another name or a given type. Otherwise it's skipped.
				if (!Type)
				{
					if ((_Params.m_Name && ReturnedEntry.m_Name != *_Params.m_Name) || _Params.m_Type)
					{
						bFinished = true;
						break;
					}
					continue;
				}

				ReturnedEntry.m_Type = *Type;

				if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "TTL"))
					ReturnedEntry.m_TTL = ResultsXML.fs_GetNodeInt(pValue);

				if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "Weight"))
					ReturnedEntry.m_Weight = ResultsXML.fs_GetNodeInt(pValue);

				if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "MultiValueAnswer"))
					ReturnedEntry.m_MultiValueAnswer = ResultsXML.fs_GetNodeBool(pValue);

				if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "Failover"))
					ReturnedEntry.m_Failover = ResultsXML.fs_GetNodeText(pValue);

				if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "HealthCheckID"))
					ReturnedEntry.m_HealthCheckID = ResultsXML.fs_GetNodeText(pValue);

				if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "Region"))
					ReturnedEntry.m_Region = ResultsXML.fs_GetNodeText(pValue);

				if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "SetIdentifier"))
					ReturnedEntry.m_SetIdentifier = ResultsXML.fs_GetNodeText(pValue);

				if (auto pAliasTarget = ResultsXML.fs_GetChildNode(iNode, "AliasTarget"))
				{
					CAwsRoute53Actor::CResourceRecordSet::CAliasTarget Target;

					Target.m_DNSName = ResultsXML.fs_GetChildValue(pAliasTarget, "DNSName", CStr{});
					Target.m_EvaluateTargetHealth = ResultsXML.fs_GetChildValue(pAliasTarget, "EvaluateTargetHealth", CStr{});
					Target.m_HostedZoneID = ResultsXML.fs_GetChildValue(pAliasTarget, "HostedZoneID", CStr{});

					ReturnedEntry.m_AliasTarget = fg_Move(Target);
				}

				if (auto pGeoLocation = ResultsXML.fs_GetChildNode(iNode, "GeoLocation"))
				{
					CAwsRoute53Actor::CResourceRecordSet::CGeoLocation GeoLocation;

					GeoLocation.m_ContinentCode = ResultsXML.fs_GetChildValue(pGeoLocation, "ContinentCode", CStr{});
					GeoLocation.m_CountryCode = ResultsXML.fs_GetChildValue(pGeoLocation, "CountryCode", CStr{});
					GeoLocation.m_SubdivisionCode = ResultsXML.fs_GetChildValue(pGeoLocation, "SubdivisionCode", CStr{});

					ReturnedEntry.m_GeoLocation = fg_Move(GeoLocation);
				}

				if (auto pResourceRecords = ResultsXML.fs_GetChildNode(iNode, "ResourceRecords"))
				{
					for (NXML::CXMLDocument::CConstNodeIterator iNode = pResourceRecords; iNode; ++iNode)
					{
						if (ResultsXML.fs_GetValue(iNode) != "ResourceRecord")
							continue;
						if (auto pValue = ResultsXML.fs_GetChildNode(iNode, "Value"))
							ReturnedEntry.m_ResourceRecords.f_Insert(ResultsXML.fs_GetNodeText(pValue));
					}
				}

				if (_Params.m_Name && ReturnedEntry.m_Name != *_Params.m_Name)
				{
					bFinished = true;
					break;
				}

				if (_Params.m_Type && ReturnedEntry.m_Type != *_Params.m_Type)
				{
					bFinished = true;
					break;
				}

				Return.f_Insert(fg_Move(ReturnedEntry));

				if (_Params.m_MaxItems && Return.f_GetLen() >= *_Params.m_MaxItems)
				{
					bFinished = true;
					break;
				}
			}

			if (bFinished)
				break;

			if (ResultsXML.fs_GetChildValue(pListResourceRecordSetsResponse, "IsTruncated", false))
			{
				CStr NextRecordName = ResultsXML.fs_GetChildValue(pListResourceRecordSetsResponse, "NextRecordName", CStr{});
				if (NextRecordName.f_IsEmpty())
					fReportInvalidXML("ListResourceRecordSetsResponse.NextRecordName");

				CurrentURL.f_AddQueryEntry({"name", NextRecordName});

				CStr NextRecordType = ResultsXML.fs_GetChildValue(pListResourceRecordSetsResponse, "NextRecordType", CStr{});
				if (NextRecordType.f_IsEmpty())
					fReportInvalidXML("ListResourceRecordSetsResponse.NextRecordType");

				CurrentURL.f_AddQueryEntry({"type", NextRecordType});
			}
			else
				break;
		}

		co_return fg_Move(Return);
	}

	auto CAwsRoute53Actor::f_ListHostedZonesByName(CListHostedZonesByNameParams _Params) -> NConcurrency::TCFuture<NContainer::TCVector<CHostedZone>>
	{
		auto &Internal = *mp_pInternal;

		// GET /2013-04-01/hostedzonesbyname?dnsname=DNSName&hostedzoneid=HostedZoneId&maxitems=MaxItems HTTP/1.1
		NHTTP::CURL AWSUrl = CStr("{}/hostedzonesbyname"_f << gc_Route53ApiUrl);

		if (_Params.m_MaxItems)
			AWSUrl.f_AddQueryEntry({"maxitems", "{}"_f << fg_Min(*_Params.m_MaxItems, 100u)});

		if (_Params.m_DNSName)
			AWSUrl.f_AddQueryEntry({"dnsname", *_Params.m_DNSName});

		NContainer::TCVector<CHostedZone> Return;
		auto CurrentURL = AWSUrl;
		while (true)
		{
			auto Results = co_await fg_DoAWSRequestXML
				(
					"List hosted zones by name"
					, Internal.m_HttpClientActor
					, 200
					, CurrentURL
					, {}
					, CHttpClientActor::EMethod_GET
					, Internal.m_Credentials
					, {}
					, "route53"
				)
			;

			auto &ResultsXML = fg_Get<0>(Results);

			auto fReportInvalidXML = [&](CStr const &_Entry)
				{
					return DMibErrorInstance("List hosted zones by name failed to find a valid '{}' in XML"_f << _Entry);
				}
			;

			auto pListHostedZonesByNameResponse = ResultsXML.fs_GetChildNode(ResultsXML.f_GetRootNode(), "ListHostedZonesByNameResponse");
			if (!pListHostedZonesByNameResponse)
				co_return fReportInvalidXML("ListHostedZonesByNameResponse");

			auto pHostedZones = ResultsXML.fs_GetChildNode(pListHostedZonesByNameResponse, "HostedZones");
			if (!pHostedZones)
				co_return fReportInvalidXML("ListHostedZonesByNameResponse.HostedZones");

			bool bFinished = false;

			for (NXML::CXMLDocument::CConstNodeIterator iNode = pHostedZones; iNode; ++iNode)
			{
				if (ResultsXML.fs_GetValue(iNode) != "HostedZone")
					continue;

				CAwsRoute53Actor::CHostedZone ReturnedEntry;

				if (auto Value = ResultsXML.fs_GetChildValue(iNode, "Id", CStr{}))
					ReturnedEntry.m_ID = Value.f_RemovePrefix("/hostedzone/");

				ReturnedEntry.m_CallerReference = ResultsXML.fs_GetChildValue(iNode, "Id", CStr{});
				ReturnedEntry.m_Name = ResultsXML.fs_GetChildValue(iNode, "Name", CStr{});

				if (ResultsXML.fs_GetChildNode(iNode, "ResourceRecordSetCount"))
					ReturnedEntry.m_ResourceRecordSetCount = ResultsXML.fs_GetChildValue(iNode, "ResourceRecordSetCount", int64(0));

				if (auto pConfig = ResultsXML.fs_GetChildNode(iNode, "Config"))
				{
					if (auto pValue = ResultsXML.fs_GetChildNode(pConfig, "Comment"))
						ReturnedEntry.m_Config.m_Comment = ResultsXML.fs_GetNodeText(pValue);
					if (auto pValue = ResultsXML.fs_GetChildNode(pConfig, "PrivateZone"))
						ReturnedEntry.m_Config.m_Private = ResultsXML.fs_GetNodeBool(pValue);
				}

				if (auto pLinkedService = ResultsXML.fs_GetChildNode(iNode, "LinkedService"))
				{
					if (auto pValue = ResultsXML.fs_GetChildNode(pLinkedService, "Description"))
						ReturnedEntry.m_LinkedService.m_Description = ResultsXML.fs_GetNodeText(pValue);
					if (auto pValue = ResultsXML.fs_GetChildNode(pLinkedService, "ServicePrincipal"))
						ReturnedEntry.m_LinkedService.m_ServicePrincipal = ResultsXML.fs_GetNodeText(pValue);
				}

				if (_Params.m_DNSName && ReturnedEntry.m_Name != *_Params.m_DNSName)
				{
					bFinished = true;
					break;
				}

				Return.f_Insert(fg_Move(ReturnedEntry));

				if (_Params.m_MaxItems && Return.f_GetLen() >= *_Params.m_MaxItems)
				{
					bFinished = true;
					break;
				}
			}

			if (bFinished)
				break;

			if (ResultsXML.fs_GetChildValue(pListHostedZonesByNameResponse, "IsTruncated", false))
			{
				CStr NextDNSName = ResultsXML.fs_GetChildValue(pListHostedZonesByNameResponse, "NextDNSName", CStr{});
				if (NextDNSName.f_IsEmpty())
					fReportInvalidXML("ListHostedZonesByNameResponse.NextDNSName");

				CurrentURL.f_AddQueryEntry({"dnsname", NextDNSName});

				CStr NextHostedZoneID = ResultsXML.fs_GetChildValue(pListHostedZonesByNameResponse, "NextHostedZoneId", CStr{});
				if (NextHostedZoneID.f_IsEmpty())
					fReportInvalidXML("ListHostedZonesByNameResponse.NextHostedZoneId");

				CurrentURL.f_AddQueryEntry({"hostedzoneid", NextHostedZoneID});
			}
			else
				break;
		}

		co_return fg_Move(Return);
	}

	NConcurrency::TCFuture<void> CAwsRoute53Actor::f_ChangeResourceRecordSets(NStr::CStr _HostedZoneID, CChangeResourceRecordSetsParams _Params)
	{
		auto &Internal = *mp_pInternal;

		// POST /2013-04-01/hostedzone/Id/rrset/ HTTP/1.1
		NHTTP::CURL AWSUrl = CStr("{}/hostedzone/{}/rrset/"_f << gc_Route53ApiUrl << _HostedZoneID);

		NXML::CXMLDocument PostDocument(false);
		{
			auto pRequest = PostDocument.f_CreateDefaultDocument("ChangeResourceRecordSetsRequest");
			PostDocument.fs_SetAttribute(pRequest, "xmlns", gc_Route53XmlNamespace);

			auto pChangeBatch = PostDocument.fs_CreateElement(pRequest, "ChangeBatch");
			auto pChanges = PostDocument.fs_CreateElement(pChangeBatch, "Changes");

			for (auto &Change : _Params.m_Changes)
			{
				auto pChange = PostDocument.fs_CreateElement(pChanges, "Change");
				{
					CStr Action;
					switch (Change.m_Action)
					{
					case EChangeResourceRecordSetsChangeAction_Create: Action = "CREATE"; break;
					case EChangeResourceRecordSetsChangeAction_Delete: Action = "DELETE"; break;
					case EChangeResourceRecordSetsChangeAction_Upsert: Action = "UPSERT"; break;
					}
					PostDocument.fs_AddElementAndText(pChange, "Action", Action);
				}
				{
					auto pResourceRecordSet = PostDocument.fs_CreateElement(pChange, "ResourceRecordSet");

					PostDocument.fs_AddElementAndText(pResourceRecordSet, "Name", Change.m_RecordSet.m_Name);
					PostDocument.fs_AddElementAndText(pResourceRecordSet, "Type", fs_ResourceRecordTypeToStr(Change.m_RecordSet.m_Type));

					if (!Change.m_RecordSet.m_ResourceRecords.f_IsEmpty())
					{
						auto pResourceRecords = PostDocument.fs_CreateElement(pResourceRecordSet, "ResourceRecords");
						for (auto &Record : Change.m_RecordSet.m_ResourceRecords)
							PostDocument.fs_AddElementAndText(PostDocument.fs_CreateElement(pResourceRecords, "ResourceRecord"), "Value", Record);
					}

					if (Change.m_RecordSet.m_AliasTarget)
					{
						auto pAliasTarget = PostDocument.fs_CreateElement(pResourceRecordSet, "AliasTarget");
						auto &AliasTarget = *Change.m_RecordSet.m_AliasTarget;
						PostDocument.fs_AddElementAndText(pAliasTarget, "DNSName", AliasTarget.m_DNSName);
						PostDocument.fs_AddElementAndText(pAliasTarget, "EvaluateTargetHealth", AliasTarget.m_EvaluateTargetHealth);
						PostDocument.fs_AddElementAndText(pAliasTarget, "HostedZoneId", AliasTarget.m_HostedZoneID);
					}

					if (Change.m_RecordSet.m_GeoLocation)
					{
						auto pGeoLocation = PostDocument.fs_CreateElement(pResourceRecordSet, "GeoLocation");
						auto &GeoLocation = *Change.m_RecordSet.m_GeoLocation;
						PostDocument.fs_AddElementAndText(pGeoLocation, "ContinentCode", GeoLocation.m_ContinentCode);
						PostDocument.fs_AddElementAndText(pGeoLocation, "CountryCode", GeoLocation.m_CountryCode);
						PostDocument.fs_AddElementAndText(pGeoLocation, "SubdivisionCode", GeoLocation.m_SubdivisionCode);
					}

					if (Change.m_RecordSet.m_Failover)
						PostDocument.fs_AddElementAndText(pResourceRecordSet, "Failover", *Change.m_RecordSet.m_Failover);

					if (Change.m_RecordSet.m_HealthCheckID)
						PostDocument.fs_AddElementAndText(pResourceRecordSet, "HealthCheckID", *Change.m_RecordSet.m_HealthCheckID);

					if (Change.m_RecordSet.m_MultiValueAnswer)
						PostDocument.fs_AddElementAndBool(pResourceRecordSet, "MultiValueAnswer", *Change.m_RecordSet.m_MultiValueAnswer);

					if (Change.m_RecordSet.m_Region)
						PostDocument.fs_AddElementAndText(pResourceRecordSet, "Region", *Change.m_RecordSet.m_Region);

					if (Change.m_RecordSet.m_SetIdentifier)
						PostDocument.fs_AddElementAndText(pResourceRecordSet, "SetIdentifier", *Change.m_RecordSet.m_SetIdentifier);

					if (Change.m_RecordSet.m_TrafficPolicyInstanceID)
						PostDocument.fs_AddElementAndText(pResourceRecordSet, "TrafficPolicyInstanceId", *Change.m_RecordSet.m_TrafficPolicyInstanceID);

					if (Change.m_RecordSet.m_TTL)
						PostDocument.fs_AddElementAndInt(pResourceRecordSet, "TTL", *Change.m_RecordSet.m_TTL);

					if (Change.m_RecordSet.m_Weight)
						PostDocument.fs_AddElementAndInt(pResourceRecordSet, "Weight", *Change.m_RecordSet.m_Weight);
				}
			}

			if (_Params.m_Comment)
				PostDocument.fs_AddElementAndText(pChangeBatch, "Comment", *_Params.m_Comment);
		}

		auto Results = co_await fg_DoAWSRequestXML
			(
				"Change resource record sets"
				, Internal.m_HttpClientActor
				, 200
				, AWSUrl
				, fg_Move(PostDocument)
				, CHttpClientActor::EMethod_POST
				, Internal.m_Credentials
				, {}
				, "route53"
			)
		;

		auto &ResultsXML = fg_Get<0>(Results);

		auto fReportInvalidXML = [&](CStr const &_Entry)
			{
				return DMibErrorInstance("Change resource record sets failed to find a valid '{}' in XML"_f << _Entry);
			}
		;

		if (!_Params.m_bWaitForPropagation)
			co_return {};

		auto pChangeResourceRecordSetsResponse = ResultsXML.fs_GetChildNode(ResultsXML.f_GetRootNode(), "ChangeResourceRecordSetsResponse");
		if (!pChangeResourceRecordSetsResponse)
			co_return fReportInvalidXML("ChangeResourceRecordSetsResponse");

		auto pChangeInfo = ResultsXML.fs_GetChildNode(pChangeResourceRecordSetsResponse, "ChangeInfo");
		if (!pChangeInfo)
			co_return fReportInvalidXML("ChangeResourceRecordSetsResponse.ChangeInfo");

		{
			NStr::CStr Status = ResultsXML.fs_GetChildValue(pChangeInfo, "Status", CStr());
			if (Status == "INSYNC")
				co_return {};
			else if (Status != "PENDING")
				co_return DMibErrorInstance("Unexpected ChangeInfo status: {}"_f << Status);
		}

		NStr::CStr Id = ResultsXML.fs_GetChildValue(pChangeInfo, "Id", CStr()).f_RemovePrefix("/change/");
		if (!Id)
			co_return fReportInvalidXML("ChangeResourceRecordSetsResponse.ChangeInfo.Id");

		while (true)
		{
			// GET /2013-04-01/change/Id HTTP/1.1
			NHTTP::CURL AWSUrl = CStr("{}/change/{}"_f << gc_Route53ApiUrl << Id);

			auto Results = co_await fg_DoAWSRequestXML("Get change status", Internal.m_HttpClientActor, 200, AWSUrl, {}, CHttpClientActor::EMethod_GET, Internal.m_Credentials, {}, "route53")
				.f_Wrap()
			;

			if (!Results)
			{
				bool bNoSuchChange = false;
				NException::fg_VisitException<CExceptionAws>
					(
						Results.f_GetException()
						, [&](CExceptionAws const &_Exception)
						{
							if (_Exception.f_GetSpecific().m_StatusCode == 404 && _Exception.f_GetSpecific().m_ErrorCode == "NoSuchChange")
								bNoSuchChange = true;
						}
					)
				;

				if (bNoSuchChange)
					co_return {}; // Change does not exist so should be done

				co_return Results.f_GetException();
			}

			auto &ResultsXML = fg_Get<0>(*Results);

			auto pGetChangeResponse = ResultsXML.fs_GetChildNode(ResultsXML.f_GetRootNode(), "GetChangeResponse");
			if (!pGetChangeResponse)
				co_return fReportInvalidXML("GetChangeResponse");

			auto pChangeInfo = ResultsXML.fs_GetChildNode(pGetChangeResponse, "ChangeInfo");
			if (!pChangeInfo)
				co_return fReportInvalidXML("GetChangeResponse.ChangeInfo");

			{
				NStr::CStr Status = ResultsXML.fs_GetChildValue(pChangeInfo, "Status", CStr());
				if (Status == "INSYNC")
					co_return {};
				else if (Status != "PENDING")
					co_return DMibErrorInstance("Unexpected ChangeInfo status: {}"_f << Status);
			}

			co_await fg_Timeout(_Params.m_PollInterval); // Poll every 5 seconds
		}
	}
}
