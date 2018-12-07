// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_FastCGIServer_Internal.h"
#include "Malterlib_Web_FastCGIServer_Connection.h"
#include "Malterlib_Web_FastCGIServer_Protocol.h"

namespace NMib::NWeb
{
	using namespace NFastCGI;

	CFastCGIConnectionActor::CRecordInfo::CRecordInfo()
		: m_Type(ERequestType_Invalid)
		, m_ID(0)
		, m_Role(ERequestRole_Invalid)
	{
	}
	CFastCGIConnectionActor::CFastCGIConnectionActor(NConcurrency::TCActor<CFastCGIServer::CInternal> const& _pServer, CFastCGIServer::CInternal& _ServerInternal)
		: mp_NeededData(0)
		, mp_IncomingPosition(0)
		, mp_pServer(_pServer)
		, mp_OutgoingPosition(0)
		, mp_ServerInternal(_ServerInternal)
		, mp_ProcessingThread(0)
	{
	}

	CFastCGIConnectionActor::~CFastCGIConnectionActor()
	{
	}

	void CFastCGIConnectionActor::f_SetSocket(NStorage::TCSharedPointer<NNetwork::CSocket> const& _pSocket)
	{
		mp_Socket = fg_Move(*_pSocket);
		fp_ProcessState();
		mp_ProcessingThread = 0;
	}

	NConcurrency::TCContinuation<void> CFastCGIConnectionActor::fp_Destroy()
	{
		mp_Socket.f_Close();

		fp_ClearState();

		return NConcurrency::TCContinuation<void>::fs_Finished();
	}

	void CFastCGIConnectionActor::f_StateAdded(NNetwork::ENetTCPState _StateAdded)
	{
		if (mp_Socket.f_IsValid())
			fp_ProcessState();
	}

	void CFastCGIConnectionActor::fp_ClearState()
	{
		mp_RecordInfo = CRecordInfo();
		mp_Params.f_Clear();
		mp_fOnStdInputRaw.f_Clear();
		mp_fOnData.f_Clear();
		mp_fOnStdInputStr.f_Clear();
		mp_fOnAbort.f_Clear();

		mp_OutgoingData.f_Clear();
		mp_OutgoingPosition = 0;
	}

	void CFastCGIConnectionActor::fp_Disconnect(ch8 const* _pReason)
	{
		if (_pReason)
			fp_OnAbort();

		fp_ClearState();

		mp_Socket.f_Close();

		mp_pServer(&CFastCGIServer::CInternal::f_RemoveConnection, fg_ThisActor(this))
			> NConcurrency::fg_DiscardResult()
		;

	}

	bool CFastCGIConnectionActor::fp_ProcessManagementRecord(CHeader const& _Header, uint8 const* _pData, mint _DataLen)
	{
		DMibTrace("Management record\n", 0);

		return true;
	}

	void CFastCGIConnectionActor::fp_SendData(uint8 const* _pData, mint _Len)
	{
		mp_OutgoingData.f_Insert(_pData, _Len);
		fp_UpdateSend();
	}

	void CFastCGIConnectionActor::fp_SendData(NContainer::CByteVector const& _Data)
	{
		fp_SendData(_Data.f_GetArray(), _Data.f_GetLen());
	}

	void CFastCGIConnectionActor::fp_SendStdOutput(NContainer::CByteVector const& _Data, ERequestType _Type)
	{
		auto *pData = _Data.f_GetArray();
		mint ToSend = _Data.f_GetLen();
		uint8 Padding[8] = {};
		uint8 HeaderData[8] = {};

		while (ToSend)
		{
			mint ThisTime = fg_Min(ToSend, 64*1024 - (sizeof(CHeader)));
			NStream::CBinaryStreamMemoryPtr<NStream::CBinaryStreamBigEndian> Stream;
			Stream.f_OpenReadWrite(HeaderData, 8, 0);

			mint PaddingSize = 0;
			if (ThisTime & 7)
				PaddingSize = 8 - (ThisTime & 7);

			CHeader Header;
			Header.m_Type = _Type;
			Header.m_RequestID = mp_RecordInfo.m_ID;
			Header.m_ContentLength = ThisTime;
			Header.m_PaddingLength = PaddingSize;
			Stream << Header;

			fp_SendData((uint8 const *)Stream.f_GetBuffer(), Stream.f_GetLength());
			fp_SendData(pData, ThisTime);
			fp_SendData(Padding, PaddingSize);

			pData += ThisTime;
			ToSend -= ThisTime;
		}
	}

	void CFastCGIConnectionActor::f_SendStdOutput(NContainer::CByteVector const& _Data)
	{
		mp_RecordInfo.m_bSentStdOut = true;
		fp_SendStdOutput(_Data, ERequestType_StdOut);
	}

