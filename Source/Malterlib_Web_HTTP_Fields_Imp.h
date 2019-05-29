// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NWeb::NHTTP
{
	//
	// Global Utility Methods
	//

	template <typename t_CEnum>
	NStr::CStr fg_WriteFieldsToString(NContainer::TCMap<t_CEnum, CFieldValue> const& _Fields)
	{
		NStr::CStr Result;
		for (auto iField = _Fields.f_GetIterator(); iField; ++iField)
		{
			Result += fg_HTTP_GetEnumName<t_CEnum>( iField.f_GetKey() );
			Result += ": ";
			Result += fg_GetFieldValueAsString(*iField).f_Get();
			Result += "\r\n";
		}

		return fg_Move(Result);
	}

	template <typename t_CEnum>
	void fg_WriteFieldsToData(COutputMethod const &_fOutput, NContainer::TCMap<t_CEnum, CFieldValue> const& _Fields)
	{
		char const pNameValueSep[] = { ':', ' ' };
		char const pLineSep[] = { '\r', '\n' };

		for (auto iField = _Fields.f_GetIterator(); iField; ++iField)
		{
			char const* pStr = fg_HTTP_GetEnumName<t_CEnum>(iField.f_GetKey());
			_fOutput((uint8 const*)pStr, NStr::fg_StrLen(pStr));
			_fOutput((uint8 const*)pNameValueSep, sizeof(pNameValueSep));

			CStringValue Value = fg_GetFieldValueAsString(*iField);
			_fOutput((uint8 const*)Value.f_Get(), NStr::fg_StrLen(Value.f_Get()));
			_fOutput((uint8 const*)pLineSep, sizeof(pLineSep));
		}
	}

	//
	// CStringValue Public Methods
	//

	CStringValue::CStringValue(NStr::CStr const& _Str)
		: mp_Storage(_Str)
	{
		mp_pStr = mp_Storage.f_GetStr();
	}

	CStringValue::CStringValue(char const* _pStr)
		: mp_pStr(_pStr)
	{
	}

	//
	// CStringValue Public Methods
	//

	CStringValue::CStringValue(CStringValue&& _ToMove)
		: mp_Storage(fg_Move(_ToMove.mp_Storage))
		, mp_pStr(_ToMove.mp_pStr)
	{
		_ToMove.f_Clear();
	}

	CStringValue::CStringValue()
		: mp_pStr(nullptr)
	{}

	void CStringValue::f_Clear()
	{
		mp_Storage.f_Clear();
		mp_pStr = nullptr;
	}

	CStringValue& CStringValue::operator=(CStringValue&& _ToMove)
	{
		mp_Storage = fg_Move(_ToMove.mp_Storage);
		mp_pStr = _ToMove.mp_pStr;
		_ToMove.f_Clear();

		return *this;
	}

	char const* CStringValue::f_Get() const
	{
		return mp_pStr;
	}

	CStringValue CStringValue::fs_CreateStaticString(char const* _pStr)
	{
		return CStringValue(_pStr);
	}

	CStringValue CStringValue::fs_CreateDynamicString(char const* _pStr)
	{
		return CStringValue(NStr::CStr(_pStr));
	}

	CStringValue CStringValue::fs_CreateDynamicString(NStr::CStr const& _Str)
	{
		return CStringValue(_Str);
	}


	//
	// TCFieldsBase Public Methods
	//

	template<typename t_CSub, typename t_CEnum>
	TCFieldsBase<t_CSub, t_CEnum>::TCFieldsBase()
		: mp_HTTPVersion(EVersion_HTTP_1_1)
	{

	}

	template<typename t_CSub, typename t_CEnum>
	TCFieldsBase<t_CSub, t_CEnum>::TCFieldsBase(TCFieldsBase&& _ToMove)
		: mp_HTTPVersion(_ToMove.mp_HTTPVersion)
		, mp_Fields(fg_Move(_ToMove.mp_Fields))
	{
		_ToMove.f_Clear();
	}

	template<typename t_CSub, typename t_CEnum>
	TCFieldsBase<t_CSub, t_CEnum>::~TCFieldsBase()
	{

	}

	template<typename t_CSub, typename t_CEnum>
	void TCFieldsBase<t_CSub, t_CEnum>::f_SetHTTPVersion(EVersion _Version)
	{
		mp_HTTPVersion = _Version;
	}

	template<typename t_CSub, typename t_CEnum>
	EVersion TCFieldsBase<t_CSub, t_CEnum>::f_GetHTTPVersion() const
	{
		return mp_HTTPVersion;
	}

	template<typename t_CSub, typename t_CEnum>
	void TCFieldsBase<t_CSub, t_CEnum>::f_Clear()
	{
		mp_Fields.f_Clear();
	}

	template<typename t_CSub, typename t_CEnum>
	bool TCFieldsBase<t_CSub, t_CEnum>::f_HasField(CEnum _Field) const
	{
		return mp_Fields.f_FindEqual(_Field) != nullptr;
	}

	// Returns:
	//	EParse_OK 			- The field was known and parsed OK
	//	EParse_Invalid 		- The field was known but the data was invalud
	//	EParse_Unknown	 	- The field was not known
	template<typename t_CSub, typename t_CEnum>
	EParse TCFieldsBase<t_CSub, t_CEnum>::f_ParseField(NStr::CStr const& _Name, NStr::CStr const& _Value)
	{
		CEnum Field = fg_HTTP_LookupEnum<CEnum>(_Name);

		if (Field != (CEnum)0)
		{ // Known field
			return static_cast<t_CSub*>(this)->f_ParseKnownField(Field, _Value);
		}
		else
		{ // Unknown field
			return static_cast<t_CSub*>(this)->f_ParseUnknownField(_Name, _Value);
		}
	}

	template<typename t_CSub, typename t_CEnum>
	void TCFieldsBase<t_CSub, t_CEnum>::f_ClearField(CEnum _Field)
	{
		mp_Fields.f_Remove(_Field);
	}

	template<typename t_CSub, typename t_CEnum>
	NStr::CStr TCFieldsBase<t_CSub, t_CEnum>::f_WriteToString() const
	{
		return fg_WriteFieldsToString(mp_Fields);
	}


	// Writes the header to the end of the paged vector
	template<typename t_CSub, typename t_CEnum>
	void TCFieldsBase<t_CSub, t_CEnum>::f_WriteToData(COutputMethod const &_fOutput) const
	{
		fg_WriteFieldsToData(_fOutput, mp_Fields);
	}

	//
	// TCFieldsBase Protected Methods
	//

	namespace NPrivate
	{
		template <typename t_CType>
		struct TCFieldsBase_GetDefaultValue
		{
			static t_CType fs_Default()
			{
				return 0;
			}
		};

		template <>
		struct TCFieldsBase_GetDefaultValue<NStr::CStr>
		{
			static NStr::CStr fs_Default()
			{
				return {};
			}
		};
	}

	template <typename t_CSub, typename t_CEnum>
	template <EFieldType t_FieldType>
	auto TCFieldsBase<t_CSub, t_CEnum>::fp_GetField(t_CEnum _Field) const ->CFieldValue::TCTypeFromMember<t_FieldType>
	{
		auto const* pValue = mp_Fields.f_FindEqual(_Field);
		if (pValue)
			return pValue->template f_Get<t_FieldType>();
		else
			return NPrivate::TCFieldsBase_GetDefaultValue<CFieldValue::TCTypeFromMember<t_FieldType>>::fs_Default();
	}
}
