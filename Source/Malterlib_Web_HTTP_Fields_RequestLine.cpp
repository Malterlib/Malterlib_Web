// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Web_HTTP_Fields.h"
#include "Malterlib_Web_HTTP_Utilities.h"

namespace NMib::NWeb::NHTTP
{
	CRequestLine::CRequestLine()
		: mp_Method(EMethod_Unknown)
		, mp_Version(EVersion_Unknown)
	{

	}

	CRequestLine::~CRequestLine()
	{

	}

	void CRequestLine::f_Clear()
	{
		mp_Method = EMethod_Unknown;
		mp_Version = EVersion_Unknown;
		mp_URI.f_Clear();
	}

	EParse CRequestLine::f_Parse(NStr::CStr const& _RequestLine, NStr::CStr& _oErrors)
	{
		NContainer::TCVector<NStr::CStr> lReqParts = fg_SplitStringOn(_RequestLine, " ");

		if (lReqParts.f_GetLen() != 3)
		{
			_oErrors = "Request was malformed. Expended METHOD URI VERSION as first line.";
			return EParse_Invalid;
		}

		mp_Method = fg_HTTP_LookupMethod(lReqParts[0]);
		mp_URI = lReqParts[1];
		mp_Version = fg_HTTP_LookupVersion(lReqParts[2]);

		if (mp_Method == EMethod_Unknown)
		{
			_oErrors = NStr::fg_Format("Unknown method: \"{}\"", lReqParts[0]);
			return EParse_Invalid;
		}

		if (!mp_URI.f_IsValid())
		{
			_oErrors = NStr::fg_Format("Invalid URI: \"{}\"", lReqParts[1]);
			return EParse_Invalid;
		}

		if (mp_Version == EVersion_Unknown)
		{
			_oErrors = NStr::fg_Format("Unknown http version: \"{}\"", lReqParts[2]);
			return EParse_Invalid;
		}

		return EParse_OK;
	}

	void CRequestLine::f_Set(EVersion _Version, EMethod _Method, CURL const &_URI)
	{
		mp_Version = _Version;
		mp_Method = _Method;
		mp_URI = _URI;
	}

	void CRequestLine::f_Write(COutputMethod const &_fOutput)
	{
		NStr::CStr Line = NStr::fg_Format
			(
				"{} {} {}\r\n"
				, fg_HTTP_GetMethodName(mp_Method)
				, mp_URI.f_Encode()
				, fg_HTTP_GetVersionName(mp_Version)
			)
		;

		_fOutput((uint8 const*)Line.f_GetStr(), Line.f_GetLen());
	}

	EMethod CRequestLine::f_GetMethod() const
	{
		return mp_Method;
	}

	CURL const& CRequestLine::f_GetURI() const
	{
		return mp_URI;
	}

	EVersion CRequestLine::f_GetVersion() const
	{
		return mp_Version;
	}
}
