// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib::NWeb::NHTTP
{
	//
	// Global Utility Methods
	//

	struct CFieldValueAsStringVisitor
	{
		CFieldValueAsStringVisitor(EFieldType _Type);

		bool f_HasResult() const;
		CStringValue f_GetResult();

		template <typename tf_CType>
		void operator () (tf_CType const &_Value);

	private:
		EFieldType mp_FieldType;
		bool mp_bOK;
		CStringValue mp_Result;
	};

	CFieldValueAsStringVisitor::CFieldValueAsStringVisitor(EFieldType _Type)
		: mp_FieldType(_Type)
		, mp_bOK(true)
	{
	}

	bool CFieldValueAsStringVisitor::f_HasResult() const
	{
		return mp_bOK;
	}

	CStringValue CFieldValueAsStringVisitor::f_GetResult()
	{
		return fg_Move(mp_Result);
	}

	template <typename tf_CType>
	void CFieldValueAsStringVisitor::operator () (tf_CType const &_Value)
	{
		mp_bOK = false;
	}

	template <>
	void CFieldValueAsStringVisitor::operator ()<NStr::CStr>(NStr::CStr const &_Value)
	{
		mp_Result = CStringValue::fs_CreateDynamicString(_Value);
	}

	template <>
	void CFieldValueAsStringVisitor::operator ()<umint>(umint const &_Value)
	{
		mp_Result = CStringValue::fs_CreateDynamicString(NStr::fg_Format("{}", _Value));
	}

	template <>
	void CFieldValueAsStringVisitor::operator ()<ETransferEncoding>(ETransferEncoding const &_Value)
	{
		mp_Result = CStringValue::fs_CreateStaticString(fg_HTTP_GetTransferEncodingName(_Value));
	}

	template <>
	void CFieldValueAsStringVisitor::operator ()<EConnectionToken>(EConnectionToken const &_Value)
	{
		mp_Result = CStringValue::fs_CreateStaticString(fg_HTTP_GetConnectionTokenName(_Value));
	}

	CStringValue fg_GetFieldValueAsString(CFieldValue const &_Value)
	{
		CFieldValueAsStringVisitor Visitor((EFieldType)_Value.f_GetTypeID());

		fg_Visit(Visitor, _Value);

		if (Visitor.f_HasResult())
			return Visitor.f_GetResult();
		else
			return CStringValue::fs_CreateStaticString("");
	}

	namespace NPrivate
	{
		NStr::CStr TCFieldsBase_GetDefaultValue<NStr::CStr>::fs_Default()
		{
			return {};
		}
	}
}
