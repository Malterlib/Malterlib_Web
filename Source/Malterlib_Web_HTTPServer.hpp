// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NWeb
{
	// Lambda Sig: CStr Func(CHTTPConnection & _Conn, CStr const& _BlockName);
	template <typename tf_FFunc>
	void CHTMLTemplate::f_WriteTemplate(CHTTPConnection &_Conn, tf_FFunc const &_fFunc)
	{
		CHTTPResponseHeader ResponseHeader;
		f_WriteTemplate(_Conn, ResponseHeader, _fFunc);
	}

	// Lambda Sig: CStr Func(CHTTPConnection & _Conn, CStr const& _BlockName);
	template <typename tf_FFunc>
	void CHTMLTemplate::f_WriteTemplate(CHTTPConnection &_Conn, CHTTPResponseHeader const &_BaseHeader, tf_FFunc const &_fFunc)
	{
		_Conn.f_WriteHeader(_BaseHeader);

		for (auto &Block : mp_pState->m_Blocks)
		{
			if (Block.m_Type == EBlock_HTML)
				_Conn.f_WriteStr(Block.m_Text);
			else if (Block.m_Type == EBlock_UserData)
				_fFunc(_Conn, Block.m_Text);
		}
	}
}
