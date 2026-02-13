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
	struct CHttpClientActor;

	struct CAwsS3Actor : public NConcurrency::CActor
	{
		struct CUserIdentity
		{
			NStr::CStr m_ID;
			NStr::CStr m_DisplayName;
		};

		enum EStorageClass
		{
			EStorageClass_Unknown
			, EStorageClass_Standard
			, EStorageClass_StandardInfrequentAccess
			, EStorageClass_OneZoneInfrequentAccess
			, EStorageClass_ReducedRedundancy
			, EStorageClass_Glacier
		};

		struct CObjectInfo
		{
			NStr::CStr m_Key;
			uint64 m_Size = 0;
			NStr::CStr m_ETag;
			NTime::CTime m_LastModified;
			CUserIdentity m_Owner;
			EStorageClass m_StorageClass = EStorageClass_Unknown;
		};

		struct CObjectInfoMetadata : public CObjectInfo
		{
			NStorage::TCOptional<NStr::CStr> m_CacheControl;
			NStorage::TCOptional<uint64> m_ContentLength;
			NStorage::TCOptional<NStr::CStr> m_ContentDisposition;
			NStorage::TCOptional<NStr::CStr> m_ContentEncoding;
			NStorage::TCOptional<NStr::CStr> m_ContentType;
			NStorage::TCOptional<NStr::CStr> m_Date;
			NStorage::TCOptional<NStr::CStr> m_LastModified;
			NStorage::TCOptional<NStr::CStr> m_RedirectLocation;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Metadata;
		};

		struct CListBucket
		{
			NStr::CStr m_BucketName;
			NContainer::TCVector<CObjectInfo> m_Objects;
		};

		struct CPutObjectInfo
		{
			NStorage::TCOptional<NStr::CStr> m_CacheControl;
			NStorage::TCOptional<NStr::CStr> m_ContentDisposition;
			NStorage::TCOptional<NStr::CStr> m_ContentEncoding;
			NStorage::TCOptional<NStr::CStr> m_ContentType;
			NStorage::TCOptional<NStr::CStr> m_RedirectLocation;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Tags;
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_Metadata;
			EStorageClass m_StorageClass = EStorageClass_Unknown;
		};

		NConcurrency::TCFuture<CListBucket> f_ListBucket(NStr::CStr _BucketName);

		NConcurrency::TCFuture<CObjectInfoMetadata> f_GetObjectMetadata(NStr::CStr _BucketName, NStr::CStr _Key);
		NConcurrency::TCFuture<void> f_PutObject(NStr::CStr _BucketName, NStr::CStr _Key, CPutObjectInfo _Info, NContainer::CByteVector _Data);
		NConcurrency::TCFuture<void> f_PutObjectMultipart
			(
				NStr::CStr _BucketName
				, NStr::CStr _Key
				, CPutObjectInfo _Info
				, uint64 _TotalSize
				, NConcurrency::TCActorFunctor<NConcurrency::TCFuture<NContainer::CByteVector> ()> _fGetPart
			)
		;

		NConcurrency::TCFuture<void> f_DeleteObject(NStr::CStr _BucketName, NStr::CStr _Key);

		CAwsS3Actor(NConcurrency::TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials);
		~CAwsS3Actor();

	private:
		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
