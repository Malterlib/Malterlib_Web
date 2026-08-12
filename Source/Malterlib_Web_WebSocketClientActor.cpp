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

	// The maximum fragment size travels with the fragmentation size: it bounds the frames
	// this peer accepts, so it has to be at least what the other end fragments at
	void CWebSocketClientActor::f_SetDefaultFragmentationSize(umint _FragmentationSize, umint _MaxFragmentSize)
	{
		mp_DefaultSettings.m_FragmentationSize = _FragmentationSize;
		mp_DefaultSettings.m_MaxFragmentSize = _MaxFragmentSize;
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
		Settings.m_bNegotiateUnmaskedFrames = _Settings.m_bNegotiateUnmaskedFrames;
		if (_Settings.m_FragmentationSize)
			Settings.m_FragmentationSize = _Settings.m_FragmentationSize;
		if (_Settings.m_MaxFragmentSize)
			Settings.m_MaxFragmentSize = _Settings.m_MaxFragmentSize;
		// The connect setting is 64 bits wide; a window past what umint holds is clamped to it. The
		// bytes the window bounds are buffers this process holds, so umint counts those as well
		if (_Settings.m_SendWindowBytes)
			Settings.m_SendWindowBytes = (umint)fg_Min(_Settings.m_SendWindowBytes, uint64(TCLimitsInt<umint>::mc_Max));

		if (!mp_DefaultSettings.m_bTimeoutForUnixSockets && ConnectToAdress.f_GetType() == NNetwork::ENetAddressType_Unix)
			Settings.m_Timeout = 0.0;

		NStr::CStr ConnectToAddress = _Settings.m_ConnectToAddress;
		NStr::CStr URI = fg_Move(_Settings.m_URI);
		NStr::CStr Origin = fg_Move(_Settings.m_Origin);
		NContainer::TCVector<NStr::CStr> Protocols = fg_Move(_Settings.m_Protocols);

		// The actor's own manager, not the global one: an actor hosted elsewhere must get loops
		// whose threads run its pool, or every socket callback crosses managers and the global
		// manager is started as a side effect
		NConcurrency::CIoLoopBinding IoBinding = f_ConcurrencyManager().f_PickIoLoopBinding(CWebSocketActor::mc_Priority);

		CPendingConnection *pPending;

		{
			CPendingConnection &Pending = mp_PendingConnects.f_Insert();
			pPending = &Pending;
			Pending.m_IoBinding = IoBinding;
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

		// The connection actor must live on the same manager whose loop the socket is bound to,
		// or stopping that manager destroys a loop the live socket still references; taken here,
		// where the actor is at hand, and carried into the callbacks as a plain pointer
		auto *pConnectionManager = &f_ConcurrencyManager();

		// Connecting is what starts the socket, so the loop has to be chosen around this call for
		// the socket to be registered with it rather than with the shared poller
		{
			NConcurrency::CIoLoopCreateScope IoLoopScope(pPending->m_IoBinding);

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
									, pConnectionManager
								]() mutable
								{
									if (pPendingDeleted->f_Load())
										return (void)pCleanupPromise.f_Clear();

									NStorage::TCUniquePointer<NNetwork::ICSocket> pNewSocket = fg_Move(pPending->m_pSocket);

									DMibFastCheck(pNewSocket->f_IsValid());

									NConcurrency::TCActor<CWebSocketActor> ConnectionActor = pConnectionManager->f_ConstructActor(fg_Construct<CWebSocketActor>(true, Settings));

									// Seed the scheduler placement to the bound queue so even the
									// first job runs where the loop reports the socket's events;
									// no pinning — every later job keeps the marker fresh
									if (pPending->m_IoBinding.m_pLoop)
										ConnectionActor->f_SetInitialQueue(pPending->m_IoBinding.m_iQueue);

									auto fFinishConnection = [=, &pNewSocket, &ConnectionActor, CleanupPending = fg_Move(CleanupPending)]() mutable
										{
											ConnectionActor.f_Bind<&CWebSocketActor::fp_SetSocket>(fg_Move(pNewSocket)).f_DiscardResult();

											ConnectionActor
												(
													&CWebSocketActor::fp_OnFinishClientConnection
													// The actor's own manager, not the global one: the finish
													// callback must run where the connection actor lives, or a
													// client hosted off the global manager starts it as a side
													// effect and the handshake completion crosses managers
													, NConcurrency::g_ActorFunctorWeak(pConnectionManager->f_GetThisConcurrentActor())
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
																	, _ConnectionInfo.m_FragmentationSize
																	, _ConnectionInfo.m_MaxFragmentSize
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

									// Replacing the state handler destroys the lambda currently executing, so
									// everything used afterwards is captured by fFinishConnection instead
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
		}

		co_return co_await fg_Move(Promise.m_Future);
	}
}
