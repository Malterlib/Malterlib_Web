// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Web_HTTPRequestHandler_ExeFS.h"
#include <Mib/File/MalterlibFS>
#include <Mib/File/VirtualFSs/MalterlibFS>
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NWeb
{
	/***************************************************************************************************\
	|¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯|
	| CBuildRequestHandler_ExeFs																		|
	|___________________________________________________________________________________________________|
	\***************************************************************************************************/

	CHTTPRequestHandler_ExeFs::CHTTPRequestHandler_ExeFs
		(
			NStr::CStr const& _ServerPath
			, NStr::CStr const& _ExeFsPath
			, NFunction::TCFunction<bool (CHTTPConnection &_Connection, CHTTPRequest const& _Req)> const &_fCheckAccess
		)
		: mp_Path(_ServerPath)
		, mp_ExeFsPath(_ExeFsPath)
		, mp_fCheckAccess(_fCheckAccess)
	{
		if (NMib::NFile::fg_OpenExeFS(mp_ExeFs))
		{
			mp_pFSInterface = fg_Construct<NFile::CFileSystemInterface_VirtualFS>(mp_ExeFs.m_FileSystem);
		}

		try
		{
			NStr::CStr ExeFilename = NSys::NFile::fg_GetProgramPath();
			NStorage::TCUniquePointer<NFile::CFile> pExeFile = fg_Construct();
			pExeFile->f_Open(ExeFilename, NFile::EFileOpen_ReadAttribs | NFile::EFileOpen_ShareAll);
			m_ExeTime = pExeFile->f_GetCreationTime().f_ToUTC();
		}
		catch (NFile::CExceptionFile)
		{
			m_ExeTime = NTime::CTime::fs_NowUTC();
		}
	}

	CHTTPRequestHandler_ExeFs::~CHTTPRequestHandler_ExeFs()
	{

	}

	// This could be called for any thread - assume nothing.
	bool CHTTPRequestHandler_ExeFs::f_HandleRequest(CHTTPConnection &_Connection, CHTTPRequest const &_Req)
	{
		if (mp_fCheckAccess)
		{
			if (!mp_fCheckAccess(_Connection, _Req))
				return true;
		}

		try
		{
			if (!mp_pFSInterface)
				return false;

			NStr::CStr Filename = _Req.m_RequestedURI;

			int iPos = Filename.f_Find(mp_Path);
			if (iPos != 0)
				return false;

			Filename = mp_ExeFsPath + Filename.f_Extract(mp_Path.f_GetLen());

			if (Filename.f_Right(1) == "/")
				return false;

			NCryptography::CHash_SHA256 Hash;
			Hash.f_AddData(&m_ExeTime, sizeof(m_ExeTime));
			Hash.f_AddData(Filename.f_GetStr(), Filename.f_GetLen());

			NStr::CStr HashTag = Hash.f_GetDigest().f_GetString();

			auto fWriteHeader = [&](CMibFilePos _nBytes) -> bool
				{
					CHTTPResponseHeader ResponseHeader;
					ResponseHeader.f_SetMimeTypeFromFilename(Filename);
					ResponseHeader.m_LastModified = m_ExeTime;
					ResponseHeader.m_ContentLength = _nBytes;
					ResponseHeader.m_AllowMethods = "GET, HEAD";
					ResponseHeader.m_CacheControl = "no-cache";
					ResponseHeader.m_ETag = HashTag;

					if (auto *pIfNoneMatch = _Req.m_Headers.f_FindEqual("if_none_match"); pIfNoneMatch)
					{
						if (*pIfNoneMatch == HashTag)
							ResponseHeader.m_Status = 304;
					}

					_Connection.f_WriteHeader(ResponseHeader);

					return ResponseHeader.m_Status == 304;
				}
			;

			{
				DMibLock(mp_Lock);

				if (!mp_pFSInterface->f_FileExists(Filename))
					return false;

				if (_Req.m_Method == "HEAD")
				{
					CMibFilePos nBytes = mp_pFSInterface->f_GetFileSize(Filename);
					fWriteHeader(nBytes);
					return true;
				}

				NStorage::TCSharedPointer<NStream::CBinaryStreamDefaultRef> pStream = mp_pFSInterface->f_OpenStream(Filename, NFile::EFileOpen_Read);

				if (!pStream)
					return false;

				CMibFilePos nBytes = pStream->f_GetLength();

				if (fWriteHeader(nBytes))
					return true;

				NContainer::CByteVector lBuf;
				lBuf.f_SetLen(1024 * 1024);

				CMibFilePos nBytesRead;
				while (nBytes)
				{
					nBytesRead = fg_Min(nBytes, 1024*1024);
					pStream->f_ConsumeBytes(lBuf.f_GetArray(), nBytesRead);
					_Connection.f_WriteBinary(lBuf.f_GetArray(), nBytesRead);
					nBytes -= nBytesRead;
				}
			}

			return true;
		}
		catch (NFile::CExceptionFile)
		{
			return false;
		}
	}
}
