// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>

namespace NMib::NWeb::NHTTP
{
	// Not part of the HTTP protocol but used system wide:
	enum EParse
	{
			EParse_OK
		,	EParse_NotPresent
		,	EParse_Invalid
		,	EParse_Incomplete
		,	EParse_Unknown
	};

	//
	// HTTP Protocol Enums
	//

	// RFC 2616 Section 5.1.1
	enum EMethod
	{
			EMethod_Unknown
		,	EMethod_Options
		,	EMethod_Get
		,	EMethod_Head
		,	EMethod_Post
		,	EMethod_Put
		,	EMethod_Delete
		,	EMethod_Trace
		,	EMethod_Connect
	};

	// RFC 2616 Section 5.1
	enum EVersion
	{
			EVersion_Unknown
		,	EVersion_HTTP_1_0
		,	EVersion_HTTP_1_1
		,	EVersion_HTTP_2_0
	};

	// RFC 2616 Section 14.10
	enum EConnectionToken
	{
			EConnectionToken_Unknown
		,	EConnectionToken_KeepAlive
		,	EConnectionToken_Close
		,	EConnectionToken_Upgrade
	};

	// RFC 2616 Section 14.41
	enum ETransferEncoding
	{
			ETransferEncoding_Unknown
		,	ETransferEncoding_Chunked
		,	ETransferEncoding_Identity
		,	ETransferEncoding_GZip
		,	ETransferEncoding_Deflate
		,	ETransferEncoding_Compress
	};

	// RFC 2616 Section 4.5
	enum EGeneralField
	{
			EGeneralField_Unknown
		,	EGeneralField_CacheControl
		,	EGeneralField_Connection
		,	EGeneralField_Date
		,	EGeneralField_Pragma
		,	EGeneralField_Trailer
		,	EGeneralField_TransferEncoding
		,	EGeneralField_Upgrade
		,	EGeneralField_Via
		,	EGeneralField_Warning
	};

	// RFC 2616 Section 5.3
	enum ERequestField
	{
			ERequestField_Unknown
		,	ERequestField_Accept
		,	ERequestField_AcceptCharset
		,	ERequestField_AcceptEncoding
		,	ERequestField_AcceptLanguage
		,	ERequestField_Authorization
		,	ERequestField_Expect
		,	ERequestField_From
		,	ERequestField_Host
		,	ERequestField_IfMatch

		,	ERequestField_IfModifiedSince
		,	ERequestField_IfNoneMatch
		,	ERequestField_IfRange
		,	ERequestField_IfUnmodifiedSince
		,	ERequestField_MaxForwards
		,	ERequestField_ProxyAuthorization
		,	ERequestField_Range
		,	ERequestField_Referer
		,	ERequestField_TE
		,	ERequestField_UserAgent
	};

	// RFC 2616 Section 7.1
	enum EEntityField
	{
			EEntityField_Unknown
		,	EEntityField_ContentEncoding
		,	EEntityField_ContentLanguage
		,	EEntityField_ContentLength
		,	EEntityField_ContentLocation
		,	EEntityField_ContentMD
		,	EEntityField_ContentRange
		,	EEntityField_ContentType
		,	EEntityField_Expires
		,	EEntityField_LastModified
		,	EEntityField_ExtensionHeader
	};

	// RFC 2616 Section 6.2
	enum EResponseField
	{
			EResponseField_Unknown
		,	EResponseField_AcceptRanges
		,	EResponseField_Age
		,	EResponseField_ETag
		,	EResponseField_Location
		,	EResponseField_ProxyAuthenticate
		,	EResponseField_RetryAfter
		,	EResponseField_Server
		,	EResponseField_Vary
		,	EResponseField_WWWAuthenticate
	};

	// RFC 2616 Section 6.1.1
	enum EStatus
	{
			EStatus_Unknown					= 0 		// Internal use

		,	EStatus_Continue				= 100
		,	EStatus_SwitchingProtocols		= 101
		,	EStatus_OK						= 200
		,	EStatus_Created 				= 201
		,	EStatus_Accepted 				= 202
		,	EStatus_NonAuthoritativeInfo	 = 203
		,	EStatus_NoContent 				= 204
		,	EStatus_ResetContent 			= 205
		,	EStatus_PartialContent	 		= 206
		,	EStatus_MultipleChoices 		= 300
		,	EStatus_MovedPermanently 		= 301
		,	EStatus_Found 					= 302
		,	EStatus_SeeOther 				= 303
		,	EStatus_NotModified 			= 304
		,	EStatus_UseProxy 				= 305
		,	EStatus_TemporaryRedirect 		= 307
		,	EStatus_BadRequest 				= 400
		,	EStatus_Unauthorized 			= 401
		,	EStatus_PaymentRequired 		= 402
		,	EStatus_Forbidden 				= 403
		,	EStatus_NotFound 				= 404
		,	EStatus_MethodNotAllowed 		= 405
		,	EStatus_NotAcceptable 			= 406

