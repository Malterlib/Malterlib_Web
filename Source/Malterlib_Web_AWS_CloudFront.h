// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>
#include <Mib/Storage/Optional>
#include <Mib/Web/HTTP/URL>

#include "Malterlib_Web_AWS_Credentials.h"

namespace NMib::NWeb
{
	struct CCurlActor;

	struct CAwsCloudFrontActor : public NConcurrency::CActor
	{
		enum EFunctionEventType
		{
			EFunctionEventType_ViewerRequest
			, EFunctionEventType_ViewerResponse
			, EFunctionEventType_OriginRequest
			, EFunctionEventType_OriginResponse
		};

		NConcurrency::TCContinuation<NStr::CStr> f_CreateInvalidation(NStr::CStr const &_DistributionID, NContainer::TCVector<NStr::CStr> const &_Paths);
		NConcurrency::TCContinuation<void> f_UpdateDistributionLambdaFunctions
			(
			 	NStr::CStr const &_DistributionID
			 	, NContainer::TCMap<EFunctionEventType, NStr::CStr> const &_FunctionAssociations
			)
		;

		CAwsCloudFrontActor(NConcurrency::TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials);
		~CAwsCloudFrontActor();

	private:
		struct CInternal;

		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NWeb;
#endif
