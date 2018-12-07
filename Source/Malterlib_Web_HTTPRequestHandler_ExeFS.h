// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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

		NTime::CTime m_ExeTime;

	public:
		CHTTPRequestHandler_ExeFs(NStr::CStr const& _ServerPath, NStr::CStr const& _ExeFsPath);

		~CHTTPRequestHandler_ExeFs();

		// This could be called for any thread - assume nothing.
		bint f_HandleRequest(CHTTPConnection &_Connection, CHTTPRequest const& _Req) override;
	};
}