		,	EStatus_ProxyAuthRequired 		= 407
		,	EStatus_RequestTimeOut	 		= 408
		,	EStatus_Conflict 				= 409
		,	EStatus_Gone 					= 410
		,	EStatus_LengthRequired	 		= 411
		,	EStatus_PreconditionFailed 		= 412
		,	EStatus_RequestEntityTooLarge 	= 413
		,	EStatus_RequestURITooLarge	 	= 414
		,	EStatus_UnsupportedMediaType 	= 415
		,	EStatus_RequestedRangeNotSatisfiable = 416
		,	EStatus_ExpectationFailed 		= 417
		,	EStatus_InternalServerError 	= 500
		,	EStatus_NotImplemented 			= 501
		,	EStatus_BadGateway 				= 502
		,	EStatus_ServiceUnavailable	 	= 503
		,	EStatus_GatewayTimeOut	 		= 504
		,	EStatus_HTTPVersionNotSupported	= 505
	};

	//
	// Utility methods for parsing and naming enums
	//

	char const* fg_HTTP_GetMethodName(EMethod _Method);
	char const* fg_HTTP_GetVersionName(EVersion _Version);
	char const* fg_HTTP_GetTransferEncodingName(ETransferEncoding _Encoding);
	char const* fg_HTTP_GetConnectionTokenName(EConnectionToken _Token);
	char const* fg_HTTP_GetGeneralFieldName(EGeneralField _Field);
	char const* fg_HTTP_GetRequestFieldName(ERequestField _Field);
	char const* fg_HTTP_GetResponseFieldName(EResponseField _Field);
	char const* fg_HTTP_GetEntityFieldName(EEntityField _Field);

	EMethod fg_HTTP_LookupMethod(NStr::CStr const& _Method);
	EVersion fg_HTTP_LookupVersion(NStr::CStr const& _Version);
	ETransferEncoding fg_HTTP_LookupTransferEncoding(NStr::CStr const& _Encoding);
	EConnectionToken fg_HTTP_LookupConnectionToken(NStr::CStr const& _Token);
	EGeneralField fg_HTTP_LookupGeneralField(NStr::CStr const& _Field);
	ERequestField fg_HTTP_LookupRequestField(NStr::CStr const& _Field);
	EResponseField fg_HTTP_LookupResponseField(NStr::CStr const& _Field);
	EEntityField fg_HTTP_LookupEntityField(NStr::CStr const& _Field);

	char const* fg_HTTP_GetReasonPhrase(EStatus _Status);


	template<typename t_CEnum>
	char const* fg_HTTP_GetEnumName(t_CEnum _Value)
	{
		static_assert(NTraits::TCIsSame<t_CEnum, void>::mc_Value);
	}

	template<>
	inline char const* fg_HTTP_GetEnumName<EMethod>(EMethod _Value)
	{ return fg_HTTP_GetMethodName(_Value); }

	template<>
	inline char const* fg_HTTP_GetEnumName<EVersion>(EVersion _Value)
	{ return fg_HTTP_GetVersionName(_Value); }

	template<>
	inline char const* fg_HTTP_GetEnumName<ETransferEncoding>(ETransferEncoding _Value)
	{ return fg_HTTP_GetTransferEncodingName(_Value); }

	template<>
	inline char const* fg_HTTP_GetEnumName<EConnectionToken>(EConnectionToken _Value)
	{ return fg_HTTP_GetConnectionTokenName(_Value); }

	template<>
	inline char const* fg_HTTP_GetEnumName<EGeneralField>(EGeneralField _Value)
	{ return fg_HTTP_GetGeneralFieldName(_Value); }

	template<>
	inline char const* fg_HTTP_GetEnumName<ERequestField>(ERequestField _Value)
	{ return fg_HTTP_GetRequestFieldName(_Value); }

	template<>
	inline char const* fg_HTTP_GetEnumName<EResponseField>(EResponseField _Value)
	{ return fg_HTTP_GetResponseFieldName(_Value); }

	template<>
	inline char const* fg_HTTP_GetEnumName<EEntityField>(EEntityField _Value)
	{ return fg_HTTP_GetEntityFieldName(_Value); }


	template<typename t_CEnum>
	t_CEnum fg_HTTP_LookupEnum(NStr::CStr const& _Name)
	{
		static_assert(NTraits::TCIsSame<t_CEnum, void>::mc_Value);
	}

	template<>
	inline EMethod fg_HTTP_LookupEnum<EMethod>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupMethod(_Name); }

	template<>
	inline EVersion fg_HTTP_LookupEnum<EVersion>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupVersion(_Name); }

	template<>
	inline ETransferEncoding fg_HTTP_LookupEnum<ETransferEncoding>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupTransferEncoding(_Name); }

	template<>
	inline EConnectionToken fg_HTTP_LookupEnum<EConnectionToken>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupConnectionToken(_Name); }

	template<>
	inline EGeneralField fg_HTTP_LookupEnum<EGeneralField>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupGeneralField(_Name); }

	template<>
	inline ERequestField fg_HTTP_LookupEnum<ERequestField>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupRequestField(_Name); }

	template<>
	inline EResponseField fg_HTTP_LookupEnum<EResponseField>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupResponseField(_Name); }

	template<>
	inline EEntityField fg_HTTP_LookupEnum<EEntityField>(NStr::CStr const& _Name)
	{ return fg_HTTP_LookupEntityField(_Name); }

}

#include "Malterlib_Web_HTTP_HTTP_Imp.h"
#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb::NHTTP;
#endif
