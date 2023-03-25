// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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

	EParse CStatusLine::f_Parse(NStr::CStr const& _StatusLine, NStr::CStr& _oErrors)
	{
		NContainer::TCVector<NStr::CStr> lReqParts;
		lReqParts.f_SetLen(3);

		NStr::CStr::CParse Parse("{} {} {}");;
		Parse >> lReqParts[0];
		Parse >> lReqParts[1];
		Parse >> lReqParts[2];

		aint nFound = 0;
		Parse.f_Parse(_StatusLine, nFound);

		if (nFound != 3)
		{
			_oErrors = "Status line was malformed. Expected VERSION STATUS REASON as first line.";
			return EParse_Invalid;
		}

		mp_Version = fg_HTTP_LookupVersion(lReqParts[0]);
		mint Status = lReqParts[1].f_ToIntExact(TCLimitsInt<mint>::mc_Max);
		if (Status == TCLimitsInt<mint>::mc_Max)
		{
			_oErrors = "Status line was malformed. Expected a valid HTTP status code.";
			return EParse_Invalid;
		}
		mp_Status = (EStatus)Status;
		mp_ReasonPhrase = lReqParts[2];

		if (mp_Version == EVersion_Unknown)
		{
			_oErrors = NStr::fg_Format("Unknown http version: \"{}\"", lReqParts[0]);
			return EParse_Invalid;
		}

		if (mp_Status == EStatus_Unknown)
		{
			// NOTE: Not actually an error, should class the message.
			// We should convert the status to it's class status:
			// mp_Status = ( ( (uint32)mp_Status ) / 100 ) * 100;
			//	TODO: Would check the new Status is valid here.
			_oErrors = NStr::fg_Format("Unknown status: \"{}\"", lReqParts[1]);
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

	NStr::CStr const& CStatusLine::f_GetReasonPhrase() const
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
		NStr::CStr Line = NStr::fg_Format
			(
				"{} {} {}\r\n"
				, fg_HTTP_GetVersionName(mp_Version)
				, (uint32)mp_Status
				, mp_ReasonPhrase
			)
		;

		_fOutput((uint8 const*)Line.f_GetStr(), Line.f_GetLen());
	}
}
