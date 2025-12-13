// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Container/Map>
#include <Mib/Container/Vector>

#include "Malterlib_Web_AWS_Credentials.h"

namespace NMib::NWeb
{
	struct CCurlActor;

	struct CAwsSsmActor : public NConcurrency::CActor
	{
		NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, NStr::CStr>> f_GetRegionLongNames(NContainer::TCVector<NStr::CStr> _RegionCodes);

		CAwsSsmActor(NConcurrency::TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials);
		~CAwsSsmActor();

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
