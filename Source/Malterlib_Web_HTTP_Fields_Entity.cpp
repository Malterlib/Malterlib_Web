// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib
{

	namespace NHTTP
	{

		//
		// CEntityFields Public Methods
		//

		CEntityFields::CEntityFields()
		{
		}

		CEntityFields::~CEntityFields()
		{

		}
		
		// Returns:
		//	EParse_OK 			- The field was known and parsed OK
		//	EParse_Invalid 		- The field was known but the data was invalud
		//	EParse_NotPresent 	- The field was not known
		EParse CEntityFields::f_ParseKnownField(EEntityField _Field, NStr::CStr const& _Value)
		{
			switch(_Field)
			{
				case EEntityField_Unknown:
				default:
					{
						DMibNeverGetHere;
						return EParse_Invalid;
					}
				case EEntityField_ContentEncoding:
				case EEntityField_ContentLanguage:
					{
						mp_Fields[_Field] = _Value;
						return EParse_OK;
					}

				case EEntityField_ContentLength:
					{
						aint nParsed;
						mint ContentLength;
						(NStr::CStr::CParse("{}") >> ContentLength).f_Parse(_Value, nParsed);
						if (nParsed != 1)
							return EParse_Invalid;
						mp_Fields[_Field] = ContentLength;

						return EParse_OK;
					}

				case EEntityField_ContentLocation:
				case EEntityField_ContentMD:
				case EEntityField_ContentRange:
				case EEntityField_ContentType:
				case EEntityField_Expires:
				case EEntityField_LastModified:
				case EEntityField_ExtensionHeader:
					{
						mp_Fields[_Field] = _Value;
						return EParse_OK;
					}
			}
		}

		EParse CEntityFields::f_ParseUnknownField(NStr::CStr const& _Name, NStr::CStr const& _Value)
		{
			mp_UnknownFields[_Name] = _Value;
			return EParse_Unknown;
		}

		NStr::CStr CEntityFields::f_GetContentEncoding() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_ContentEncoding);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		NStr::CStr CEntityFields::f_GetContentLanguage() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_ContentLanguage);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		mint CEntityFields::f_GetContentLength() const
		{
			auto pContentLength = mp_Fields.f_FindEqual(EEntityField_ContentLength);
			if (pContentLength)
				return pContentLength->f_Get<EFieldType_Mint>();
			else
				return 0;
		}

		NStr::CStr CEntityFields::f_GetContentLocation() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_ContentLocation);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		NStr::CStr CEntityFields::f_GetContentMD() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_ContentMD);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		NStr::CStr CEntityFields::f_GetContentRange() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_ContentRange);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		NStr::CStr CEntityFields::f_GetContentType() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_ContentType);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		NStr::CStr CEntityFields::f_GetExpires() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_Expires);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		NStr::CStr CEntityFields::f_GetLastModified() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_LastModified);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		NStr::CStr CEntityFields::f_GetExtensionHeader() const
		{
			auto pValue = mp_Fields.f_FindEqual(EEntityField_ExtensionHeader);
			if (pValue)
				return pValue->f_Get<EFieldType_String>();
			else
				return NStr::CStr();
		}

		void CEntityFields::f_SetContentEncoding(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_ContentEncoding] = _Value;
		}

		void CEntityFields::f_SetContentLanguage(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_ContentLanguage] = _Value;
		}

		void CEntityFields::f_SetContentLength(mint _Value)
		{
			mp_Fields[EEntityField_ContentLength] = _Value;
		}

		void CEntityFields::f_SetContentLocation(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_ContentLocation] = _Value;
		}

		void CEntityFields::f_SetContentMD(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_ContentMD] = _Value;
		}

		void CEntityFields::f_SetContentRange(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_ContentRange] = _Value;
		}

		void CEntityFields::f_SetContentType(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_ContentType] = _Value;
		}

		void CEntityFields::f_SetExpires(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_Expires] = _Value;
		}

		void CEntityFields::f_SetLastModified(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_LastModified] = _Value;
		}

		void CEntityFields::f_SetExtensionHeader(NStr::CStr const& _Value)
		{
			mp_Fields[EEntityField_ExtensionHeader] = _Value;
		}
		
		void CEntityFields::f_SetUnknownField(NStr::CStr const &_Field, NStr::CStr const &_Value)
		{
			mp_UnknownFields[_Field] = _Value;
		}
		
		NStr::CStr CEntityFields::f_WriteToString() const
		{
			NStr::CStr Ret = TCFieldsBase<CEntityFields, EEntityField>::f_WriteToString();
			return Ret;
		}
		
		void CEntityFields::f_WriteToData(COutputMethod const &_fOutput) const
		{
			TCFieldsBase<CEntityFields, EEntityField>::f_WriteToData(_fOutput);
			
			char const pNameValueSep[] = { ':', ' ' };
			char const pLineSep[] = { '\r', '\n' };
			
			for (auto iField = mp_UnknownFields.f_GetIterator(); iField; ++iField)
			{
				_fOutput((uint8 const*)iField.f_GetKey().f_GetStr(), iField.f_GetKey().f_GetLen());
				_fOutput((uint8 const*)pNameValueSep, sizeof(pNameValueSep));

				_fOutput((uint8 const*)iField->f_GetStr(), iField->f_GetLen());
				_fOutput((uint8 const*)pLineSep, sizeof(pLineSep));
			}
		}		
		
		NStr::CStr const *CEntityFields::f_GetUnknownField(NStr::CStr const &_Field) const
		{
			return mp_UnknownFields.f_FindEqual(_Field);
		}
		


	} // Namespace NHTTP

} // Namespace NMib