	void CFastCGIConnectionActor::f_SendStdError(NContainer::CByteVector const& _Data)
	{
		mp_RecordInfo.m_bSentStdErr = true;
		fp_SendStdOutput(_Data, ERequestType_StdErr);
	}

	void CFastCGIConnectionActor::f_FinishRequest()
	{
		NStream::CBinaryStreamMemory<NStream::CBinaryStreamBigEndian> Stream;

		// Send end of stream for std out and std err

		if (mp_RecordInfo.m_bSentStdOut)
		{
			CHeader Header;
			Header.m_Type = ERequestType_StdOut;
			Header.m_RequestID = mp_RecordInfo.m_ID;
			Header.m_ContentLength = 0;
			Stream << Header;
		}

		if (mp_RecordInfo.m_bSentStdErr)
		{
			CHeader Header;
			Header.m_Type = ERequestType_StdErr;
			Header.m_RequestID = mp_RecordInfo.m_ID;
			Header.m_ContentLength = 0;
			Stream << Header;
		}

		// End request
		{
			CHeader Header;
			Header.m_Type = ERequestType_EndRequest;
			Header.m_RequestID = mp_RecordInfo.m_ID;
			Header.m_ContentLength = sizeof(CEndRequestBody);
			Stream << Header;

			CEndRequestBody Request;
			Request.m_ProtocolStatus = EEndRequestStatus_RequestComplete;
			Request.m_AppStatus = 0;
			Stream << Request;
		}

		mp_RecordInfo.m_bFinished = true;

		fp_SendData(Stream.f_MoveVector());

	}


	void CFastCGIConnectionActor::fp_OnStdIn(uint8 const* _pData, mint _Len)
	{
		if (mp_fOnStdInputRaw)
			mp_fOnStdInputRaw(_pData, _Len, _Len == 0);
		else if (mp_fOnStdInputStr)
		{
			NStr::CStr String;
			String.f_AddStr((ch8 const*)_pData, _Len);
			mp_fOnStdInputStr(String, _Len == 0);
		}
	}

	void CFastCGIConnectionActor::fp_OnStdData(uint8 const* _pData, mint _Len)
	{
		if (mp_fOnData)
			mp_fOnData(_pData, _Len, _Len == 0);
	}

	void CFastCGIConnectionActor::fp_OnParams(NContainer::TCMap<NStr::CStr, NStr::CStr> const& _Params)
	{
		NStorage::TCSharedPointer<CFastCGIRequest> pRequest = fg_Construct(fg_ThisActor(this), *this);
		mp_ServerInternal.mp_fOnRequest(pRequest);
	}

	void CFastCGIConnectionActor::fp_OnAbort()
	{
		if (mp_fOnAbort)
			mp_fOnAbort();
	}


	bool CFastCGIConnectionActor::fp_ProcessBeginRequest(CHeader const& _Header, uint8 const* _pData, mint _DataLen)
	{
		if (_DataLen != sizeof(CBeginRequestBody))
		{
			fp_Disconnect("Invalid content size for begin request");
			return false;
		}
		if (mp_RecordInfo.m_bBegun)
		{
			mp_RecordInfo.m_bInvalid = true; // Don't receive any more data, and close connection after all outstanding data has been sent

			NStream::CBinaryStreamMemory<NStream::CBinaryStreamBigEndian> Stream;

			CHeader Header;
			Header.m_Type = ERequestType_EndRequest;
			Header.m_RequestID = mp_RecordInfo.m_ID;
			Header.m_ContentLength = sizeof(CEndRequestBody);
			Stream << Header;

			CEndRequestBody Request;
			Request.m_ProtocolStatus = EEndRequestStatus_CantMultiplexConnection;
			Stream << Request;

			fp_SendData(Stream.f_MoveVector());
			return false;
		}

		mp_RecordInfo.m_ID = _Header.m_RequestID;

		NStream::CBinaryStreamMemoryPtr<NStream::CBinaryStreamBigEndian> Stream;
		Stream.f_OpenRead(_pData, _DataLen);

		CBeginRequestBody Request;

		Stream >> Request;

		mp_RecordInfo.m_bKeepConnection = (Request.m_Flags & ERequestBodyFlag_KeepConnection) != 0;
		mp_RecordInfo.m_Role = Request.m_Role;
		mp_RecordInfo.m_bBegun = true;

		return true;
	}

