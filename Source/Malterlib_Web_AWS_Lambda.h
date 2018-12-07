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

	struct CAwsLambdaActor : public NConcurrency::CActor
	{
		CAwsLambdaActor(NConcurrency::TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials);
		~CAwsLambdaActor();

		struct CFunctionConfiguration
		{
			struct CDeadLetterConfig
			{
				NStorage::TCOptional<NStr::CStr> m_TargetArn;
			};

			enum ETracingMode
			{
				ETracingMode_Unknown
				, ETracingMode_Active
				, ETracingMode_PassThrough
			};

			struct CTracingConfig
			{
				NStorage::TCOptional<ETracingMode> m_Mode;
			};

			struct CVpcConfig
			{
				NStorage::TCOptional<NContainer::TCVector<NStr::CStr>> m_SecurityGroupIds;
				NStorage::TCOptional<NContainer::TCVector<NStr::CStr>> m_SubnetIDs;
			};

			CDeadLetterConfig m_DeadLetterConfig;
			CTracingConfig m_TracingConfig;
			CVpcConfig m_VpcConfig;

			NStorage::TCOptional<NStr::CStr> m_Handler; // Required for create
			NStorage::TCOptional<NStr::CStr> m_Role; // Required for create
			NStorage::TCOptional<NStr::CStr> m_Runtime;  // Required for create

			NStorage::TCOptional<NContainer::TCMap<NStr::CStr, NStr::CStr>> m_EnvironmentVariables;
			NStorage::TCOptional<NContainer::TCMap<NStr::CStr, NStr::CStr>> m_Tags;
			NStorage::TCOptional<NStr::CStr> m_Description;
			NStorage::TCOptional<NStr::CStr> m_KMSKeyArn;
			NStorage::TCOptional<uint32> m_MemorySizeMB;
			NStorage::TCOptional<uint32> m_TimeoutSeconds;
			NStorage::TCOptional<bool> m_bPublish;
		};

		struct CFunctionInfo
		{
			NStr::CStr m_Arn;
			NStr::CStr m_Version;
		};

		NConcurrency::TCContinuation<CFunctionInfo> f_CreateOrUpdateFunction
			(
			 	NStr::CStr const &_FunctionName
			 	, NContainer::TCMap<NStr::CStr, NStr::CStr> const &_Files
			 	, CFunctionConfiguration const &_Config
			)
		;

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
