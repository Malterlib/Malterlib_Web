// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

namespace NMib::NWeb
{
	template <typename tf_CStream>
	void CAwsErrorData::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ErrorCode;
		_Stream % m_StatusCode;
	}
}
