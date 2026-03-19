// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Fields.h"

namespace NMib::NWeb::NHTTP
{
	//
	// Global Utility Methods
	//

	struct CFieldValueAsStringVisitor
	{
	private:
		EFieldType mp_FieldType;
		bool mp_bOK;
		CStringValue mp_Result;

	public:

		CFieldValueAsStringVisitor(EFieldType _Type)
			: mp_FieldType(_Type)
			, mp_bOK(true)
		{}

		bool f_HasResult() const { return mp_bOK; }
		CStringValue f_GetResult() { return fg_Move(mp_Result); }

		template<typename t_CType>
		void operator()(t_CType const& _Value)
		{
			mp_bOK = false;
		}

	};

	template<>
	void CFieldValueAsStringVisitor::operator()<NStr::CStr>(NStr::CStr const& _Value)
	{
		mp_Result = CStringValue::fs_CreateDynamicString(_Value);
	}

	template<>
	void CFieldValueAsStringVisitor::operator()<umint>(umint const& _Value)
	{
		mp_Result = CStringValue::fs_CreateDynamicString(NStr::fg_Format("{}", _Value));
	}

	template<>
	void CFieldValueAsStringVisitor::operator()<ETransferEncoding>(ETransferEncoding const& _Value)
	{
		mp_Result = CStringValue::fs_CreateStaticString( fg_HTTP_GetTransferEncodingName(_Value) );
	}

	template<>
	void CFieldValueAsStringVisitor::operator()<EConnectionToken>(EConnectionToken const& _Value)
	{
		mp_Result = CStringValue::fs_CreateStaticString( fg_HTTP_GetConnectionTokenName(_Value) );
	}

	CStringValue fg_GetFieldValueAsString(CFieldValue const &_Value)
	{
		CFieldValueAsStringVisitor Visitor((EFieldType)_Value.f_GetTypeID());

		fg_Visit(Visitor, _Value);

		if (Visitor.f_HasResult())
		{
			return Visitor.f_GetResult();
		}
		else
		{
			return CStringValue::fs_CreateStaticString("");
		}
	}
}