	bool CFastCGIConnectionActor::fp_ProcessStreamData(CHeader const& _Header, uint8 const* _pData, mint _DataLen)
	{
		// DMibTrace("Stream data\n", _Header.m_);

#if 0
							   WS->App   management  stream

		FCGI_GET_VALUES           x          x
		FCGI_GET_VALUES_RESULT               x
		FCGI_UNKNOWN_TYPE                    x

		FCGI_BEGIN_REQUEST        x
		FCGI_ABORT_REQUEST        x
		FCGI_END_REQUEST
		FCGI_PARAMS               x                    x
		FCGI_STDIN                x                    x
		FCGI_DATA                 x                    x
		FCGI_STDOUT                                    x
		FCGI_STDERR                                    x
#endif

		switch (_Header.m_Type)
		{
		case ERequestType_Params:
			{
				if (_DataLen == 0)
					fp_OnParams(mp_Params);
				else
				{
					try
					{
						NStream::CBinaryStreamMemoryPtr<NStream::CBinaryStreamBigEndian> Stream;
						Stream.f_OpenRead(_pData, _DataLen);

						while (!Stream.f_IsAtEndOfStream())
						{

							auto Pos = Stream.f_GetPosition();
							uint8 KeyLen8;
							uint32 KeyLen;
							Stream >> KeyLen8;
							if (KeyLen8 & DMibBit(7))
							{
								Stream.f_SetPosition(Pos);
								Stream >> KeyLen;
								KeyLen &= ~DMibBitTyped(31, uint32);
							}
							else
								KeyLen = KeyLen8;

							Pos = Stream.f_GetPosition();
							uint8 ValueLen8;
							uint32 ValueLen;
							Stream >> ValueLen8;
							if (ValueLen8 & DMibBit(7))
							{
								Stream.f_SetPosition(Pos);
								Stream >> ValueLen;
								ValueLen &= ~DMibBitTyped(31, uint32);
							}
							else
								ValueLen = ValueLen8;

							NStr::CStr Key;
							NStr::CStr Value;

							auto StreamLen = Stream.f_GetLength();
							if ((StreamLen - Stream.f_GetPosition()) < KeyLen)
							{
								fp_Disconnect("Invalid params recond");
								return false;
							}
							Stream.f_ConsumeBytes(Key.f_GetStr(KeyLen + 1), KeyLen);
							Key.f_SetAt(KeyLen, 0);
							Key.f_SetStrLen(KeyLen);

							if ((StreamLen - Stream.f_GetPosition()) < ValueLen)
							{
								fp_Disconnect("Invalid params recond");
								return false;
							}
							Stream.f_ConsumeBytes(Value.f_GetStr(ValueLen + 1), ValueLen);
							Value.f_SetAt(ValueLen, 0);
							Value.f_SetStrLen(ValueLen);

							mp_Params(Key, Value);
						}
					}
					catch (NException::CException const& _Exception)
					{
						// Protect against bad data in stream
						fp_Disconnect(_Exception.f_GetErrorStr());
						return false;
					}
				}
			}
			break;
		case ERequestType_StdIn:
			{
				fp_OnStdIn(_pData, _DataLen);
			}
			break;
		case ERequestType_Data:
			{
				fp_OnStdIn(_pData, _DataLen);
			}
			break;
		// Should have been taken care of separately
		case ERequestType_BeginRequest:
		case ERequestType_AbortRequest:
		// Only valid for management records
		case ERequestType_GetValues:
		// Not valid for server
		case ERequestType_EndRequest:
		case ERequestType_StdOut:
		case ERequestType_StdErr:
		case ERequestType_GetValuesResult:
		case ERequestType_UnknownType:
		default:
			{
				fp_Disconnect("Unexpected request type");
				return false;
			}
			break;
		}

		return true;
	}

	void CFastCGIConnectionActor::fp_UpdateSend()
	{
		if (!mp_Socket.f_IsValid())
			return;
		mint ToSend = mp_OutgoingData.f_GetLen() - mp_OutgoingPosition;
		if (ToSend)
		{
			try
			{
				mint Sent = mp_Socket.f_Send(mp_OutgoingData.f_GetArray() + mp_OutgoingPosition, ToSend);
				mp_OutgoingPosition += Sent;
			}
			catch (NNetwork::CExceptionNet const& _Exception)
			{
				fp_Disconnect(_Exception.f_GetErrorStr());
				return;
			}

			ToSend = mp_OutgoingData.f_GetLen() - mp_OutgoingPosition;
			if (ToSend < mp_OutgoingData.f_GetLen() / 2)
			{
				// Discard old data
				NContainer::CByteVector OldData = fg_Move(mp_OutgoingData);
				mp_OutgoingData.f_Insert(OldData.f_GetArray() + mp_OutgoingPosition, ToSend);
				mp_OutgoingPosition = 0;
			}

			if (ToSend == 0)
			{
				if (mp_RecordInfo.m_bFinished)
				{
					if (!mp_RecordInfo.m_bKeepConnection)
						fp_Disconnect(nullptr);
					else
						fp_ClearState();
				}
				else if (mp_RecordInfo.m_bInvalid)
					fp_Disconnect("Invalid connection closed");
			}
		}
	}

