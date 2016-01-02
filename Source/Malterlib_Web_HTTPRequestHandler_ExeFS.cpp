// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTPRequestHandler_ExeFS.h"
#include <Mib/File/MalterlibFS>
#include <Mib/File/VirtualFSs/MalterlibFS>


namespace NMib
{
	namespace NWeb
	{
		/***************************************************************************************************\
		|¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯|
		| CBuildRequestHandler_ExeFs																		|
		|___________________________________________________________________________________________________|
		\***************************************************************************************************/

		CHTTPRequestHandler_ExeFs::CHTTPRequestHandler_ExeFs(NStr::CStr const& _ServerPath, NStr::CStr const& _ExeFsPath)
			: mp_Path(_ServerPath)
			, mp_ExeFsPath(_ExeFsPath)
		{
			if (NMib::NFile::fg_OpenExeFS(mp_ExeFs))
			{
				mp_pFSInterface = fg_Construct<NFile::CFileSystemInterface_VirtualFS>(mp_ExeFs.m_FileSystem);
			}

			try
			{
				NStr::CStr ExeFilename = NSys::NFile::fg_GetProgramPath();
				NPtr::TCUniquePointer<NFile::CFile> pExeFile = fg_Construct();
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
		bint CHTTPRequestHandler_ExeFs::f_HandleRequest(CHTTPConnection &_Connection, CHTTPRequest const& _Req)
		{
			try
			{
				if (!mp_pFSInterface)
					return false;

				NStr::CStr Filename = _Req.m_RequestedURI;

				int iPos = Filename.f_Find(mp_Path);
				if (iPos != 0)
					return false;

				Filename = mp_ExeFsPath + Filename.f_Extract(mp_Path.f_GetLen());
				NTime::CTime ExeTime = m_ExeTime;

				if (Filename.f_Right(1) == "/")
					return false;

				auto f_WriteHeader = [&Filename, &_Connection, ExeTime](CMibFilePos _nBytes)
				{

					CHTTPResponseHeader ResponseHeader;		
					ResponseHeader.f_SetMimeTypeFromFilename(Filename);
					ResponseHeader.m_LastModified = ExeTime;
					ResponseHeader.m_Expires = NTime::CTime::fs_NowUTC() + NTime::CTimeSpanConvert::fs_CreateDaySpan(1);
					ResponseHeader.m_ContentLength = _nBytes;
					ResponseHeader.m_AllowMethods = "GET, HEAD";
					_Connection.f_Write(ResponseHeader);
				};


				{
					DMibLock(mp_Lock);

					if (_Req.m_Method == "HEAD")
					{
						CMibFilePos nBytes = mp_pFSInterface->f_GetFileSize(Filename);
						f_WriteHeader(nBytes);
						return true;
					}

					if (!mp_pFSInterface->f_FileExists(Filename))
						return false;
					
					NPtr::TCSharedPointer<NStream::CBinaryStreamDefaultRef> pStream = mp_pFSInterface->f_OpenStream(Filename, NFile::EFileOpen_Read);

					if (!pStream)
						return false;

					CMibFilePos nBytes = pStream->f_GetLength();

					f_WriteHeader(nBytes);

					NContainer::TCVector<uint8> lBuf;
					lBuf.f_SetLen(1024 * 1024);

					CMibFilePos nBytesRead;
					while (nBytes)
					{
						nBytesRead = fg_Min(nBytes, 1024*1024);
						pStream->f_ConsumeBytes(lBuf.f_GetArray(), nBytesRead);
						_Connection.f_Write(lBuf.f_GetArray(), nBytesRead);
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
}