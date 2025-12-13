// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Web/Curl>
#include <Mib/XML/XML>

#include "Malterlib_Web_AWS_EC2.h"
#include "Malterlib_Web_AWS_Internal.h"

namespace NMib::NWeb
{
	using namespace NStr;
	using namespace NContainer;
	using namespace NConcurrency;
	using namespace NStorage;

	struct CAwsEc2Actor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
			: m_CurlActor{_CurlActor}
			, m_Credentials{_Credentials}
		{
		}

		CAwsCredentials m_Credentials;
		TCActor<CCurlActor> m_CurlActor;
	};

	CAwsEc2Actor::CAwsEc2Actor(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_CurlActor, _Credentials)}
	{
	}

	CAwsEc2Actor::~CAwsEc2Actor() = default;

	TCFuture<TCVector<CAwsEc2Actor::CRegionInfo>> CAwsEc2Actor::f_DescribeRegions()
	{
		auto &Internal = *mp_pInternal;

		CStr Region = Internal.m_Credentials.m_Region.f_IsEmpty()
			? "us-east-1"
			: Internal.m_Credentials.m_Region;

		NHTTP::CURL URL = CStr{"https://ec2.{}.amazonaws.com/"_f << Region};
		CByteVector Body = CByteVector::fs_FromString("Action=DescribeRegions&Version=2016-11-15");

		auto Results = co_await fg_DoAWSRequestXML
			(
				"DescribeRegions"
				, Internal.m_CurlActor
				, 200
				, URL
				, Body
				, CCurlActor::EMethod_POST
				, Internal.m_Credentials
				, {{"Content-Type", "application/x-www-form-urlencoded"}}
				, "ec2"
			)
		;

		auto &XML = fg_Get<0>(Results);

		auto fReportInvalidXML = [&](CStr const &_Entry)
			{
				return DMibErrorInstance("DescribeRegions failed to find a valid '{}' in XML"_f << _Entry);
			}
		;

		TCVector<CRegionInfo> Regions;

		auto pDescribeRegionsResponse = XML.f_GetChildNode(XML.f_GetRootNode(), "DescribeRegionsResponse");
		if (!pDescribeRegionsResponse)
			co_return fReportInvalidXML("DescribeRegionsResponse");

		auto pRegionInfo = XML.f_GetChildNode(pDescribeRegionsResponse, "regionInfo");
		if (!pRegionInfo)
			co_return fReportInvalidXML("DescribeRegionsResponse.regionInfo");

		for (NXML::CXMLDocument::CConstNodeIterator iNode = pRegionInfo; iNode; ++iNode)
		{
			if (XML.f_GetValue(iNode) != "item")
				continue;

			CRegionInfo &Info = Regions.f_Insert();
			Info.m_RegionName = XML.f_GetChildValue(iNode, "regionName", CStr{});
			Info.m_Endpoint = XML.f_GetChildValue(iNode, "regionEndpoint", CStr{});
			Info.m_OptInStatus = XML.f_GetChildValue(iNode, "optInStatus", CStr{});
		}

		co_return Regions;
	}
}
