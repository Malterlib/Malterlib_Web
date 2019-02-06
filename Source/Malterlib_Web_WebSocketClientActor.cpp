// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/WeakActor>
#include <Mib/Encoding/Base64>
#include <Mib/Network/Sockets/TCP>

#include "Malterlib_Web_WebSocket.h"

namespace NMib::NWeb
{
	CWebSocketClientActor::CWebSocketClientActor()
		: mp_MaxMessageSize(24*1024*1024)
		, mp_FragmentationSize(32*1024)
		, mp_Timeout(60.0)
	{
	}

	CWebSocketClientActor::~CWebSocketClientActor()
	{
	}

	void CWebSocketClientActor::f_SetDefaultMaxMessageSize(mint _MaxMessageSize)
	{
		mp_MaxMessageSize = _MaxMessageSize;
	}

	void CWebSocketClientActor::f_SetDefaultFragmentationSize(mint _FragmentationSize)
	{
		mp_FragmentationSize = _FragmentationSize;
	}

	void CWebSocketClientActor::f_SetDefaultTimeout(fp64 _Timeout)
	{
		mp_Timeout = _Timeout;
	}

	NConcurrency::TCFuture<void> CWebSocketClientActor::fp_Destroy()
	{
		return fg_Explicit();
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
		NConcurrency::TCPromise<CWebSocketNewClientConnection> Promise;

		if (_ConnectToAddress.f_IsEmpty())
		{
			Promise.f_SetException(DMibErrorInstance("Connect to address cannot be empty"));
			return Promise.f_MoveFuture();
		}

		if (!mp_AddressResolver)
			mp_AddressResolver = NConcurrency::fg_ConstructActor<NNetwork::CResolveActor>();

		NStorage::TCSharedPointer<NHTTP::CRequest> pRequest = fg_Construct(fg_Move(_Request));

		mp_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _ConnectToAddress, _PreferAddress)
			+ mp_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _BindToAddress, _PreferAddress)
			> [Promise, _Port, pRequest, this, _ConnectToAddress, _URI, _Origin, _Protocols, _SocketFactory]
			(
				NConcurrency::TCAsyncResult<NNetwork::CNetAddress> &&_Result
				, NConcurrency::TCAsyncResult<NNetwork::CNetAddress> &&_BindToResult
			)
			{
				if (!_Result)
					Promise.f_SetException(_Result);
				else if (!_BindToResult)
					Promise.f_SetException(_BindToResult);
				else
				{
					CPendingConnection &Pending = mp_PendingConnects.f_Insert();

					Pending.m_pSocket = _SocketFactory(_ConnectToAddress);

					auto *pPending = &Pending;

					auto Address = fg_Move(*_Result);
					Address.f_SetPort(_Port);

					auto WeakThis = fg_ThisActor(this).f_Weak();

					NStorage::TCSharedPointer<bool> pStateReceived = fg_Construct(false);

					try
					{
						NException::CDisableExceptionTraceScope DisableExceptionTrace;
						Pending.m_pSocket->f_AsyncConnect
							(
								fg_Move(Address)
								, [pStateReceived, pPending, WeakThis, Promise, this, pRequest, _ConnectToAddress, _URI, _Origin, _Protocols](::NMib::NNetwork::ENetTCPState _StateAdded)
								{
									auto This = WeakThis.f_Lock();
									if (!This)
									{
										Promise.f_SetException(DMibErrorInstance("Client connection actor was deleted"));
										return;
									}
									This
										(
											&CActor::f_Dispatch
											, [WeakThis, pStateReceived, _StateAdded, this, pPending, Promise, pRequest, _ConnectToAddress, _URI, _Origin, _Protocols]
											{
												auto This = WeakThis.f_Lock();
												if (!This)
												{
													Promise.f_SetException(DMibErrorInstance("Client connection actor was deleted"));
													return;
												}
												if (*pStateReceived)
													return;
												if (_StateAdded == NNetwork::ENetTCPState_Connected)
												{
													NConcurrency::TCActor<CWebSocketActor> ConnectionActor
														= NConcurrency::fg_ConstructActor<CWebSocketActor>(true, mp_MaxMessageSize, mp_FragmentationSize, mp_Timeout)
													;
													NStorage::TCUniquePointer<NNetwork::ICSocket> pNewSocket = fg_Move(pPending->m_pSocket);

													NConcurrency::TCWeakActor<CWebSocketActor> WeakConnectionActor = ConnectionActor;
													pNewSocket->f_SetOnStateChange
														(
															[WeakConnectionActor](NNetwork::ENetTCPState _StateAdded)
															{
																auto ConnectionActor = WeakConnectionActor.f_Lock();
																if (ConnectionActor)
																{
																	ConnectionActor(&CWebSocketActor::fp_StateAdded, _StateAdded)
																		> NConcurrency::fg_DiscardResult()
																	;
																}
															}
														)
													;

													{
														ConnectionActor(&CWebSocketActor::fp_SetSocket, fg_Move(pNewSocket))
															> NConcurrency::fg_DiscardResult()
														;
													}

													NStorage::TCSharedPointer<NAtomic::TCAtomic<bool>> pRemovedPending = fg_Construct();
													ConnectionActor
														(
															&CWebSocketActor::fp_OnFinishClientConnection
															, This
															, [this, Promise, ConnectionActor, pRemovedPending, pPending]
															(
																CWebSocketActor::EFinishConnectionResult _Result
																, CWebSocketActor::CClientConnectionInfo &&_ConnectionInfo
															)
															mutable
															{
																if (_Result == CWebSocketActor::EFinishConnectionResult_Error)
																{
																	Promise.f_SetException(DMibErrorInstance(_ConnectionInfo.m_Error));
																}
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
																*pRemovedPending = true;
																mp_PendingConnects.f_Remove(*pPending);
															}
															, fg_Move(*pRequest)
															, _ConnectToAddress
															, _URI
															, _Origin
															, _Protocols
														)
														> This / [pRemovedPending, pPending](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
														{
															if (_Result && !*pRemovedPending)
																pPending->m_OnFinishConnectionSubscription = fg_Move(*_Result);
														}
													;
													*pStateReceived = true;
												}
												else if (_StateAdded == NNetwork::ENetTCPState_Closed)
												{
													Promise.f_SetException(DMibErrorInstance(pPending->m_pSocket->f_GetCloseReason()));
													*pStateReceived = true;
													mp_PendingConnects.f_Remove(*pPending);
												}

											}
										)
										> NConcurrency::fg_DiscardResult()
									;

								}
								, *_BindToResult
							)
						;
					}
					catch (NNetwork::CExceptionNet const &_Exception)
					{
						Promise.f_SetException(_Exception);
					}
				}
			}
		;

		return Promise.f_MoveFuture();
	}
}
