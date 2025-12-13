// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_AWS_Credentials.h"

namespace NMib::NWeb
{
	struct CCurlActor;

	struct CAwsEc2Actor : public NConcurrency::CActor
	{
		struct CRegionInfo
		{
			NStr::CStr m_RegionName;
			NStr::CStr m_Endpoint;
			NStr::CStr m_OptInStatus;
		};

		NConcurrency::TCFuture<NContainer::TCVector<CRegionInfo>> f_DescribeRegions();

		CAwsEc2Actor(NConcurrency::TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials);
		~CAwsEc2Actor();

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
