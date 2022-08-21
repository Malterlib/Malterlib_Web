// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/WeakActor>
#include <Mib/Encoding/Base64>
#include <Mib/Network/Sockets/TCP>
#include <Mib/Cryptography/Exception>

#include "Malterlib_Web_WebSocket.h"

namespace NMib::NWeb
{
	CWebSocketClientActor::CWebSocketClientActor(CWebsocketSettings const &_DefaultSettings)
		: mp_DefaultSettings(_DefaultSettings)
	{
	}

	CWebSocketClientActor::~CWebSocketClientActor()
	{
	}

	void CWebSocketClientActor::f_SetDefaultMaxMessageSize(mint _MaxMessageSize)
	{
		mp_DefaultSettings.m_MaxMessageSize = _MaxMessageSize;
	}

	void CWebSocketClientActor::f_SetDefaultFragmentationSize(mint _FragmentationSize)
	{
		mp_DefaultSettings.m_FragmentationSize = _FragmentationSize;
	}

	void CWebSocketClientActor::f_SetDefaultTimeout(fp64 _Timeout)
	{
		mp_DefaultSettings.m_Timeout = _Timeout;
	}

	NConcurrency::TCFuture<void> CWebSocketClientActor::fp_Destroy()
	{
		mp_PendingConnects.f_Clear();
		co_return {};
	}

	CWebSocketClientActor::CPendingConnection::~CPendingConnection()
	{
		*m_pDeleted = true;
	}

