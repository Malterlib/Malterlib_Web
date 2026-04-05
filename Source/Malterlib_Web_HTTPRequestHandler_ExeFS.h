// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "Malterlib_Web_HTTPServer.h"
#include <Mib/File/VirtualFS>
#include <Mib/File/ExeFS>

namespace NMib::NWeb
{
	class CHTTPRequestHandler_ExeFs : public CHTTPRequestHandler
	{
	private:
		NStr::CStr mp_Path;
		NStr::CStr mp_ExeFsPath;
		NThread::CMutual mp_Lock;
		NMib::NFile::CExeFS mp_ExeFs;
		NStorage::TCUniquePointer<NFile::ICFileSystemInterface> mp_pFSInterface;
		NFunction::TCFunction<bool (CHTTPConnection &_Connection, CHTTPRequest const& _Req)> mp_fCheckAccess;

		NTime::CTime m_ExeTime;

	public:
		CHTTPRequestHandler_ExeFs
			(
				NStr::CStr const& _ServerPath
				, NStr::CStr const& _ExeFsPath
				, NFunction::TCFunction<bool (CHTTPConnection &_Connection, CHTTPRequest const& _Req)> const &_fCheckAccess = nullptr
			)
		;

		~CHTTPRequestHandler_ExeFs();

		// This could be called for any thread - assume nothing.
		bool f_HandleRequest(CHTTPConnection &_Connection, CHTTPRequest const& _Req) override;
	};
}
