// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTPRequestHandlerActor_ExeFS.h"

#include <Mib/File/MalterlibFS>
#include <Mib/File/VirtualFSs/MalterlibFS>
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NWeb
{
	using namespace NConcurrency;
	using namespace NStorage;
	using namespace NStr;

	CHTTPRequestHandlerActor_ExeFs::CHTTPRequestHandlerActor_ExeFs(CStr const& _ServerPath, CStr const& _ExeFsPath, FCheckAccess &&_fCheckAccess)
		: mp_Path(_ServerPath)
		, mp_ExeFsPath(_ExeFsPath)
		, mp_fCheckAccess(fg_Move(_fCheckAccess))
	{
		g_Dispatch(fg_ThisActor(this)) / [this]
			{
				if (NMib::NFile::fg_OpenExeFS(mp_ExeFs))
				{
					mp_pFSInterface = fg_Construct<NFile::CFileSystemInterface_VirtualFS>(mp_ExeFs.m_FileSystem);
				}

				try
				{
					CStr ExeFilename = NSys::NFile::fg_GetProgramPath();
					TCUniquePointer<NFile::CFile> pExeFile = fg_Construct();
					pExeFile->f_Open(ExeFilename, NFile::EFileOpen_ReadAttribs | NFile::EFileOpen_ShareAll);
					mp_ExeTime = pExeFile->f_GetCreationTime().f_ToUTC();
				}
				catch (NFile::CExceptionFile)
				{
					mp_ExeTime = NTime::CTime::fs_NowUTC();
				}
			}
			> g_DiscardResult;
		;
	}

	CHTTPRequestHandlerActor_ExeFs::~CHTTPRequestHandlerActor_ExeFs()
	{
	}

	TCFuture<bool> CHTTPRequestHandlerActor_ExeFs::f_HandleRequest(TCSharedPointer<CHTTPConnection> _pConnection, TCSharedPointer<CHTTPRequest> _pRequest)
	{
		if (mp_fCheckAccess)
		{
			if (!co_await mp_fCheckAccess(_pConnection, _pRequest))
				co_return true;
		}

		auto ExceptionCapture = co_await g_CaptureExceptions;

		if (!mp_pFSInterface)
			co_return false;

		auto &Request = *_pRequest;
		auto &Connection = *_pConnection;

		CStr Filename = Request.m_RequestedURI;

		int iPos = Filename.f_Find(mp_Path);
		if (iPos != 0)
			co_return false;

		Filename = mp_ExeFsPath + Filename.f_Extract(mp_Path.f_GetLen());

		if (Filename.f_Right(1) == "/")
			co_return false;

		NCryptography::CHash_SHA256 Hash;
		Hash.f_AddData(&mp_ExeTime, sizeof(mp_ExeTime));
		Hash.f_AddData(Filename.f_GetStr(), Filename.f_GetLen());

		CStr HashTag = Hash.f_GetDigest().f_GetString();

		NConcurrency::TCFutureQueue<void> QueuedWrites(16);

		auto fWriteHeader = [&](CMibFilePos _nBytes) -> TCUnsafeFuture<bool>
			{
				CHTTPResponseHeader ResponseHeader;
				ResponseHeader.f_SetMimeTypeFromFilename(Filename);
				ResponseHeader.m_LastModified = mp_ExeTime;
				ResponseHeader.m_ContentLength = _nBytes;
				ResponseHeader.m_AllowMethods = "GET, HEAD";
				ResponseHeader.m_CacheControl = "no-cache";
				ResponseHeader.m_ETag = HashTag;

				if (auto *pIfNoneMatch = Request.m_Headers.f_FindEqual("if_none_match"); pIfNoneMatch)
				{
					if (*pIfNoneMatch == HashTag)
						ResponseHeader.m_Status = 304;
				}

				if (auto Future = QueuedWrites.f_Insert(Connection.f_WriteAsyncHeader(ResponseHeader)); Future.f_IsValid())
					co_await fg_Move(Future);

				co_return ResponseHeader.m_Status == 304;
			}
		;

		{
			DMibLock(mp_Lock);

			if (!mp_pFSInterface->f_FileExists(Filename))
				co_return false;

			if (Request.m_Method == "HEAD")
			{
				CMibFilePos nBytes = mp_pFSInterface->f_GetFileSize(Filename);
				co_await fg_CallSafe(fWriteHeader, nBytes);

				while (QueuedWrites)
					co_await QueuedWrites.f_PopFirst();

				co_return true;
			}

			TCSharedPointer<NStream::CBinaryStreamDefaultRef> pStream = mp_pFSInterface->f_OpenStream(Filename, NFile::EFileOpen_Read);

			if (!pStream)
				co_return false;

			CMibFilePos nBytes = pStream->f_GetLength();

			if (co_await fg_CallSafe(fWriteHeader, nBytes))
			{
				while (QueuedWrites)
					co_await QueuedWrites.f_PopFirst();

				co_return true;
			}

			CMibFilePos nBytesRead;
			while (nBytes)
			{
				NContainer::CIOByteVector Buffer;
				Buffer.f_SetLen(NFile::gc_IdealIoSize);
				nBytesRead = fg_Min(nBytes, CMibFilePos(NFile::gc_IdealIoSize));
				pStream->f_ConsumeBytes(Buffer.f_GetArray(), nBytesRead);

				if (auto Future = QueuedWrites.f_Insert(Connection.f_WriteAsyncBinary(fg_Move(Buffer))); Future.f_IsValid())
					co_await fg_Move(Future);

				nBytes -= nBytesRead;
			}

			while (QueuedWrites)
				co_await QueuedWrites.f_PopFirst();

		}

		co_return true;
	}
}
