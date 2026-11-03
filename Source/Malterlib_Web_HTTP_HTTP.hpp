// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb::NHTTP
{
	template <typename tf_CEnum>
	char const *fg_HTTP_GetEnumName(tf_CEnum _Value)
	{
		static_assert(NTraits::cIsSame<tf_CEnum, void>);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<EMethod>(EMethod _Value)
	{
		return fg_HTTP_GetMethodName(_Value);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<EVersion>(EVersion _Value)
	{
		return fg_HTTP_GetVersionName(_Value);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<ETransferEncoding>(ETransferEncoding _Value)
	{
		return fg_HTTP_GetTransferEncodingName(_Value);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<EConnectionToken>(EConnectionToken _Value)
	{
		return fg_HTTP_GetConnectionTokenName(_Value);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<EGeneralField>(EGeneralField _Value)
	{
		return fg_HTTP_GetGeneralFieldName(_Value);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<ERequestField>(ERequestField _Value)
	{
		return fg_HTTP_GetRequestFieldName(_Value);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<EResponseField>(EResponseField _Value)
	{
		return fg_HTTP_GetResponseFieldName(_Value);
	}

	template <>
	inline char const *fg_HTTP_GetEnumName<EEntityField>(EEntityField _Value)
	{
		return fg_HTTP_GetEntityFieldName(_Value);
	}

	template <typename tf_CEnum>
	tf_CEnum fg_HTTP_LookupEnum(NStr::CStr const &_Name)
	{
		static_assert(NTraits::cIsSame<tf_CEnum, void>);
	}

	template <>
	inline EMethod fg_HTTP_LookupEnum<EMethod>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupMethod(_Name);
	}

	template <>
	inline EVersion fg_HTTP_LookupEnum<EVersion>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupVersion(_Name);
	}

	template <>
	inline ETransferEncoding fg_HTTP_LookupEnum<ETransferEncoding>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupTransferEncoding(_Name);
	}

	template <>
	inline EConnectionToken fg_HTTP_LookupEnum<EConnectionToken>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupConnectionToken(_Name);
	}

	template <>
	inline EGeneralField fg_HTTP_LookupEnum<EGeneralField>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupGeneralField(_Name);
	}

	template <>
	inline ERequestField fg_HTTP_LookupEnum<ERequestField>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupRequestField(_Name);
	}

	template <>
	inline EResponseField fg_HTTP_LookupEnum<EResponseField>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupResponseField(_Name);
	}

	template <>
	inline EEntityField fg_HTTP_LookupEnum<EEntityField>(NStr::CStr const &_Name)
	{
		return fg_HTTP_LookupEntityField(_Name);
	}
}