	void CFastCGIConnectionActor::fp_ProcessState()
	{
		if (!mp_Socket.f_IsValid() )
			return;

		mp_ProcessingThread = NSys::fg_Thread_GetCurrentUID();
		auto Cleanup
			= fg_OnScopeExit
			(
				[&]
				{
					mp_ProcessingThread = 0;
				}
			)
		;

		auto StateAdded = mp_Socket.f_GetState();
		if (StateAdded & NNetwork::ENetTCPState_Closed)
		{
			fp_Disconnect("Connection closed");
			return;
		}
		if (StateAdded & NNetwork::ENetTCPState_Read && !mp_RecordInfo.m_bInvalid)
		{
			uint8 Data[4096];
			try
			{
				while (true)
				{
					mint Size = 4096;
					mint Received = mp_Socket.f_Receive(Data, Size);
					if (Received == 0)
						break;
					mp_IncomingData.f_Insert(Data, Received);
				}
			}
			catch (NNetwork::CExceptionNet const& _Exception)
			{
				fp_Disconnect(_Exception.f_GetErrorStr());
				return;
			}
			while (true)
			{
				if (mp_NeededData == 0)
				{
					mint Available = mp_IncomingData.f_GetLen() - mp_IncomingPosition;
					if (Available >= 8)
					{
						// We have enough for header
						NStream::CBinaryStreamMemoryPtr<NStream::CBinaryStreamBigEndian> Stream;
						Stream.f_OpenRead(mp_IncomingData.f_GetArray() + mp_IncomingPosition, 8);
						Stream >> mp_CurrentHeader;
						mp_NeededData = mp_CurrentHeader.m_ContentLength + mp_CurrentHeader.m_PaddingLength;
						mp_IncomingPosition += 8;
						if (mp_CurrentHeader.m_Version != ERequestVersion_1)
						{
							// Unsupported version / corrupt data
							fp_Disconnect("Unsupported fast CGI version");
							return;
						}
						//DMibTrace("Received header ({}) with content: {}  RequestID: {} \n", mp_CurrentHeader.m_Type << mp_CurrentHeader.m_ContentLength << mp_CurrentHeader.m_RequestID);
					}
					else
						break;
				}
				mint Available = mp_IncomingData.f_GetLen() - mp_IncomingPosition;
				if (Available >= mp_NeededData)
				{
					if (mp_CurrentHeader.m_RequestID == 0)
					{
						// This is a management records
						if (!fp_ProcessManagementRecord(mp_CurrentHeader, mp_IncomingData.f_GetArray() + mp_IncomingPosition, mp_CurrentHeader.m_ContentLength))
							break;
					}
					else if (mp_CurrentHeader.m_Type == ERequestType_BeginRequest)
					{
						if (!fp_ProcessBeginRequest(mp_CurrentHeader, mp_IncomingData.f_GetArray() + mp_IncomingPosition, mp_CurrentHeader.m_ContentLength))
							break;
					}
					else if (mp_CurrentHeader.m_RequestID == mp_RecordInfo.m_ID)
					{
						if (!fp_ProcessStreamData(mp_CurrentHeader, mp_IncomingData.f_GetArray() + mp_IncomingPosition, mp_CurrentHeader.m_ContentLength))
							break;
					}
					else if (mp_CurrentHeader.m_RequestID != mp_RecordInfo.m_ID)
					{
						// Just skip this packet
						DMibTrace("Skip out of order request\n", 0);
					}
					else if (mp_CurrentHeader.m_Type != mp_RecordInfo.m_Type)
					{
						fp_Disconnect("Protocol error");
						return;
					}

					mp_IncomingPosition += mp_NeededData;
					mp_NeededData = 0;
					Available = mp_IncomingData.f_GetLen() - mp_IncomingPosition;
				}

				if (Available < mp_IncomingData.f_GetLen() / 2)
				{
					// Discard old packet data
					NContainer::CByteVector OldData = fg_Move(mp_IncomingData);
					mp_IncomingData.f_Insert(OldData.f_GetArray() + mp_IncomingPosition, Available);
					mp_IncomingPosition = 0;
				}
			}
		}

		if (StateAdded & NNetwork::ENetTCPState_Write)
			fp_UpdateSend();
	}
}
