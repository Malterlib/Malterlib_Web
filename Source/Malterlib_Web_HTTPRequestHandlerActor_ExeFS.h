// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include "Malterlib_Web_HTTPServer.h"
#include <Mib/File/VirtualFS>
#include <Mib/File/ExeFS>

#include <Mib/Concurrency/ActorFunctorWeak>

namespace NMib::NWeb
{
	struct CHTTPRequestHandlerActor_ExeFs : public CHTTPRequestHandlerActor
	{
		using CActorHolder = NConcurrency::CSeparateThreadActorHolder;
		using FCheckAccess = NConcurrency::TCActorFunctorWeak
			<
				NConcurrency::TCFuture<bool>
				(
					NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection
					, NStorage::TCSharedPointer<CHTTPRequest> const &_pRequest
				)
			>
		;

		CHTTPRequestHandlerActor_ExeFs(NStr::CStr const &_ServerPath, NStr::CStr const &_ExeFsPath, FCheckAccess &&_fCheckAccess = nullptr);
		~CHTTPRequestHandlerActor_ExeFs();

		NConcurrency::TCFuture<bool> f_HandleRequest(NStorage::TCSharedPointer<CHTTPConnection> const &_pConnection, NStorage::TCSharedPointer<CHTTPRequest> const &_pRequest);

	private:
		NStr::CStr mp_Path;
		NStr::CStr mp_ExeFsPath;
		NThread::CMutual mp_Lock;
		NMib::NFile::CExeFS mp_ExeFs;
		NStorage::TCUniquePointer<NFile::ICFileSystemInterface> mp_pFSInterface;
		FCheckAccess mp_fCheckAccess;

		NTime::CTime mp_ExeTime;
	};
}
