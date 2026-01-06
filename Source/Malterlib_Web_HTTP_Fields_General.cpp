// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib::NWeb::NHTTP
{
	//
	// CGeneralFields Public Methods
	//

	CGeneralFields::CGeneralFields()
	{

	}

	CGeneralFields::~CGeneralFields()
	{

	}

	EParse CGeneralFields::f_ParseKnownField(EGeneralField _Field, NStr::CStr const& _Value)
	{
		switch(_Field)
		{
			case EGeneralField_Unknown:
				{
					return EParse_Unknown;
				}

			default:
				{
					DMibNeverGetHere;
					return EParse_Invalid;
				}

			case EGeneralField_CacheControl:
				{
					mp_Fields[_Field] = _Value;
					return EParse_OK;
				}

			case EGeneralField_Connection:
				{
					EConnectionToken Token = fg_HTTP_LookupConnectionToken(_Value);
					if (Token == EConnectionToken_Unknown)
					{
						return EParse_Unknown;
					}

					mp_Fields[_Field] = Token;
					return EParse_OK;
				}

			case EGeneralField_Date:
			case EGeneralField_Pragma:
				{
					mp_Fields[_Field] = _Value;
					return EParse_OK;
				}

			case EGeneralField_Trailer:
				{
					// List field
					auto pValue = mp_Fields.f_FindEqual(_Field);
					if (pValue)
					{
						pValue->f_Get<EFieldType_String>() += ",";
						pValue->f_Get<EFieldType_String>() += _Value;
					}
					else
					{
						mp_Fields[_Field] = _Value;

					}
					return EParse_OK;
				}

			case EGeneralField_TransferEncoding:
				{
					ETransferEncoding Encoding = fg_HTTP_LookupTransferEncoding(_Value);
					if (Encoding == ETransferEncoding_Unknown)
						return EParse_Invalid;

					mp_Fields[_Field] = Encoding;
					return EParse_OK;
				}

			case EGeneralField_Upgrade:
			case EGeneralField_Via:
			case EGeneralField_Warning:
				{
					mp_Fields[_Field] = _Value;
					return EParse_OK;
				}
		}
	}

	EParse CGeneralFields::f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value)
	{
		return EParse_Unknown;
	}

	NStr::CStr CGeneralFields::f_GetCacheControl() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_CacheControl);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	EConnectionToken CGeneralFields::f_GetConnection() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_Connection);
		if (pValue)
			return pValue->f_Get<EFieldType_ConnectionToken>();
		else
		{
			// Default behaviour when the field is not specified
			// RFC 2616 Section 14.10 Connection
			switch(f_GetHTTPVersion())
			{
				case EVersion_HTTP_1_0:
				default:
					return EConnectionToken_Close;

				case EVersion_HTTP_1_1:
				case EVersion_HTTP_2_0:
					return EConnectionToken_KeepAlive;
			}
		}
	}

	NStr::CStr CGeneralFields::f_GetDate() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_Date);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CGeneralFields::f_GetPragma() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_Pragma);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CGeneralFields::f_GetTrailer() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_Trailer);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	ETransferEncoding CGeneralFields::f_GetTransferEncoding() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_TransferEncoding);
		if (pValue)
			return pValue->f_Get<EFieldType_TransferEncoding>();
		else
		{
			// Default behaviour when the field is not specified
			// RFC 2616 Section 14.41 Transfer-Encoding
			return ETransferEncoding_Identity;
		}
	}

	NStr::CStr CGeneralFields::f_GetUpgrade() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_Upgrade);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CGeneralFields::f_GetVia() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_Via);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}

	NStr::CStr CGeneralFields::f_GetWarning() const
	{
		auto pValue = mp_Fields.f_FindEqual(EGeneralField_Warning);
		if (pValue)
			return pValue->f_Get<EFieldType_String>();
		else
			return NStr::CStr();
	}


	void CGeneralFields::f_SetCacheControl(NStr::CStr const& _Value)
	{
		mp_Fields[EGeneralField_CacheControl] = _Value;
	}

	void CGeneralFields::f_SetConnection(EConnectionToken _Value)
	{
		mp_Fields[EGeneralField_Connection] = _Value;
	}

	namespace
	{
		ch8 const *g_WeekDays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
		ch8 const *g_Months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	}

	void CGeneralFields::f_SetDate(NTime::CTime const& _Value)
	{
		NTime::CTimeConvert::CDateTime DateTime;
		NTime::CTimeConvert(_Value).f_ExtractDateTime(DateTime);

		mp_Fields[EGeneralField_Date] = NStr::fg_Format
			(
				"{}, {sj2,sf0} {} {} {sf2,sj0}:{sf2,sj0}:{sf2,sj0} GMT"
				, g_WeekDays[DateTime.m_DayOfWeek]
				, DateTime.m_DayOfMonth
				, g_Months[DateTime.m_Month]
				, DateTime.m_Year
				, DateTime.m_Hour
				, DateTime.m_Minute
				, DateTime.m_Second
			)
		;
	}

	void CGeneralFields::f_SetDate(NStr::CStr const& _Value)
	{
		mp_Fields[EGeneralField_Date] = _Value;
	}

	void CGeneralFields::f_SetPragma(NStr::CStr const& _Value)
	{
		mp_Fields[EGeneralField_Pragma] = _Value;
	}

	void CGeneralFields::f_SetTrailer(NStr::CStr const& _Value)
	{
		mp_Fields[EGeneralField_Trailer] = _Value;
	}

	void CGeneralFields::f_SetTransferEncoding(ETransferEncoding _Value)
	{
		mp_Fields[EGeneralField_TransferEncoding] = _Value;
	}

	void CGeneralFields::f_SetUpgrade(NStr::CStr const& _Value)
	{
		mp_Fields[EGeneralField_Upgrade] = _Value;
	}

	void CGeneralFields::f_SetVia(NStr::CStr const& _Value)
	{
		mp_Fields[EGeneralField_Via] = _Value;
	}

	void CGeneralFields::f_SetWarning(NStr::CStr const& _Value)
	{
		mp_Fields[EGeneralField_Warning] = _Value;
	}
}
