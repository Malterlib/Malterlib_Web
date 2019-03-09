// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSON>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Web/Curl>
#include <Mib/Network/SSL>
#include <Mib/XML/XML>

#include "Malterlib_Web_AWS_S3.h"
#include "Malterlib_Web_AWS_Internal.h"

namespace NMib::NWeb
{
	struct CAwsS3Actor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
			: m_CurlActor{_CurlActor}
			, m_Credentials{_Credentials}
		{
		}

		CAwsCredentials m_Credentials;
		TCActor<CCurlActor> m_CurlActor;
	};

	CAwsS3Actor::CAwsS3Actor(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_CurlActor, _Credentials)}
	{
	}

	CAwsS3Actor::~CAwsS3Actor() = default;

	NConcurrency::TCFuture<CAwsS3Actor::CObjectInfoMetaData> CAwsS3Actor::f_GetObjectMetaData(NStr::CStr const &_BucketName, NStr::CStr const &_Key)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://s3-{}.amazonaws.com/{}/{}"_f << Internal.m_Credentials.m_Region << _BucketName << _Key};

		TCPromise<CAwsS3Actor::CObjectInfoMetaData> Promise;

		fg_DoAWSRequestHEAD("Get meta data", Internal.m_CurlActor, 200, AWSUrl, Internal.m_Credentials, {}, "s3")
			> Promise / [=](NContainer::TCMap<NStr::CStr, NStr::CStr> &&_Headers)
			{
				CAwsS3Actor::CObjectInfoMetaData MetaData;

				for (auto &HeaderValue : _Headers)
				{
					auto &Header = _Headers.fs_GetKey(HeaderValue);
					if (Header == "cache-control")
						MetaData.m_CacheControl = HeaderValue;
					else if (Header == "content-length")
						MetaData.m_ContentLength = HeaderValue.f_ToInt(uint64(0));
					else if (Header == "cache-control")
						MetaData.m_CacheControl = HeaderValue;
					else if (Header == "content-type")
						MetaData.m_ContentType = HeaderValue;
					else if (Header == "content-encoding")
						MetaData.m_ContentEncoding = HeaderValue;
					else if (Header == "content-disposition")
						MetaData.m_ContentDisposition = HeaderValue;
					else if (Header == "etag")
						MetaData.m_ETag = HeaderValue;
					else if (Header == "date")
						MetaData.m_Date = HeaderValue;
					else if (Header == "last-modified")
						MetaData.m_LastModified = HeaderValue;
					else if (Header == "x-amz-website​-redirect-location")
						MetaData.m_RedirectLocation = HeaderValue;
					else if (Header.f_StartsWith("x-amz-meta-"))
						MetaData.m_MetaData[Header.f_Extract(11)] = HeaderValue;
				}

				Promise.f_SetResult(fg_Move(MetaData));
			}
		;

		return Promise.f_MoveFuture();
	}

	TCFuture<CAwsS3Actor::CListBucket> CAwsS3Actor::f_ListBucket(CStr const &_BucketName)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://s3-{}.amazonaws.com/{}/?list-type=2"_f << Internal.m_Credentials.m_Region << _BucketName};

		TCPromise<CAwsS3Actor::CListBucket> Promise;
		NStorage::TCSharedPointer<CAwsS3Actor::CListBucket> pResult = fg_Construct();

		auto fDoRequest = [=](auto const &_fDoRequest, CStr const &_ContinuationToken) -> void
			{
				auto &Internal = *mp_pInternal;

				auto NewURL = AWSUrl;
				if (_ContinuationToken)
					NewURL.f_AddQueryEntry({"continuation-token", _ContinuationToken});

				fg_DoAWSRequestXML("List bucket", Internal.m_CurlActor, 200, NewURL, {}, CCurlActor::EMethod_GET, Internal.m_Credentials, {}, "s3")
					> Promise / [=](NStorage::TCTuple<NXML::CXMLDocument, CCurlActor::CResult> &&_Result)
					{
						auto &[Results, CurlResult] = _Result;

						auto fReportInvalidXML = [&](CStr const &_Entry)
							{
								Promise.f_SetException(DMibErrorInstance("List bucket request failed to find a valid '{}' in XML"_f << _Entry));
							}
						;

						auto pListBucketResult = Results.f_GetChildNode(Results.f_GetRootNode(), "ListBucketResult");
						if (!pListBucketResult)
							return fReportInvalidXML("ListBucketResult");

						for (NXML::CXMLDocument::CConstNodeIterator iNode = pListBucketResult; iNode; ++iNode)
						{
							if (Results.f_GetValue(iNode) != "Contents")
								continue;
							auto Key = Results.f_GetChildValue(iNode, "Key", CStr{});
							if (!Key)
								continue;

							auto &Object = pResult->m_Objects.f_Insert();
							Object.m_Key = Key;

							if (auto Value = Results.f_GetChildValue(iNode, "LastModified", CStr{}))
							{
								int64 Year;
								int32 Month;
								int32 Day;
								int32 Hour;
								int32 Minute;
								int32 Second;
								int32 Millisecond;
								aint nParsed = 0;
								(CStr::CParse("{}-{}-{}T{}:{}:{}.{}Z") >> Year >> Month >> Day >> Hour >> Minute >> Second >> Millisecond).f_Parse(Value, nParsed);
								if (nParsed == 7)
									Object.m_LastModified = NTime::CTimeConvert::fs_CreateTime(Year, Month, Day, Hour, Minute, Second, fp64(Millisecond) / fp64(1000.0));
							}
							if (auto Value = Results.f_GetChildValue(iNode, "ETag", CStr{}))
							{
								CStr ParsedValue;
								(CStr::CParse("\"{}\"") >> ParsedValue).f_Parse(Value);
								if (!ParsedValue.f_IsEmpty())
									Object.m_ETag = ParsedValue;
								else
									Object.m_ETag = Value;
							}
							Object.m_Size = Results.f_GetChildValue(iNode, "Size", int64(0));
							if (auto Value = Results.f_GetChildValue(iNode, "StorageClass", CStr{}))
							{
								if (Value == "STANDARD")
									Object.m_StorageClass = EStorageClass_Standard;
								else if (Value == "STANDARD_IA")
									Object.m_StorageClass = EStorageClass_StandardInfrequentAccess;
								else if (Value == "ONEZONE_IA")
									Object.m_StorageClass = EStorageClass_OneZoneInfrequentAccess;
								else if (Value == "REDUCED_REDUNDANCY")
									Object.m_StorageClass = EStorageClass_ReducedRedundancy;
								else if (Value == "GLACIER")
									Object.m_StorageClass = EStorageClass_Glacier;
							}
						}

						if (Results.f_GetChildValue(pListBucketResult, "IsTruncated", false))
						{
							CStr ContinuationToken = Results.f_GetChildValue(pListBucketResult, "NextContinuationToken", CStr{});
							if (!ContinuationToken)
								return fReportInvalidXML("NextContinuationToken");

							_fDoRequest(_fDoRequest, ContinuationToken);
							return;
						}
						pResult->m_BucketName = Results.f_GetChildValue(pListBucketResult, "Name", _BucketName);

						Promise.f_SetResult(*pResult);
					}
				;
			}
		;

		fDoRequest(fDoRequest, "");

		return Promise.f_MoveFuture();
	}

	namespace
	{
		TCMap<CStr, CStr> fg_GetPutHeaders(CAwsS3Actor::CPutObjectInfo const &_Info)
		{
			TCMap<CStr, CStr> Headers;

			if (_Info.m_CacheControl)
				Headers["Cache-Control"] = *_Info.m_CacheControl;

			if (_Info.m_ContentDisposition)
				Headers["Content-Disposition"] = *_Info.m_ContentDisposition;

			if (_Info.m_ContentEncoding)
				Headers["Content-Encoding"] = *_Info.m_ContentEncoding;

			if (_Info.m_ContentType)
				Headers["Content-Type"] = *_Info.m_ContentType;

			if (_Info.m_RedirectLocation)
				Headers["x-amz-website​-redirect-location"] = *_Info.m_RedirectLocation;

			for (auto &MetaValue : _Info.m_MetaData)
				Headers[CStr{"x-amz-meta-{}"_f << _Info.m_MetaData.fs_GetKey(MetaValue)}] = MetaValue;

			if (_Info.m_StorageClass != CAwsS3Actor::EStorageClass_Unknown)
			{
				CStr Class;
				switch (_Info.m_StorageClass)
				{
					case CAwsS3Actor::EStorageClass_Standard: Class = "STANDARD"; break;
					case CAwsS3Actor::EStorageClass_StandardInfrequentAccess: Class = "STANDARD_IA"; break;
					case CAwsS3Actor::EStorageClass_OneZoneInfrequentAccess: Class = "ONEZONE_IA"; break;
					case CAwsS3Actor::EStorageClass_ReducedRedundancy: Class = "REDUCED_REDUNDANCY"; break;
					default: DMibNeverGetHere; break;
				}
				if (!Class.f_IsEmpty())
					Headers["x-amz-storage-class"] = Class;
			}

			if (!_Info.m_Tags.f_IsEmpty())
			{
				CStr QueryParams;
				for (auto &Value : _Info.m_Tags)
				{
					CStr ParamName;
					CStr ParamValue;

					NHTTP::CURL::fs_PercentEncode(ParamName, _Info.m_Tags.fs_GetKey(Value), nullptr, true);
					NHTTP::CURL::fs_PercentEncode(ParamValue, Value, nullptr, true);

					fg_AddStrSep(QueryParams, "{}={}"_f << ParamName << ParamValue, "&");
				}

				Headers["x-amz-tagging"] = QueryParams;
			}

			return Headers;
		}
	}

	NConcurrency::TCFuture<void> CAwsS3Actor::f_PutObject(NStr::CStr const &_BucketName, NStr::CStr const &_Key, CPutObjectInfo const &_Info, NContainer::CByteVector &&_Data)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://s3-{}.amazonaws.com/{}/{}"_f << Internal.m_Credentials.m_Region << _BucketName << _Key};

		TCPromise<void> Promise;

		auto AWSHeaders = fg_GetPutHeaders(_Info);
		AWSHeaders["Content-Length"] = "{}"_f << _Data.f_GetLen();
		auto Digest = NCryptography::CHash_MD5::fs_DigestFromData(_Data);
		AWSHeaders["Content-MD5"] = NEncoding::fg_Base64Encode(CByteVector(Digest.f_GetData(), Digest.fs_GetSize()));

		fg_DoAWSRequestXML("Put object", Internal.m_CurlActor, 200, AWSUrl, _Data, CCurlActor::EMethod_PUT, Internal.m_Credentials, fg_GetPutHeaders(_Info), "s3")
			> Promise / [=](NStorage::TCTuple<NXML::CXMLDocument, CCurlActor::CResult> &&_Result)
			{
				Promise.f_SetResult();
			}
		;

		return Promise.f_MoveFuture();
	}

	NConcurrency::TCFuture<void> CAwsS3Actor::f_PutObjectMultipart
		(
			NStr::CStr const &_BucketName
			, NStr::CStr const &_Key
			, CPutObjectInfo const &_Info
			, uint64 _TotalSize
		 	, NConcurrency::TCActorFunctor<NConcurrency::TCFuture<NContainer::CByteVector> ()> &&_fGetPart
		)
	{
		return DMibErrorInstance("Not implemented");
	}

	NConcurrency::TCFuture<void> CAwsS3Actor::f_DeleteObject(NStr::CStr const &_BucketName, NStr::CStr const &_Key)
	{
		auto &Internal = *mp_pInternal;
		NHTTP::CURL AWSUrl = CStr{"https://s3-{}.amazonaws.com/{}/{}"_f << Internal.m_Credentials.m_Region << _BucketName << _Key};

		TCPromise<void> Promise;

		fg_DoAWSRequestXML("Delete object", Internal.m_CurlActor, 204, AWSUrl, {}, CCurlActor::EMethod_DELETE, Internal.m_Credentials, {}, "s3")
			> Promise / [=](NStorage::TCTuple<NXML::CXMLDocument, CCurlActor::CResult> &&_Result)
			{
				Promise.f_SetResult();
			}
		;

		return Promise.f_MoveFuture();
	}
}
