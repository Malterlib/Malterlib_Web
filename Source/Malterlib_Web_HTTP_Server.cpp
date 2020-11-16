// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_Server.h"
#include "Malterlib_Web_HTTP_Connection.h"

namespace NMib::NWeb::NHTTP
{
	class CConnectionWorker
	{
	private:
		DMibListLinkD_List(CConnection, mp_Link) mp_ConnectionsList;

		NThread::CMutual mp_NewConnectionsLock;
		DMibListLinkD_List(CConnection, mp_Link) mp_NewConnectionsList;

		NStorage::TCUniquePointer<NThread::CThreadObject> mp_pThread;

		aint fp_Work(NThread::CThreadObject* _pThread);

	public:
		CConnectionWorker();
		~CConnectionWorker();

		// Thread safe
		void f_AddConnection( NStorage::TCUniquePointer<CConnection> _pConn );

		void f_Start();
		void f_Stop();

	};

	class CServer::CDetails
	{
	private:
		enum EState
		{
			EState_Stopped,
			EState_Running,
		};

		CConfig mp_Config;
		EState mp_State;

		NStorage::TCUniquePointer<NThread::CThreadObject> mp_pListenThread;

		// TEMP: Will be a pool of workers
		CConnectionWorker mp_Worker;
//		NContainer::TCVector< TCUniquePointer<NThread::CThreadObject> > mp_lProcessingThreads;

		// Run on mp_ListenThread
		aint fp_Listen(NThread::CThreadObject* _pThread);

	private:

	public:
		CDetails(CConfig const& _Config);
		~CDetails();

		bool f_Start(NStr::CStr& _oError);
		void f_Stop();
	};

	//
	// CServer::CDetails Public Methods
	//

	CServer::CDetails::CDetails(CConfig const& _Config)
		: mp_State(EState_Stopped)
		, mp_Config(_Config)
	{
	}

	CServer::CDetails::~CDetails()
	{
	}

	bool CServer::CDetails::f_Start(NStr::CStr& _oError)
	{
		if (mp_State != EState_Stopped)
		{
			_oError = "The server is already running.";
			return false;
		}

		mp_pListenThread = NThread::CThreadObject::fs_StartThread(
								[this](NThread::CThreadObject* _pThread) { return fp_Listen(_pThread); }
							,	"HTTPServerListener"
							);

		mp_Worker.f_Start();

		mp_State = EState_Running;
		return true;
	}

	void CServer::CDetails::f_Stop()
	{
		if (mp_State != EState_Running)
			return;

		if (mp_pListenThread)
		{
			mp_pListenThread->f_Stop();
			mp_pListenThread = nullptr;
		}

		mp_Worker.f_Stop();

	}

	//
	// CServer::CDetails Private Methods
	//

	aint CServer::CDetails::fp_Listen(NThread::CThreadObject* _pThread)
	{
		NNetwork::CSocket ListenSocket;

		try
		{
			NNetwork::CNetAddressTCPv4 TCPListenAddr;
			TCPListenAddr.m_Port = mp_Config.m_Port;

			NNetwork::CNetAddress ListenAddr;
			ListenAddr.f_Set(TCPListenAddr);

			ListenSocket.f_Listen(ListenAddr, &_pThread->m_EventWantQuit, NMib::NNetwork::ENetFlag_None);
		}
		catch (NException::CException const &/*_Exception*/)
		{
			return -1;
		}

		NStorage::TCUniquePointer<NNetwork::CSocket> pIncomingSocket;

		while(_pThread->f_GetState() != NThread::EThreadState_EventWantQuit)
		{
				while ( 1 )
				{
					if (!pIncomingSocket)
						pIncomingSocket = fg_Construct();

					pIncomingSocket->f_Accept(&ListenSocket, nullptr);

					if (pIncomingSocket->f_IsValid())
					{
						NStorage::TCUniquePointer<CConnection> pNewConn = fg_Construct(fg_Move(pIncomingSocket));

						// Put on connection list
						mp_Worker.f_AddConnection(fg_Move(pNewConn));
					}
					else
					{
						break;
					}
				}

			_pThread->m_EventWantQuit.f_Wait();
		}

		pIncomingSocket = nullptr;

		return 0;
	}

	//
	// CServer Public Methods
	//

	CServer::CServer(CConfig const& _Config)
		: mp_pD(fg_Construct(_Config))
	{

	}

	CServer::~CServer()
	{

	}

	bool CServer::f_Start(NStr::CStr & _oError)
	{
		return mp_pD->f_Start(_oError);
	}

	void CServer::f_Stop()
	{
		mp_pD->f_Stop();
	}

	//
	// CConnectionWorker Public Methods
	//

	CConnectionWorker::CConnectionWorker()
	{

	}

	CConnectionWorker::~CConnectionWorker()
	{
		mp_NewConnectionsList.f_DeleteAllDefiniteType();
		mp_ConnectionsList.f_DeleteAllDefiniteType();
	}

	// Thread safe
	void CConnectionWorker::f_AddConnection( NStorage::TCUniquePointer<CConnection> _pConn )
	{
		DMibLock(mp_NewConnectionsLock);
		mp_NewConnectionsList.f_Insert(_pConn.f_Detach());

		if (mp_pThread)
			mp_pThread->m_EventWantQuit.f_Signal();
	}

	void CConnectionWorker::f_Start()
	{
		if (!mp_pThread)
			mp_pThread = NThread::CThreadObject::fs_StartThread( [this](NThread::CThreadObject* _pThread) { return fp_Work(_pThread); }, "HTTPConnectionWorker" );
	}

	void CConnectionWorker::f_Stop()
	{
		mp_pThread->f_Stop();
	}

	aint CConnectionWorker::fp_Work(NThread::CThreadObject* _pThread)
	{
		while (_pThread->f_GetState() != NThread::EThreadState_EventWantQuit)
		{
			{ // Take new connections
				DMibLock(mp_NewConnectionsLock);
				while( !mp_NewConnectionsList.f_IsEmpty() )
				{
					mp_NewConnectionsList.f_GetIterator()->f_SetReportTo(&_pThread->m_EventWantQuit);
					mp_ConnectionsList.f_Insert( mp_NewConnectionsList.f_GetIterator() );
				}
			}

			for (auto ConnIter = mp_ConnectionsList.f_GetIterator()
				;ConnIter
				;)
			{
				if ((*ConnIter).f_IsConnected())
				{
					DMibTraceRaw("Processing Connection\n");

					(*ConnIter).f_Process();
					++ConnIter;
				}
				else
				{
					DMibTraceRaw("Removing Connection\n");

					auto CurIter = ConnIter;
					++ConnIter;

					NStorage::TCUniquePointer<CConnection> pConn = NStorage::TCUniquePointer<CConnection>(&*CurIter);
					mp_ConnectionsList.f_Remove(*CurIter);
				}
			}

			DMibTraceRaw("ConnectionWorker: Waiting\n");
//				NMib::NSys::fg_Thread_SmallestSleep();
			_pThread->m_EventWantQuit.f_Wait();
			DMibTraceRaw("ConnectionWorker: Woke\n");
		}

		return 0;
	}
}
