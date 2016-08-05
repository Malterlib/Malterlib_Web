// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Connection.h"
#include "Malterlib_Web_HTTP_Request.h"
#include "Malterlib_Web_HTTP_PagedByteVector.h"

namespace NMib
{

	namespace NHTTP
	{

		class CConnection::CDetails
		{
		private:
			enum EState
			{
				EState_Connected,
			};

			static constexpr mint mc_ReadBufferSize = 2048;

			EState mp_State;

			NPtr::TCUniquePointer<NMib::NNet::CSocket> mp_pSocket;

			CPagedByteVector mp_IncomingBuffer;

			bint mp_bFirstProcess;

			CRequest mp_PendingRequest; // If a request is awaiting content it is stored here.

			class CLinkedRequest : public CRequest
			{
			public:
				inline CLinkedRequest();
				inline CLinkedRequest(CLinkedRequest&& _ToMove);
				inline CLinkedRequest(CRequest&& _ToMove);
				inline ~CLinkedRequest();

				DMibListLinkD_Link(CRequest, mp_Link);
			};

			DMibListLinkD_List(CLinkedRequest, mp_Link) mp_OpenRequests;

		public:
			CDetails(NPtr::TCUniquePointer<NNet::CSocket> _pSock);
			~CDetails();

			bint f_IsConnected() const;

			void f_SetReportTo(NMib::NThread::CSemaphoreReportableAggregate *_pReportTo);

			void f_Process();

		};

		//
		// CConnection::CDetails::CLinkedRequest Public Methods
		//

		CConnection::CDetails::CLinkedRequest::CLinkedRequest()
		{

		}

		CConnection::CDetails::CLinkedRequest::CLinkedRequest(CLinkedRequest&& _ToMove)
			: CRequest(fg_Move(_ToMove))
		{
		}

		CConnection::CDetails::CLinkedRequest::CLinkedRequest(CRequest&& _ToMove)
			: CRequest(fg_Move(_ToMove))
		{
		}

		CConnection::CDetails::CLinkedRequest::~CLinkedRequest()
		{
		}


		//
		// CConnection::CDetails Public Methods
		//

		CConnection::CDetails::CDetails(NPtr::TCUniquePointer<NNet::CSocket> _pSock)
			: mp_State(EState_Connected)
			, mp_pSocket(fg_Move(_pSock))
			, mp_bFirstProcess(true)
			, mp_IncomingBuffer(mc_ReadBufferSize)
		{
		}

		CConnection::CDetails::~CDetails()
		{
		}


		bint CConnection::CDetails::f_IsConnected() const
		{
			if (!mp_pSocket->f_IsValid() || mp_pSocket->f_GetState() & NNet::ENetTCPState_Closed)
				return false;
			else
				return true;
		}

		void CConnection::CDetails::f_SetReportTo(NMib::NThread::CSemaphoreReportableAggregate *_pReportTo)
		{
			mp_pSocket->f_SetReportTo(_pReportTo);
		}

		void CConnection::CDetails::f_Process()
		{
//			DTraceRaw("CConnection::f_Process()\n");
			
			auto SocketState = mp_pSocket->f_GetState();

			if ( (SocketState & NNet::ENetTCPState_Read) || mp_bFirstProcess)
			{
				DMibTraceRaw("CConnection: Read Set\n");

				mint nTotalBytesRead = 0;

				// Read as much as we can from the socket into the incoming buffer.
				{
					uint8 ReadBuffer[mc_ReadBufferSize];

					mint nBytesRead = 0;

					mint nBytesToRead = mp_IncomingBuffer.f_GetFirstPageSpace() ? mp_IncomingBuffer.f_GetFirstPageSpace() : mc_ReadBufferSize;
					DMibTrace("nInitialBytesToRead: {}\n", nBytesToRead );

					while ( (nBytesRead = mp_pSocket->f_Receive(ReadBuffer, nBytesToRead) ) == nBytesToRead)
					{
						DMibTrace("Received:\n{}\n---\n", NStr::CStr((char const*)ReadBuffer, nBytesRead) );

						mp_IncomingBuffer.f_InsertBack(ReadBuffer, nBytesRead);
						nBytesToRead = mc_ReadBufferSize;
						nTotalBytesRead += nBytesRead;
					}

					if (nBytesRead)
					{
						DMibTrace("Received:\n{}\n---\n", NStr::CStr((char const*)ReadBuffer, nBytesRead) );
						mp_IncomingBuffer.f_InsertBack(ReadBuffer, nBytesRead);
						nTotalBytesRead += nBytesRead;
					}

				}

				DMibTrace("CConnection: nTotalRead: {}\n", nTotalBytesRead);


				if (nTotalBytesRead > 0)
				{
					// Send the incoming data to the pending request to parse.
					switch(mp_PendingRequest.f_Parse(mp_IncomingBuffer))
					{
						case ERequestStatus_InProgress:
							{
								// All is OK, we are parsing the request.
								break;
							}
						case ERequestStatus_Complete:
							{
								DMibTraceRaw("CConnection::f_Process() Request Complete\n");
								mp_PendingRequest.f_GetRequestLine().f_GetURI().f_DebugOut();

								// The request is parsed, dispatch it.
								NPtr::TCUniquePointer<CLinkedRequest> pNewReq = fg_Construct(fg_Move(mp_PendingRequest));
								mp_OpenRequests.f_InsertLast(pNewReq.f_Detach());
								break;
							}
						case ERequestStatus_Invalid:
						default: // Nothing else is valid here.
							{
								// Connection has failed, drop the connection.
								DMibTraceRaw("CConnection::f_Process() Request Invalid\n");
								mp_pSocket->f_Close(); // ??
								break;
							}

					}
				}

			}

			if (SocketState & NNet::ENetTCPState_Write || mp_bFirstProcess)
			{
				
			}

			mp_bFirstProcess = false;
		}

		//
		// CConnection Public Methods
		//

		CConnection::CConnection(NPtr::TCUniquePointer<NNet::CSocket> _pSock)
			: mp_pD(fg_Construct(fg_Move(_pSock)))
		{

		}

		CConnection::~CConnection()
		{

		}

		bint CConnection::f_IsConnected() const
		{
			return mp_pD->f_IsConnected();
		}

		void CConnection::f_SetReportTo(NMib::NThread::CSemaphoreReportableAggregate *_pReportTo)
		{
			mp_pD->f_SetReportTo(_pReportTo);
		}

		void CConnection::f_Process()
		{
			mp_pD->f_Process();
		}

	} // Namespace NHTTP

} // Namespace NMib