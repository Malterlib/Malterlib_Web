// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

namespace NMib::NWeb
{
	template <typename tf_CStream>
	void CAwsErrorData::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ErrorCode;
		_Stream % m_StatusCode;
	}
}
