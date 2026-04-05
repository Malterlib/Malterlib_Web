// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib::NWeb::NHTTP
{
	CResponseFields::CResponseFields()
	{

	}

	CResponseFields::~CResponseFields()
	{

	}



	EParse CResponseFields::f_ParseKnownField(EResponseField _Field, NStr::CStr const& _Value)
	{
		switch(_Field)
		{
			case EResponseField_Unknown:
			default:
				{
					DMibNeverGetHere;
					return EParse_Invalid;
				}
			case EResponseField_AcceptRanges:
			case EResponseField_Age:
			case EResponseField_ETag:
			case EResponseField_Location:
			case EResponseField_ProxyAuthenticate:
			case EResponseField_RetryAfter:
			case EResponseField_Server:
			case EResponseField_Vary:
			case EResponseField_WWWAuthenticate:
				{
					mp_Fields[_Field] = _Value;
					return EParse_OK;
				}
		}
	}

	EParse CResponseFields::f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value)
	{
		return EParse_Unknown;
	}



	NStr::CStr CResponseFields::f_GetAcceptRanges() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_AcceptRanges);
	}

	NStr::CStr CResponseFields::f_GetAge() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_Age);
	}

	NStr::CStr CResponseFields::f_GetETag() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_ETag);
	}

	NStr::CStr CResponseFields::f_GetLocation() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_Location);
	}

	NStr::CStr CResponseFields::f_GetProxyAuthenticate() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_ProxyAuthenticate);
	}

	NStr::CStr CResponseFields::f_GetRetryAfter() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_RetryAfter);
	}

	NStr::CStr CResponseFields::f_GetServer() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_Server);
	}

	NStr::CStr CResponseFields::f_GetVary() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_Vary);
	}

	NStr::CStr CResponseFields::f_GetWWWAuthenticate() const
	{
		return fp_GetField<EFieldType_String>(EResponseField_WWWAuthenticate);
	}



	void CResponseFields::f_SetAcceptRanges(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_AcceptRanges] = _Value;
	}

	void CResponseFields::f_SetAge(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_Age] = _Value;
	}

	void CResponseFields::f_SetETag(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_ETag] = _Value;
	}

	void CResponseFields::f_SetLocation(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_Location] = _Value;
	}

	void CResponseFields::f_SetProxyAuthenticate(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_ProxyAuthenticate] = _Value;
	}

	void CResponseFields::f_SetRetryAfter(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_RetryAfter] = _Value;
	}

	void CResponseFields::f_SetServer(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_Server] = _Value;
	}

	void CResponseFields::f_SetVary(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_Vary] = _Value;
	}

	void CResponseFields::f_SetWWWAuthenticate(NStr::CStr const& _Value)
	{
		mp_Fields[EResponseField_WWWAuthenticate] = _Value;
	}
}
