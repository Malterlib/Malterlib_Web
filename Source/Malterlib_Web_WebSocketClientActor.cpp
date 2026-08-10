// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

	void CWebSocketClientActor::f_SetDefaultMaxMessageSize(umint _MaxMessageSize)
	{
		mp_DefaultSettings.m_MaxMessageSize = _MaxMessageSize;
	}

	void CWebSocketClientActor::f_SetDefaultFragmentationSize(umint _FragmentationSize)
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

	NConcurrency::TCFuture<CWebSocketNewClientConnection> CWebSocketClientActor::f_Connect(CConnectSettings _Settings)
	{
		if (!_Settings.m_SocketFactory)
			_Settings.m_SocketFactory = NNetwork::CSocket_TCP::fs_GetFactory();

		if (_Settings.m_ConnectToAddress.f_IsEmpty())
			co_return DMibErrorInstance("Connect to address cannot be empty");

		if (!mp_AddressResolver)
			mp_AddressResolver = NConcurrency::fg_ConstructActor<NNetwork::CResolveActor>();

		auto [ConnectToAdress, BindToAddress] = co_await
			(
				mp_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _Settings.m_ConnectToAddress, _Settings.m_PreferAddress)
				+ mp_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _Settings.m_BindAddress, _Settings.m_PreferAddress)
			)
		;

		if (_Settings.m_Port)
			ConnectToAdress.f_SetPort(_Settings.m_Port);

		auto Settings = mp_DefaultSettings;

		Settings.m_bAllowUnmaskedFrames = _Settings.m_bAllowUnmaskedFrames;

		if (!mp_DefaultSettings.m_bTimeoutForUnixSockets && ConnectToAdress.f_GetType() == NNetwork::ENetAddressType_Unix)
			Settings.m_Timeout = 0.0;

		NStr::CStr ConnectToAddress = _Settings.m_ConnectToAddress;
		NStr::CStr URI = fg_Move(_Settings.m_URI);
		NStr::CStr Origin = fg_Move(_Settings.m_Origin);
		NContainer::TCVector<NStr::CStr> Protocols = fg_Move(_Settings.m_Protocols);

		CPendingConnection *pPending;

		{
			CPendingConnection &Pending = mp_PendingConnects.f_Insert();
			pPending = &Pending;
			Pending.m_pSocket = _Settings.m_SocketFactory(ConnectToAddress);
		}

		auto CleanupPending = NConcurrency::g_OnScopeExitActor / [this, pPendingDeleted = pPending->m_pDeleted, pPending]
			{
				if (pPendingDeleted->f_Load())
					return;
				mp_PendingConnects.f_Remove(*pPending);
			}
		;

		NConcurrency::TCPromiseFuturePair<CWebSocketNewClientConnection> Promise;

		auto CaptureScope = co_await NConcurrency::g_CaptureExceptions.f_Specific<NCryptography::CExceptionCryptography, NNetwork::CExceptionNet>();

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
					, pRequest = NStorage::TCSharedPointer<NHTTP::CRequest>(fg_Construct(fg_Move(_Settings.m_Request)))
					, Promise = fg_Move(Promise.m_Promise)
				]
				(::NMib::NNetwork::ENetTCPState _StateAdded) mutable
				{
					if (_StateAdded & (NNetwork::ENetTCPState_Closed | NNetwork::ENetTCPState_RemoteClosed))
					{
						if (!pReplied->f_Exchange(true))
						{
							auto pCleanupPromise = g_OnScopeExitShared / [Promise]
								{
									if (!Promise.f_IsSet())
										Promise.f_SetException(DMibErrorInstance("Client connection actor was deleted"));
								}
							;

							auto This = WeakThis.f_Lock();
							if (This)
							{
								NConcurrency::g_Dispatch(This) / [pPendingDeleted, pPending, Promise, CleanupPending, pCleanupPromise]
									{
										NStr::CStr Error;
										if (!pPendingDeleted->f_Load())
											Error = pPending->m_pSocket->f_GetCloseReason();
										else
											Error = "Client connection actor was deleted";

										Promise.f_SetException(DMibErrorInstance(Error));
									}
									> NConcurrency::g_DiscardResult
								;
							}
						}

						CleanupPending.f_Clear();
					}
					else if (_StateAdded & NNetwork::ENetTCPState_Connected)
					{
						if (pReplied->f_Exchange(true))
							return (void)CleanupPending.f_Clear();

						auto pCleanupPromise = g_OnScopeExitShared / [Promise]
							{
								if (!Promise.f_IsSet())
									Promise.f_SetException(DMibErrorInstance("Client connection actor was deleted"));
							}
						;

						auto This = WeakThis.f_Lock();
						if (!This || pPendingDeleted->f_Load())
							return (void)pCleanupPromise.f_Clear();

						NConcurrency::g_Dispatch(This) /
							[
								pPendingDeleted
								, pPending
								, Promise
								, pCleanupPromise
								, CleanupPending = fg_Move(CleanupPending)
								, Settings
								, pRequest = fg_Move(pRequest)
								, ConnectToAddress
								, URI
								, Origin
								, Protocols
							]() mutable
							{
								if (pPendingDeleted->f_Load())
									return (void)pCleanupPromise.f_Clear();

								NStorage::TCUniquePointer<NNetwork::ICSocket> pNewSocket = fg_Move(pPending->m_pSocket);

								DMibFastCheck(pNewSocket->f_IsValid());

								NConcurrency::TCActor<CWebSocketActor> ConnectionActor = NConcurrency::fg_ConstructActor<CWebSocketActor>(true, Settings);

								// Capture here
								auto fFinishConnection = [=, &pNewSocket, &ConnectionActor, CleanupPending = fg_Move(CleanupPending)]() mutable
									{
										ConnectionActor.f_Bind<&CWebSocketActor::fp_SetSocket>(fg_Move(pNewSocket)).f_DiscardResult();

										ConnectionActor
											(
												&CWebSocketActor::fp_OnFinishClientConnection
												, NConcurrency::g_ActorFunctorWeak(NConcurrency::fg_ThisConcurrentActor())
												/ [Promise, pCleanupPromise, ConnectionActor, CleanupPending = fg_Move(CleanupPending), AllowDestroy = NConcurrency::g_AllowWrongThreadDestroy]
												(CWebSocketActor::EFinishConnectionResult _Result, CWebSocketActor::CClientConnectionInfo _ConnectionInfo) mutable
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
												, ConnectToAddress
												, URI
												, Origin
												, Protocols
											)
											> [pPendingDeleted, pPending](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
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
											ConnectionActor.f_Bind<&CWebSocketActor::fp_StateAdded>(_StateAdded).f_DiscardResult();
										}
									)
								;

								fFinishConnection();
							}
							> NConcurrency::g_DiscardResult
						;
					}
				}
				, BindToAddress
			)
		;

		co_return co_await fg_Move(Promise.m_Future);
	}
}
