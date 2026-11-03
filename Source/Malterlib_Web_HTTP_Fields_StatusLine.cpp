// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Container/PagedByteVector>
#include "Malterlib_Web_HTTP_Fields.h"
#include "Malterlib_Web_HTTP_Utilities.h"

namespace NMib::NWeb::NHTTP
{
	CStatusLine::CStatusLine()
		: mp_Version(EVersion_Unknown)
		, mp_Status(EStatus_Unknown)
	{
	}

	CStatusLine::~CStatusLine()
	{
	}

	void CStatusLine::f_Clear()
	{
		mp_Version = EVersion_Unknown;
		mp_Status = EStatus_Unknown;
		mp_ReasonPhrase.f_Clear();
	}

	EParse CStatusLine::f_Parse(NStr::CStr const &_StatusLine, NStr::CStr &o_Errors)
	{
		NContainer::TCVector<NStr::CStr> StatusParts;
		StatusParts.f_SetLen(3);

		NStr::CStr::CParse Parse("{} {} {}");
		Parse >> StatusParts[0];
		Parse >> StatusParts[1];
		Parse >> StatusParts[2];

		aint nFound = 0;
		Parse.f_Parse(_StatusLine, nFound);

		if (nFound != 3)
		{
			o_Errors = "Status line was malformed. Expected VERSION STATUS REASON as first line.";
			return EParse_Invalid;
		}

		mp_Version = fg_HTTP_LookupVersion(StatusParts[0]);
		umint Status = StatusParts[1].f_ToIntExact(TCLimitsInt<umint>::mc_Max);
		if (Status == TCLimitsInt<umint>::mc_Max)
		{
			o_Errors = "Status line was malformed. Expected a valid HTTP status code.";
			return EParse_Invalid;
		}
		mp_Status = (EStatus)Status;
		mp_ReasonPhrase = StatusParts[2];

		if (mp_Version == EVersion_Unknown)
		{
			o_Errors = NStr::fg_Format("Unknown http version: \"{}\"", StatusParts[0]);
			return EParse_Invalid;
		}

		if (mp_Status == EStatus_Unknown)
		{
			// NOTE: Not actually an error, should class the message.
			// We should convert the status to it's class status:
			// mp_Status = ( ( (uint32)mp_Status ) / 100 ) * 100;
			//	TODO: Would check the new Status is valid here.
			o_Errors = NStr::fg_Format("Unknown status: \"{}\"", StatusParts[1]);
			return EParse_Invalid;
		}

		return EParse_OK;
	}

	EVersion CStatusLine::f_GetVersion() const
	{
		return mp_Version;
	}

	EStatus CStatusLine::f_GetStatus() const
	{
		return mp_Status;
	}

	NStr::CStr const &CStatusLine::f_GetReasonPhrase() const
	{
		return mp_ReasonPhrase;
	}

	void CStatusLine::f_Set(EVersion _Version, EStatus _Status, NStr::CStr const &_CustomReason)
	{
		mp_Version = _Version;
		mp_Status = _Status;
		if (_CustomReason.f_IsEmpty())
			mp_ReasonPhrase = fg_HTTP_GetReasonPhrase(mp_Status);
		else
			mp_ReasonPhrase = _CustomReason;
	}

	void CStatusLine::f_Write(COutputMethod const &_fOutput)
	{
		NStr::CStr Line = NStr::fg_Format("{} {} {}\r\n", fg_HTTP_GetVersionName(mp_Version), (uint32)mp_Status, mp_ReasonPhrase);

		_fOutput((uint8 const *)Line.f_GetStr(), Line.f_GetLen());
	}
}
