// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb::NHTTP
{
	template <typename tf_CEnum>
	void fg_WriteFieldsToData(COutputMethod const &_fOutput, NContainer::TCMap<tf_CEnum, CFieldValue> const &_Fields);

	namespace NPrivate
	{
		template <typename t_CType>
		struct TCFieldsBase_GetDefaultValue
		{
			static t_CType fs_Default();
		};

		template <>
		struct TCFieldsBase_GetDefaultValue<NStr::CStr>
		{
			static NStr::CStr fs_Default();
		};
	}
}

#include "Malterlib_Web_HTTP_Fields_Imp.hpp"
