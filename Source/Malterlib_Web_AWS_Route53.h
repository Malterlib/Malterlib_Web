// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>
#include <Mib/Storage/Optional>
#include <Mib/Web/HTTP/URL>

#include "Malterlib_Web_AWS_Credentials.h"

namespace NMib::NWeb
{
	struct CHttpClientActor;

	struct CAwsRoute53Actor : public NConcurrency::CActor
	{
		enum EResourceRecordType
		{
			EResourceRecordType_A
			, EResourceRecordType_AAAA
			, EResourceRecordType_CAA
			, EResourceRecordType_CNAME
			, EResourceRecordType_MX
			, EResourceRecordType_NAPTR
			, EResourceRecordType_NS
			, EResourceRecordType_PTR
			, EResourceRecordType_SOA
			, EResourceRecordType_SPF
			, EResourceRecordType_SRV
			, EResourceRecordType_TXT
		};

		enum EChangeResourceRecordSetsChangeAction
		{
			EChangeResourceRecordSetsChangeAction_Create
			, EChangeResourceRecordSetsChangeAction_Delete
			, EChangeResourceRecordSetsChangeAction_Upsert
		};

		struct CListResourceRecordSetsParams
		{
			NStorage::TCOptional<uint32> m_MaxItems;
			NStorage::TCOptional<NStr::CStr> m_Name;
			NStorage::TCOptional<EResourceRecordType> m_Type;
		};

		struct CListHostedZonesByNameParams
		{
			NStorage::TCOptional<uint32> m_MaxItems;
			NStorage::TCOptional<NStr::CStr> m_DNSName;
		};

		struct CResourceRecordSet
		{
			struct CAliasTarget
			{
				NStr::CStr m_DNSName;
				NStr::CStr m_EvaluateTargetHealth;
				NStr::CStr m_HostedZoneID;
			};

			struct CGeoLocation
			{
				NStr::CStr m_ContinentCode;
				NStr::CStr m_CountryCode;
				NStr::CStr m_SubdivisionCode;
			};

			NStr::CStr m_Name;
			EResourceRecordType m_Type;
			NContainer::TCVector<NStr::CStr> m_ResourceRecords;

			NStorage::TCOptional<CAliasTarget> m_AliasTarget;
			NStorage::TCOptional<NStr::CStr> m_Failover;
			NStorage::TCOptional<CGeoLocation> m_GeoLocation;
			NStorage::TCOptional<NStr::CStr> m_HealthCheckID;
			NStorage::TCOptional<bool> m_MultiValueAnswer;
			NStorage::TCOptional<NStr::CStr> m_Region;
			NStorage::TCOptional<NStr::CStr> m_SetIdentifier;
			NStorage::TCOptional<NStr::CStr> m_TrafficPolicyInstanceID;
			NStorage::TCOptional<uint32> m_TTL;
			NStorage::TCOptional<uint32> m_Weight;
		};

		struct CHostedZone
		{
			struct CConfig
			{
				NStorage::TCOptional<NStr::CStr> m_Comment;
				NStorage::TCOptional<bool> m_Private;
			};

			struct CLinkedService
			{
				NStorage::TCOptional<NStr::CStr> m_Description;
				NStorage::TCOptional<NStr::CStr> m_ServicePrincipal;
			};

			NStr::CStr m_CallerReference;
			CConfig m_Config;
			NStr::CStr m_ID;
			CLinkedService m_LinkedService;
			NStr::CStr m_Name;
			NStorage::TCOptional<uint32> m_ResourceRecordSetCount;
		};

		struct CChangeResourceRecordSetsParams
		{
			struct CChange
			{
				CResourceRecordSet m_RecordSet;
				EChangeResourceRecordSetsChangeAction m_Action = EChangeResourceRecordSetsChangeAction_Upsert;
			};

			NContainer::TCVector<CChange> m_Changes;
			NStorage::TCOptional<NStr::CStr> m_Comment;
			fp32 m_PollInterval = 5.0;
			bool m_bWaitForPropagation = true;
		};

		static NStr::CStr fs_ResourceRecordTypeToStr(CAwsRoute53Actor::EResourceRecordType _Type);
		static CAwsRoute53Actor::EResourceRecordType fs_ResourceRecordTypeFromStr(NStr::CStr const &_Type);

		NConcurrency::TCFuture<NContainer::TCVector<CHostedZone>> f_ListHostedZonesByName(CListHostedZonesByNameParams _Params);
		NConcurrency::TCFuture<NContainer::TCVector<CResourceRecordSet>> f_ListResourceRecordSets(NStr::CStr _HostedZoneID, CListResourceRecordSetsParams _Params);
		NConcurrency::TCFuture<void> f_ChangeResourceRecordSets(NStr::CStr _HostedZoneID, CChangeResourceRecordSetsParams _Params);

		CAwsRoute53Actor(NConcurrency::TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials);
		~CAwsRoute53Actor();

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
