// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb
{
	template <typename tf_CStream>
	void CHttpClientRequestExceptionData::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_StatusCode;
		_Stream % m_StatusMessage;
	}
}
