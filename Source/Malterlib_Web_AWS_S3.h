// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctor>
#include <Mib/Storage/Optional>
#include <Mib/Web/HTTP/URL>

namespace NMib::NWeb
{
	struct CCurlActor;

	struct CAwsS3Actor : public NConcurrency::CActor
	{
		struct CCredentials
		{
			NStr::CStr m_Region;
			NStr::CStr m_AccessKeyID;
			NStr::CStrSecure m_SecretKey;
		};

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
			NContainer::TCMap<NStr::CStr, NStr::CStr> m_MetaData;
			EStorageClass m_StorageClass = EStorageClass_Unknown;
		};

		NConcurrency::TCContinuation<CListBucket> f_ListBucket(NStr::CStr const &_BucketName);

		NConcurrency::TCContinuation<void> f_PutObject(NStr::CStr const &_BucketName, NStr::CStr const &_Key, CPutObjectInfo const &_Info, NContainer::TCVector<uint8> &&_Data);
		NConcurrency::TCContinuation<void> f_PutObjectMultipart
			(
			 	NStr::CStr const &_BucketName
			 	, NStr::CStr const &_Key
			 	, CPutObjectInfo const &_Info
			 	, uint64 _TotalSize
			 	, NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<NContainer::TCVector<uint8>> ()> &&_fGetPart
			)
		;

		NConcurrency::TCContinuation<void> f_DeleteObject(NStr::CStr const &_BucketName, NStr::CStr const &_Key);

		CAwsS3Actor(NConcurrency::TCActor<CCurlActor> const &_CurlActor, CCredentials const &_Credentials);
		~CAwsS3Actor();

	private:
		struct CInternal;

		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NWeb;
#endif