	NConcurrency::TCFuture<CWebSocketNewClientConnection> CWebSocketClientActor::f_Connect
		(
			NStr::CStr const& _ConnectToAddress
			, NStr::CStr const& _BindToAddress
			, NMib::NNetwork::ENetAddressType _PreferAddress
			, uint16 _Port
			, NStr::CStr const& _URI
			, NStr::CStr const& _Origin
			, NContainer::TCVector<NStr::CStr> const &_Protocols
			, NHTTP::CRequest &&_Request
			, NNetwork::FVirtualSocketFactory &&_SocketFactory
		)
	{
		if (!_SocketFactory)
			_SocketFactory = NNetwork::CSocket_TCP::fs_GetFactory();

		if (_ConnectToAddress.f_IsEmpty())
			co_return DMibErrorInstance("Connect to address cannot be empty");

		if (!mp_AddressResolver)
			mp_AddressResolver = NConcurrency::fg_ConstructActor<NNetwork::CResolveActor>();

		auto [ConnectToAdress, BindToAddress] = co_await
			(
			 	mp_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _ConnectToAddress, _PreferAddress)
			 	+ mp_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _BindToAddress, _PreferAddress)
			)
		;

		ConnectToAdress.f_SetPort(_Port);

		auto Settings = mp_DefaultSettings;

		if (!mp_DefaultSettings.m_bTimeoutForUnixSockets && ConnectToAdress.f_GetType() == NNetwork::ENetAddressType_Unix)
			Settings.m_Timeout = 0.0;

		CPendingConnection *pPending;

		{
			CPendingConnection &Pending = mp_PendingConnects.f_Insert();
			pPending = &Pending;
			Pending.m_pSocket = _SocketFactory(_ConnectToAddress);
		}

		auto CleanupPending = NConcurrency::g_OnScopeExitActor / [this, pPendingDeleted = pPending->m_pDeleted, pPending]
			{
				if (pPendingDeleted->f_Load())
					return;
				mp_PendingConnects.f_Remove(*pPending);
			}
		;

		NConcurrency::TCPromise<CWebSocketNewClientConnection> Promise;

		try
		{
			NException::CDisableExceptionTraceScope DisableExceptionTrace;
			pPending->m_pSocket->f_AsyncConnect
				(
					ConnectToAdress
					,
				 	[
					 	=
					 	, pPendingDeleted = pPending->m_pDeleted
					 	, pReplied = NStorage::TCSharedPointer<NAtomic::TCAtomic<bool>>(fg_Construct(false))
					 	, WeakThis = fg_ThisActor(this).f_Weak()
					 	, CleanupPending = fg_Move(CleanupPending)
					 	, pRequest = NStorage::TCSharedPointer<NHTTP::CRequest>(fg_Construct(fg_Move(_Request)))
					]
				 	(::NMib::NNetwork::ENetTCPState _StateAdded) mutable
					{
						if (_StateAdded & (NNetwork::ENetTCPState_Closed | NNetwork::ENetTCPState_RemoteClosed))
						{
							if (!pReplied->f_Exchange(true))
							{
								auto This = WeakThis.f_Lock();
								if (This)
								{
									NConcurrency::g_Dispatch(This) / [pPendingDeleted, pPending, Promise, CleanupPending]
										{
											NStr::CStr Error;
											if (!pPendingDeleted->f_Load())
												Error = pPending->m_pSocket->f_GetCloseReason();
											else
												Error = "Client connection actor was deleted";

											Promise.f_SetException(DMibErrorInstance(Error));
										}
										> NConcurrency::NPrivate::fg_DirectResultActor() / [Promise](NConcurrency::TCAsyncResult<void> &&_Result)
										{
											if (!Promise.f_IsSet())
												Promise.f_SetException(DMibErrorInstance("Client connection actor was deleted"));
										}
									;
								}
								else
									Promise.f_SetException(DMibErrorInstance("Client connection actor was deleted"));
							}

							CleanupPending.f_Clear();
						}
						else if (_StateAdded & NNetwork::ENetTCPState_Connected)
						{
							if (pReplied->f_Exchange(true))
							{
								CleanupPending.f_Clear();
								return;
							}

							auto This = WeakThis.f_Lock();
							if (!This || pPendingDeleted->f_Load())
								return Promise.f_SetException(DMibErrorInstance("Client connection actor was deleted"));

							NStorage::TCUniquePointer<NNetwork::ICSocket> pNewSocket = fg_Move(pPending->m_pSocket);

							DMibFastCheck(pNewSocket->f_IsValid());

							NConcurrency::TCActor<CWebSocketActor> ConnectionActor = NConcurrency::fg_ConstructActor<CWebSocketActor>(true, Settings);

							// Capture here
							auto fFinishConnection = [=, &pNewSocket, &ConnectionActor, CleanupPending = fg_Move(CleanupPending)]() mutable
								{
									ConnectionActor(&CWebSocketActor::fp_SetSocket, fg_Move(pNewSocket)) > NConcurrency::fg_DiscardResult();

									ConnectionActor
										(
											&CWebSocketActor::fp_OnFinishClientConnection
											, NConcurrency::g_ActorFunctorWeak(NConcurrency::fg_ThisConcurrentActor())
											/ [Promise, ConnectionActor, CleanupPending = fg_Move(CleanupPending), AllowDestroy = NConcurrency::g_AllowWrongThreadDestroy]
											(CWebSocketActor::EFinishConnectionResult _Result, CWebSocketActor::CClientConnectionInfo &&_ConnectionInfo) mutable
											-> NConcurrency::TCFuture<void>
											{
												if (_Result == CWebSocketActor::EFinishConnectionResult_Error)
													Promise.f_SetException(DMibErrorInstance(_ConnectionInfo.m_Error));
												else
												{
													CWebSocketNewClientConnection NewConnection
														(
															fg_Move(*_ConnectionInfo.m_pResponse)
															, fg_Move(_ConnectionInfo.m_Protocol)
															, fg_Move(ConnectionActor)
															, fg_Move(_ConnectionInfo.m_pSocketInfo)
															, _ConnectionInfo.m_PeerAddress
														)
													;

													Promise.f_SetResult(fg_Move(NewConnection));
												}
												CleanupPending.f_Clear();

												co_return {};
											}
											, fg_Move(*pRequest)
											, _ConnectToAddress
											, _URI
											, _Origin
											, _Protocols
										)
										> This / [pPendingDeleted, pPending](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
										{
											if (_Result && !pPendingDeleted->f_Load())
												pPending->m_OnFinishConnectionSubscription = fg_Move(*_Result);
										}
									;
								}
							;

							// Lambda will be destroyed when this is called, this is why we capture everything in fFinishConnection
							pNewSocket->f_SetOnStateChange
								(
									[WeakConnectionActor = ConnectionActor.f_Weak()](NNetwork::ENetTCPState _StateAdded)
									{
										auto ConnectionActor = WeakConnectionActor.f_Lock();
										if (!ConnectionActor)
											return;
										ConnectionActor(&CWebSocketActor::fp_StateAdded, _StateAdded) > NConcurrency::fg_DiscardResult();
									}
								)
							;

							fFinishConnection();
						}
					}
					, BindToAddress
				)
			;
		}
		catch (NCryptography::CExceptionCryptography const &_Exception)
		{
			co_return _Exception.f_ExceptionPointer();
		}
		catch (NNetwork::CExceptionNet const &_Exception)
		{
			co_return _Exception.f_ExceptionPointer();
		}

		co_return co_await Promise.f_MoveFuture();
	}
}
