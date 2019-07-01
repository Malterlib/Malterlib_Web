// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib
 
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Web_WebSocket.h"
#include "Malterlib_Web_WebSocketServerActor_Internal.h"
#include "Malterlib_Web_WebSocketServerActor_Internal_Listen.h"

#include <Mib/Network/Sockets/TCP>

namespace NMib::NWeb
{
	using namespace NWebSocket;
	CWebSocketServerActor::CWebSocketServerActor()
		: mp_pInternal(fg_Construct(this))
	{
	}

	CWebSocketServerActor::~CWebSocketServerActor()
	{
	}

	void CWebSocketServerActor::f_SetDefaultMaxMessageSize(mint _MaxMessageSize)
	{
		DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
		mp_pInternal->m_MaxMessageSize = _MaxMessageSize;
	}

	void CWebSocketServerActor::f_SetDefaultFragmentationSize(mint _FragmentationSize)
	{
		DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
		mp_pInternal->m_FragmentationSize = _FragmentationSize;
	}

	void CWebSocketServerActor::f_SetDefaultTimeout(fp64 _Timeout)
	{
		DMibRequire(mp_pInternal->m_ListenSockets.f_IsEmpty());
		mp_pInternal->m_Timeout = _Timeout;
	}

	void CWebSocketServerActor::CInternal::f_Clear()
	{
		m_ListenSockets.f_Clear();
		m_OnNewConnection.f_Clear();
		m_OnFailedConnection.f_Clear();
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CWebSocketServerActor::f_StartListenAddress
		(
			NContainer::TCVector<NNetwork::CNetAddress> &&_AddressesToListenTo
			, NMib::NNetwork::ENetFlag _ListenFlags
			, NConcurrency::TCActor<NConcurrency::CActor> const &_Actor
			, NFunction::TCFunctionMovable<void (CWebSocketNewServerConnection && _Connection)> &&_fNewConnection
			, NFunction::TCFunctionMovable<void (CWebSocketActor::CConnectionInfo && _ConnectionInfo)> &&_fFailedConnection
			, NNetwork::FVirtualSocketFactory &&_SocketFactory
		)
	{
		NNetwork::FVirtualSocketFactory SocketFactory = fg_Move(_SocketFactory);
		if (!SocketFactory)
			SocketFactory = NNetwork::CSocket_TCP::fs_GetFactory();

		try
		{
			if (!mp_pInternal->m_ListenSockets.f_IsEmpty())
				DMibError("Socket server is already listening");

			NConcurrency::TCActorResultVector<void> SetSocketResults;
			NConcurrency::CActorSubscription Reference;
			{
				mp_pInternal->f_Clear();
				auto NewReference = mp_pInternal->m_OnNewConnection.f_Register(_Actor, fg_Move(_fNewConnection));
				auto FailedReference = mp_pInternal->m_OnFailedConnection.f_Register(_Actor, fg_Move(_fFailedConnection));
				Reference = fg_CombinedCallbackReference(fg_Move(NewReference), fg_Move(FailedReference));

				mint nListenTo = _AddressesToListenTo.f_GetLen();

				mp_pInternal->m_ListenSockets.f_SetLen(nListenTo);


				for (mint i = 0; i < nListenTo; ++i)
				{
					NNetwork::CNetAddress &Address = _AddressesToListenTo[i];

					NConcurrency::TCActor<CListenActor> &ListenActor = mp_pInternal->m_ListenSockets[i];
					ListenActor = NConcurrency::fg_ConstructActor<CListenActor>(fg_ThisActor(this), mp_pInternal->m_MaxMessageSize, mp_pInternal->m_FragmentationSize, mp_pInternal->m_Timeout);

					NConcurrency::TCWeakActor<CListenActor> WeakListenActor = ListenActor;

					NStorage::TCUniquePointer<NNetwork::ICSocket> pListenSocket = SocketFactory("");
					NException::CDisableExceptionTraceScope DisableExceptionTrace;
					pListenSocket->f_Listen
						(
							Address
							, [WeakListenActor](NNetwork::ENetTCPState _StateAdded)
							{
								auto ListenActor = WeakListenActor.f_Lock();
								if (!ListenActor)
									return;
								ListenActor(&CListenActor::f_StateAdded, _StateAdded) > NConcurrency::fg_DiscardResult();
							}
							, _ListenFlags
						)
					;

					ListenActor(&CListenActor::f_SetSocket, fg_Move(pListenSocket)) > SetSocketResults.f_AddResult();
				}
			}
			
			co_await SetSocketResults.f_GetResults() | NConcurrency::g_Unwrap;

			co_return fg_Move(Reference);
		}
		catch (NException::CException const &_Exception)
		{
			mp_pInternal->f_Clear();
			co_return _Exception.f_ExceptionPointer();
		}

		DMibNeverGetHere;
		co_return {};
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CWebSocketServerActor::f_StartListen
		(
			uint16 _StartListen
			, uint16 _nListen
			, NMib::NNetwork::ENetFlag _ListenFlags
			, NConcurrency::TCActor<NConcurrency::CActor> const &_Actor
			, NFunction::TCFunctionMovable<void (CWebSocketNewServerConnection && _Connection)> &&_fNewConnection
			, NFunction::TCFunctionMovable<void (CWebSocketActor::CConnectionInfo && _ConnectionInfo)> &&_fFailedConnection
			, NNetwork::FVirtualSocketFactory &&_SocketFactory
		)
	{
		NContainer::TCVector<NNetwork::CNetAddress> AddressesToListenTo;
		for (mint i = 0; i < _nListen; ++i)
		{
			NNetwork::CNetAddressTCPv4 AnyAddress;
			AnyAddress.m_Port = _StartListen + i;
			NNetwork::CNetAddress Address;
			Address.f_Set(AnyAddress);
			AddressesToListenTo.f_Insert(fg_Move(Address));
		}
		return f_StartListenAddress(fg_Move(AddressesToListenTo), _ListenFlags, _Actor, fg_Move(_fNewConnection), fg_Move(_fFailedConnection), fg_Move(_SocketFactory));
	}

	NConcurrency::TCFuture<void> CWebSocketServerActor::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		NConcurrency::TCActorResultVector<void> Results;
		for (auto &ListenSocket : Internal.m_ListenSockets)
			ListenSocket->f_Destroy() > Results.f_AddResult();

		for (auto &Connection : Internal.m_Subscriptions)
		{
			if (Connection)
				Connection->f_Destroy() > Results.f_AddResult();
		}

		Internal.m_Subscriptions.f_Clear();

		co_await Results.f_GetResults() | NConcurrency::g_Unwrap;

		co_return {};
	}

	void CWebSocketServerActor::fp_AddConnection(NConcurrency::TCActor<CWebSocketActor> &&_Connection)
	{
		if (mp_bDestroyed)
			return;

		auto pSubscription = &mp_pInternal->m_Subscriptions.f_Insert();
		NStorage::TCSharedPointer<NAtomic::TCAtomic<bool>> pHandled = fg_Construct(false);
		_Connection
			(
				&CWebSocketActor::fp_OnFinishServerConnection
				, fg_ThisActor(this)
				, [this, _Connection, pSubscription, pHandled]
				(CWebSocketActor::EFinishConnectionResult _Result, CWebSocketActor::CConnectionInfo &&_ConnectionInfo)
				{
					if (*pHandled || mp_bDestroyed)
						return;

					switch (_Result)
					{
					case CWebSocketActor::EFinishConnectionResult_Error:
						{
							mp_pInternal->m_OnFailedConnection(fg_Move(_ConnectionInfo)) > NConcurrency::fg_DiscardResult();
							pSubscription->f_Clear();
						}
						break;
					case CWebSocketActor::EFinishConnectionResult_Success:
						{
							auto Protocols = _ConnectionInfo.m_Protocols;
							CWebSocketNewServerConnection Connection(fg_Move(_ConnectionInfo), fg_Move(Protocols), _Connection);
							mp_pInternal->m_OnNewConnection(fg_Move(Connection)) > NConcurrency::fg_DiscardResult();
							pSubscription->f_Clear();
						}
						break;
					default:
						DMibPDebugBreak;
						break;
					}

					*pHandled = true;
					mp_pInternal->m_Subscriptions.f_Remove(*pSubscription);
				}
			)
			> [this, pSubscription, pHandled](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
			{
				if (!*pHandled && !mp_bDestroyed)
					*pSubscription = fg_Move(*_Result);
			}
		;
	}
}
