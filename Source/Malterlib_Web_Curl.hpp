// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NWeb
{
	template <typename tf_CStream>
	void CWebRequestExceptionData::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_StatusCode;
		_Stream % m_StatusMessage;
	}
}
