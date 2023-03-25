// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib::NWeb::NHTTP
{
	//
	// CRequestFields Public Methods
	//

	CRequestFields::CRequestFields()
	{
	}

	CRequestFields::~CRequestFields()
	{

	}

	// Returns:
	//	EParse_OK			- The field was known and parsed OK
	//	EParse_Invalid		- The field was known but the data was invalud
	//	EParse_NotPresent	- The field was not known
	EParse CRequestFields::f_ParseKnownField(ERequestField _Field, NStr::CStr const& _Value)
	{
		switch(_Field)
		{
			case ERequestField_Unknown:
				// Handled by super class
			default:
				{
					DMibNeverGetHere;
					return EParse_Invalid;
				}

			case ERequestField_Accept:
			case ERequestField_AcceptCharset:
			case ERequestField_AcceptEncoding:
			case ERequestField_AcceptLanguage:
			case ERequestField_Authorization:
			case ERequestField_Expect:
			case ERequestField_From:
			case ERequestField_Host:
			case ERequestField_IfMatch:
			case ERequestField_IfModifiedSince:
			case ERequestField_IfNoneMatch:
			case ERequestField_IfRange:
			case ERequestField_IfUnmodifiedSince:
			case ERequestField_MaxForwards:
			case ERequestField_ProxyAuthorization:
			case ERequestField_Range:
			case ERequestField_Referer:
			case ERequestField_TE:
			case ERequestField_UserAgent:
				{
					mp_Fields[_Field] = _Value;
					return EParse_OK;
				}
		}
	}

	EParse CRequestFields::f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value)
	{
		return EParse_Unknown;
	}

	NStr::CStr CRequestFields::f_GetAccept() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_Accept);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetAcceptCharset() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_AcceptCharset);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetAcceptEncoding() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_AcceptEncoding);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetAcceptLanguage() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_AcceptLanguage);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetAuthorization() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_Authorization);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetExpect() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_Expect);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetFrom() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_From);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetHost() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_Host);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetIfMatch() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_IfMatch);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetIfModifiedSince() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_IfModifiedSince);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetIfNoneMatch() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_IfNoneMatch);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetIfRange() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_IfRange);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetIfUnmodifiedSince() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_IfUnmodifiedSince);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetMaxForwards() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_MaxForwards);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetProxyAuthorization() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_ProxyAuthorization);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetRange() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_Range);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetReferer() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_Referer);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetTE() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_TE);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CRequestFields::f_GetUserAgent() const
	{
		auto pValue = mp_Fields.f_FindEqual(ERequestField_UserAgent);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}


	void CRequestFields::f_SetAccept(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_Accept] = _Value;
	}

	void CRequestFields::f_SetAcceptCharset(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_AcceptCharset] = _Value;
	}

	void CRequestFields::f_SetAcceptEncoding(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_AcceptEncoding] = _Value;
	}

	void CRequestFields::f_SetAcceptLanguage(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_AcceptLanguage] = _Value;
	}

	void CRequestFields::f_SetAuthorization(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_Authorization] = _Value;
	}

	void CRequestFields::f_SetExpect(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_Expect] = _Value;
	}

	void CRequestFields::f_SetFrom(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_From] = _Value;
	}

	void CRequestFields::f_SetHost(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_Host] = _Value;
	}

	void CRequestFields::f_SetIfMatch(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_IfMatch] = _Value;
	}

	void CRequestFields::f_SetIfModifiedSince(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_IfModifiedSince] = _Value;
	}

	void CRequestFields::f_SetIfNoneMatch(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_IfNoneMatch] = _Value;
	}

	void CRequestFields::f_SetIfRange(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_IfRange] = _Value;
	}

	void CRequestFields::f_SetIfUnmodifiedSince(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_IfUnmodifiedSince] = _Value;
	}

	void CRequestFields::f_SetMaxForwards(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_MaxForwards] = _Value;
	}

	void CRequestFields::f_SetProxyAuthorization(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_ProxyAuthorization] = _Value;
	}

	void CRequestFields::f_SetRange(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_Range] = _Value;
	}

	void CRequestFields::f_SetReferer(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_Referer] = _Value;
	}

	void CRequestFields::f_SetTE(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_TE] = _Value;
	}

	void CRequestFields::f_SetUserAgent(NStr::CStr const& _Value)
	{
		mp_Fields[ERequestField_UserAgent] = _Value;
	}
}
