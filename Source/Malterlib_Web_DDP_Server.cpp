
#include "Malterlib_Web_DDP_Server.h"
#include "Malterlib_Web_DDP_Client.h"

#include <Mib/Concurrency/ActorCallbackManager>
#include <Mib/Concurrency/Actor/Timer>

namespace NMib::NWeb
{
	struct CDDPServerConnection::CInternal
	{
		CDDPServerConnection *mp_pServerConnection;
		NStorage::TCUniquePointer<CWebSocketNewServerConnection> m_pNewWebsocketConnection;

		NConcurrency::TCActor<CWebSocketActor> m_WebSocket;
		NConcurrency::CActorSubscription m_WebSocketCallbacks;

		NConcurrency::TCActorSubscriptionManager<void (CConnectionInfo const &_MethodInfo)> m_OnConnection;
		NConcurrency::TCActorSubscriptionManager<void (CMethodInfo const &_MethodInfo)> m_OnMethod;
		NConcurrency::TCActorSubscriptionManager<void (CSubscribeInfo const &_SubscribeInfo)> m_OnSubscribe;
		NConcurrency::TCActorSubscriptionManager<void (NStr::CStr const &_ID)> m_OnUnSubscribe;
		NConcurrency::TCActorSubscriptionManager<void (NStr::CStr const &_Error)> m_OnError;
		NConcurrency::TCActorSubscriptionManager<void (EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> m_OnClose;


		NConcurrency::CActorSubscription m_SockJSHeartbeatCallback;

		EConnectionType m_ConnectionType;

		CInternal(CDDPServerConnection *_pDDPServerConnection, CWebSocketNewServerConnection &&_NewWebsocketConnection, EConnectionType _ConnectionType)
			: mp_pServerConnection(_pDDPServerConnection)
			, m_pNewWebsocketConnection(fg_Construct(fg_Move(_NewWebsocketConnection)))
			, m_WebSocket()
			, m_WebSocketCallbacks()
			, m_OnConnection(mp_pServerConnection, false)
			, m_OnMethod(mp_pServerConnection, false)
			, m_OnSubscribe(mp_pServerConnection, false)
			, m_OnUnSubscribe(mp_pServerConnection, false)
			, m_OnError(mp_pServerConnection, false)
			, m_OnClose(mp_pServerConnection, false)
			, m_ConnectionType(_ConnectionType)
		{
		}

		void f_SendMessage(NEncoding::CEJSON const &_Data);

		void fp_ReceiveMessage(NStr::CStr const &_Message);
		void fp_ReceiveMessage(NEncoding::CEJSON const &_Message);
		void fp_OnError(NStr::CStr const &_Message);
	};

	void CDDPServerConnection::CInternal::f_SendMessage(NEncoding::CEJSON const &_Data)
	{
		if (m_ConnectionType != EConnectionType_SockJSWebsocket)
			m_WebSocket(&CWebSocketActor::f_SendText, _Data.f_ToString(nullptr), 0) > NConcurrency::fg_DiscardResult();
		else
		{
			NEncoding::CEJSON MessageArray;
			MessageArray.f_Insert(_Data.f_ToString(nullptr));
			m_WebSocket(&CWebSocketActor::f_SendText, "a" + MessageArray.f_ToString(nullptr), 0) > NConcurrency::fg_DiscardResult();
		}
	}

	void CDDPServerConnection::CInternal::fp_ReceiveMessage(NEncoding::CEJSON const &_Message)
	{
		NEncoding::CEJSON const &JSON = _Message;

		auto *pValue = JSON.f_GetMember("msg");
		if (!pValue)
		{
			fp_OnError(NStr::fg_Format("No msg in DDP packet {}", JSON));
			return;
		}

		if (pValue->f_Type() != NEncoding::EJSONType_String)
		{
			fp_OnError("Invalid type for msg (should be String)");
			return;
		}

		auto &MessageType = pValue->f_String();
		if (MessageType == "connect")
		{
			auto pVersion = JSON.f_GetMember("version");

			if (!fg_ValidateType(pVersion, NEncoding::EJSONType_String) || pVersion->f_String() != "1")
			{
				NEncoding::CEJSON Message;
				Message["msg"] = "failed";
				Message["version"] = "1";

				f_SendMessage(Message);
				return;
			}

			if (m_OnConnection.f_IsEmpty())
				mp_pServerConnection->fp_AcceptConnection(NStr::CStr());
			else
			{
				CConnectionInfo ConnectionInfo{mp_pServerConnection};
				auto pSessionID = JSON.f_GetMember("session");
				if (fg_ValidateType(pSessionID, NEncoding::EJSONType_String))
					ConnectionInfo.m_Session = pSessionID->f_String();
				m_OnConnection(ConnectionInfo) > NConcurrency::fg_DiscardResult();
			}
		}
		else if (MessageType == "method")
		{
			auto pMethodID = JSON.f_GetMember("id");
			if (!fg_ValidateType(pMethodID, NEncoding::EJSONType_String) || pMethodID->f_String().f_IsEmpty())
			{
				fp_OnError("(method) Invalid MethodID, either empty or not a String type");
				return;
			}

			auto pName = JSON.f_GetMember("method");
			if (!fg_ValidateType(pName, NEncoding::EJSONType_String) || pName->f_String().f_IsEmpty())
			{
				fp_OnError("(method) Invalid Method name, either empty or not a String type");
				return;
			}

			CMethodInfo MethodInfo(mp_pServerConnection);
			MethodInfo.m_ID = pMethodID->f_String();
			MethodInfo.m_Name = pName->f_String();

			auto pParams = JSON.f_GetMember("params");
			if (fg_ValidateType(pParams, NEncoding::EJSONType_Array))
				MethodInfo.m_Parameters = pParams->f_Array();

			auto pRandomSeed = JSON.f_GetMember("randomSeed");
			if (pRandomSeed)
				MethodInfo.m_RandomSeed = *pRandomSeed;

			m_OnMethod(MethodInfo) > NConcurrency::fg_DiscardResult();
		}
		else if (MessageType == "sub")
		{
			auto pSubscriptionID = JSON.f_GetMember("id");
			if (!fg_ValidateType(pSubscriptionID, NEncoding::EJSONType_String) || pSubscriptionID->f_String().f_IsEmpty())
			{
				fp_OnError("(sub) Invalid Subscription ID, either empty or not a String type");
				return;
			}

			auto pName = JSON.f_GetMember("name");
			if (!fg_ValidateType(pName, NEncoding::EJSONType_String) || pName->f_String().f_IsEmpty())
			{
				fp_OnError("(sub) Invalid Subscription name, either empty or not a String type");
				return;
			}

			CSubscribeInfo SubscribeInfo(mp_pServerConnection);
			SubscribeInfo.m_ID = pSubscriptionID->f_String();
			SubscribeInfo.m_Name = pName->f_String();

			auto pParams = JSON.f_GetMember("params");
			if (fg_ValidateType(pParams, NEncoding::EJSONType_Array))
				SubscribeInfo.m_Parameters = pParams->f_Array();

			m_OnSubscribe(SubscribeInfo) > NConcurrency::fg_DiscardResult();
		}
		else if (MessageType == "unsub")
		{
			auto pSubscriptionID = JSON.f_GetMember("id");
			if (!fg_ValidateType(pSubscriptionID, NEncoding::EJSONType_String) || pSubscriptionID->f_String().f_IsEmpty())
			{
				fp_OnError("(unsub) Invalid Subscription ID, either empty or not a String type");
				return;
			}

			m_OnUnSubscribe(pSubscriptionID->f_String()) > NConcurrency::fg_DiscardResult();
		}
		else if (MessageType == "ping")
		{
			NEncoding::CEJSON Reply;
			Reply["msg"] = "pong";

			if (auto pID = JSON.f_GetMember("id"))
				Reply["id"] = *pID;

			f_SendMessage(Reply);
		}
		else
		{
			fp_OnError(fg_Format("Unknown message: {}\n", MessageType));
		}
	}

	void CDDPServerConnection::CInternal::fp_ReceiveMessage(NStr::CStr const &_Message)
	{
		try
		{
			NEncoding::CEJSON JSON = NEncoding::CEJSON::fs_FromString(_Message);

			if (m_ConnectionType != EConnectionType_SockJSWebsocket)
				fp_ReceiveMessage(JSON);
			else
			{
				if (JSON.f_IsArray())
				{
					for (auto &Message : JSON.f_Array())
						fp_ReceiveMessage(NEncoding::CEJSON::fs_FromString(Message.f_String()));
				}
				else
					fp_ReceiveMessage(JSON);
			}

		}
		catch (NException::CException const &_Exception)
		{
			fp_OnError(fg_Format("Processing DDP message failed: {}", _Exception.f_GetErrorStr()));
		}
	}

	void CDDPServerConnection::CInternal::fp_OnError(NStr::CStr const &_Message)
	{
		m_OnError(_Message) > NConcurrency::fg_DiscardResult();
	}

	struct CDDPServerConnection::CConnectionInfo::CInternal
	{
		CInternal(CDDPServerConnection *_pDDPConnection)
			: mp_DDPConnection(fg_ThisActor(_pDDPConnection).f_Weak())
		{
		}

		NConcurrency::TCWeakActor<CDDPServerConnection> mp_DDPConnection;
	};

	CDDPServerConnection::CConnectionInfo::~CConnectionInfo()
	{
	}

	CDDPServerConnection::CConnectionInfo::CConnectionInfo(CConnectionInfo const &_Other) = default;
	CDDPServerConnection::CConnectionInfo &CDDPServerConnection::CConnectionInfo::operator =(CConnectionInfo &&_Other) = default;

	CDDPServerConnection::CConnectionInfo::CConnectionInfo(CConnectionInfo &&_Other) = default;
	CDDPServerConnection::CConnectionInfo &CDDPServerConnection::CConnectionInfo::operator =(CConnectionInfo const &_Other) = default;

	void CDDPServerConnection::CConnectionInfo::f_Accept(NStr::CStr const &_Session) const
	{
		if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
			Actor(&CDDPServerConnection::fp_AcceptConnection, _Session) > NConcurrency::fg_DiscardResult();
	}

	void CDDPServerConnection::CConnectionInfo::f_Reject() const
	{
		if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
			Actor(&CDDPServerConnection::fp_RejectConnection) > NConcurrency::fg_DiscardResult();
	}


	CDDPServerConnection::CConnectionInfo::CConnectionInfo(CDDPServerConnection *_pDDPConnection)
		: mp_Internal(_pDDPConnection)
	{
	}

	struct CDDPServerConnection::CMethodInfo::CInternal
	{
		CInternal(CDDPServerConnection *_pDDPConnection)
			: mp_DDPConnection(fg_ThisActor(_pDDPConnection).f_Weak())
		{
		}

		NConcurrency::TCWeakActor<CDDPServerConnection> mp_DDPConnection;
	};


	CDDPServerConnection::CMethodInfo::CMethodInfo(CDDPServerConnection *_pDDPConnection)
		: mp_Internal(_pDDPConnection)
	{
	}

	CDDPServerConnection::CMethodInfo::CMethodInfo(CMethodInfo const &_Other) = default;
	CDDPServerConnection::CMethodInfo::CMethodInfo(CMethodInfo &&_Other) = default;

	CDDPServerConnection::CMethodInfo &CDDPServerConnection::CMethodInfo::operator =(CMethodInfo const &_Other) = default;
	CDDPServerConnection::CMethodInfo &CDDPServerConnection::CMethodInfo::operator =(CMethodInfo &&_Other) = default;

	CDDPServerConnection::CMethodInfo::~CMethodInfo()
	{
	}

	void CDDPServerConnection::CMethodInfo::f_Result(NEncoding::CEJSON const &_Result, bool _bUpdated) const
	{
		if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
			Actor(&CDDPServerConnection::fp_MethodResult, m_ID, _Result, _bUpdated) > NConcurrency::fg_DiscardResult();
	}

	void CDDPServerConnection::CMethodInfo::f_Error(NEncoding::CEJSON const &_Error) const
	{
		if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
			Actor(&CDDPServerConnection::fp_MethodError, m_ID, _Error) > NConcurrency::fg_DiscardResult();
	}

	struct CDDPServerConnection::CSubscribeInfo::CInternal
	{
		CInternal(CDDPServerConnection *_pDDPConnection)
			: mp_DDPConnection(fg_ThisActor(_pDDPConnection).f_Weak())
		{
		}

		NConcurrency::TCWeakActor<CDDPServerConnection> mp_DDPConnection;
	};

	CDDPServerConnection::CSubscribeInfo::CSubscribeInfo(CDDPServerConnection *_pDDPConnection)
		: mp_Internal(_pDDPConnection)
	{
	}

	CDDPServerConnection::CSubscribeInfo::~CSubscribeInfo()
	{
	}

	CDDPServerConnection::CSubscribeInfo::CSubscribeInfo(CSubscribeInfo const &_Other) = default;
	CDDPServerConnection::CSubscribeInfo::CSubscribeInfo(CSubscribeInfo &&_Other) = default;

	CDDPServerConnection::CSubscribeInfo &CDDPServerConnection::CSubscribeInfo::operator =(CSubscribeInfo const &_Other) = default;
	CDDPServerConnection::CSubscribeInfo &CDDPServerConnection::CSubscribeInfo::operator =(CSubscribeInfo &&_Other) = default;

	void CDDPServerConnection::CSubscribeInfo::f_Error(NEncoding::CEJSON const &_Error) const
	{
		if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
		{
			auto Error = _Error;
			if (!Error.f_IsValid())
			{
				Error["error"] = "sub-not-found";
				Error["reason"] = "Subscription not found";
			}
			Actor(&CDDPServerConnection::fp_SubscriptionError, m_ID, fg_Move(Error)) > NConcurrency::fg_DiscardResult();
		}
	}

	CDDPServerConnection::CDDPServerConnection(CWebSocketNewServerConnection &&_ServerConnection, EConnectionType _ConnectionType)
		: mp_pInternal(fg_Construct(this, fg_Move(_ServerConnection), _ConnectionType))
	{
	}

	CDDPServerConnection::~CDDPServerConnection()
	{
	}

	NConcurrency::TCContinuation<void> CDDPServerConnection::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		Internal.m_WebSocketCallbacks.f_Clear();

		NConcurrency::TCContinuation<void> Continuation;

		if (Internal.m_WebSocket)
			Internal.m_WebSocket->f_Destroy() > Continuation;
		else
			Continuation.f_SetResult();

		return Continuation;

	}

	NConcurrency::CActorSubscription CDDPServerConnection::f_Register
		(
			NConcurrency::TCActor<CActor> const &_Actor
			, NFunction::TCFunction<void (CConnectionInfo const &_MethodInfo)> &&_fOnConnection
			, NFunction::TCFunction<void (CMethodInfo const &_MethodInfo)> &&_fOnMethod
			, NFunction::TCFunction<void (CSubscribeInfo const &_SubscribeInfo)> &&_fOnSubscribe
			, NFunction::TCFunction<void (NStr::CStr const &_ID)> &&_fOnUnSubscribe
			, NFunction::TCFunction<void (NStr::CStr const &_Error)> &&_fOnError
			, NFunction::TCFunction<void (EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> &&_fOnClose
		)
	{
		auto &Internal = *mp_pInternal;

		NStorage::TCUniquePointer<NConcurrency::CCombinedCallbackReference> pCombinedReference = fg_Construct();

		if (_fOnConnection)
			pCombinedReference->m_References.f_Insert(Internal.m_OnConnection.f_Register(_Actor, fg_Move(_fOnConnection)));

		if (_fOnMethod)
			pCombinedReference->m_References.f_Insert(Internal.m_OnMethod.f_Register(_Actor, fg_Move(_fOnMethod)));

		if (_fOnSubscribe)
			pCombinedReference->m_References.f_Insert(Internal.m_OnSubscribe.f_Register(_Actor, fg_Move(_fOnSubscribe)));

		if (_fOnUnSubscribe)
			pCombinedReference->m_References.f_Insert(Internal.m_OnUnSubscribe.f_Register(_Actor, fg_Move(_fOnUnSubscribe)));

		if (_fOnError)
			pCombinedReference->m_References.f_Insert(Internal.m_OnError.f_Register(_Actor, fg_Move(_fOnError)));

		if (_fOnClose)
		{
			pCombinedReference->m_References.f_Insert(Internal.m_OnClose.f_Register(_Actor, fg_Move(_fOnClose)));
			Internal.m_pNewWebsocketConnection->m_fOnClose =
				[this, ThisWeak = fg_ThisActor(this).f_Weak()](EWebSocketStatus _Reason, NStr::CStr const &_Message, EWebSocketCloseOrigin _Origin)
				{
					auto ThisActor = ThisWeak.f_Lock();
					if (ThisActor)
					{
						ThisActor
							(
								&CActor::f_Dispatch
								, [this, _Reason, _Message, _Origin]
								{
									auto &Internal = *mp_pInternal;
									Internal.m_OnClose(_Reason, _Message, _Origin) > NConcurrency::fg_DiscardResult();
								}
							)
							> NConcurrency::fg_DiscardResult()
						;
					}
				}
			;
		}

		auto pInternal = &Internal;
		Internal.m_pNewWebsocketConnection->m_fOnReceiveTextMessage = [pInternal](NStr::CStr const &_Message)
			{
				pInternal->fp_ReceiveMessage(_Message);
			}
		;

		Internal.m_WebSocket = Internal.m_pNewWebsocketConnection->f_Accept
			(
				"" // check which protocol
				, fg_ThisActor(this) / [this](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Callback)
				{
					if (_Callback)
						mp_pInternal->m_WebSocketCallbacks = fg_Move(*_Callback);
				}
			)
		;
		if (Internal.m_ConnectionType == EConnectionType_SockJSWebsocket)
		{
			Internal.m_WebSocket(&CWebSocketActor::f_SendText, "o", 0) > NConcurrency::fg_DiscardResult(); // Open frame
			NConcurrency::fg_TimerActor()
				(
					&NConcurrency::CTimerActor::f_RegisterTimer
					, 25.0
					, fg_ThisActor(this)
					, [this]() -> NConcurrency::TCContinuation<void>
					{
						auto &Internal = *mp_pInternal;
						Internal.m_WebSocket(&CWebSocketActor::f_SendText, "h", 0) > NConcurrency::fg_DiscardResult(); // Heartbeat frame

						return fg_Explicit();
					}
				)
				> [this](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
				{
					auto &Internal = *mp_pInternal;
					Internal.m_SockJSHeartbeatCallback = fg_Move(*_Result);
				}
			;
		}
		Internal.m_pNewWebsocketConnection.f_Clear();

		return fg_Move(pCombinedReference);
	}

	void CDDPServerConnection::f_SendChanges(NContainer::TCVector<CChange> &&_Changes)
	{
		auto &Internal = *mp_pInternal;
		for (auto &Change : _Changes)
		{
			switch (Change.f_GetTypeID())
			{
			case EChange_Added:
				{
					auto &Added = Change.f_Get<EChange_Added>();

					NEncoding::CEJSON Message;
					Message["msg"] = "added";
					Message["collection"] = fg_Move(Added.m_Collection);
					Message["id"] = fg_Move(Added.m_DocumentID);
					if (Added.m_Fields.f_IsValid())
						Message["fields"] = fg_Move(Added.m_Fields);

					Internal.f_SendMessage(Message);
				}
				break;
			case EChange_Changed:
				{
					auto &Changed = Change.f_Get<EChange_Changed>();

					NEncoding::CEJSON Message;
					Message["msg"] = "changed";
					Message["collection"] = fg_Move(Changed.m_Collection);
					Message["id"] = fg_Move(Changed.m_DocumentID);
					if (Changed.m_Fields.f_IsValid())
						Message["fields"] = fg_Move(Changed.m_Fields);
					if (!Changed.m_Cleared.f_IsEmpty())
					{
						auto &ClearedArray = (Message["cleared"] = NEncoding::EJSONType_Array).f_Array();

						for (auto &ToClear : Changed.m_Cleared)
							ClearedArray.f_Insert() = fg_Move(ToClear);
					}

					Internal.f_SendMessage(Message);
				}
				break;
			case EChange_Removed:
				{
					auto &Removed = Change.f_Get<EChange_Removed>();

					NEncoding::CEJSON Message;
					Message["msg"] = "removed";
					Message["collection"] = fg_Move(Removed.m_Collection);
					Message["id"] = fg_Move(Removed.m_DocumentID);

					Internal.f_SendMessage(Message);
				}
				break;
			case EChange_Ready:
				{
					auto &Ready = Change.f_Get<EChange_Ready>();

					NEncoding::CEJSON Message;
					Message["msg"] = "ready";
					auto &SubsArray = (Message["subs"] = NEncoding::EJSONType_Array).f_Array();
					for (auto &ReadySub : Ready.m_Subscriptions)
						SubsArray.f_Insert() = ReadySub;

					Internal.f_SendMessage(Message);
				}
				break;
			case EChange_Updated:
				{
					auto &Updated = Change.f_Get<EChange_Updated>();

					NEncoding::CEJSON Message;
					Message["msg"] = "updated";
					auto &MethodsArray = (Message["methods"] = NEncoding::EJSONType_Array).f_Array();
					for (auto &MethodID : Updated.m_IDs)
						MethodsArray.f_Insert() = MethodID;

					Internal.f_SendMessage(Message);
				}
				break;
			case EChange_NoSub:
				{
					auto &NoSub = Change.f_Get<EChange_NoSub>();

					NEncoding::CEJSON Message;
					Message["msg"] = "nosub";
					Message["id"] = NoSub.m_SubscriptionID;

					Internal.f_SendMessage(Message);
				}
				break;
			}
		}
	}

	void CDDPServerConnection::fp_AcceptConnection(NStr::CStr const &_SessionID)
	{
		auto &Internal = *mp_pInternal;

		NEncoding::CEJSON Message;
		Message["msg"] = "connected";
		if (!_SessionID.f_IsEmpty())
			Message["session"] = _SessionID;
		else
			Message["session"] = CDDPClient::fs_RandomID();
		Internal.f_SendMessage(Message);
	}

	void CDDPServerConnection::fp_RejectConnection()
	{
		auto &Internal = *mp_pInternal;

		NEncoding::CEJSON Message;
		Message["msg"] = "failed";
		Message["version"] = "1";
		Internal.f_SendMessage(Message);
	}

	void CDDPServerConnection::fp_MethodResult(NStr::CStr const &_MethodID, NEncoding::CEJSON const &_Result, bool _bUpdated)
	{
		auto &Internal = *mp_pInternal;
		{
			NEncoding::CEJSON Message;
			Message["msg"] = "result";
			Message["id"] = _MethodID;
			if (_Result.f_IsValid())
				Message["result"] = _Result;
			Internal.f_SendMessage(Message);
		}
		if (_bUpdated)
		{
			NEncoding::CEJSON Message;
			Message["msg"] = "updated";
			auto &MethodsArray = (Message["methods"] = NEncoding::EJSONType_Array).f_Array();
			MethodsArray.f_Insert() = _MethodID;
			Internal.f_SendMessage(Message);
		}
	}

	void CDDPServerConnection::fp_MethodError(NStr::CStr const &_MethodID, NEncoding::CEJSON const &_Error)
	{
		auto &Internal = *mp_pInternal;
		{
			NEncoding::CEJSON Message;
			Message["msg"] = "result";
			Message["id"] = _MethodID;
			Message["error"] = _Error;
			Internal.f_SendMessage(Message);
		}
		{
			NEncoding::CEJSON Message;
			Message["msg"] = "updated";
			auto &MethodsArray = (Message["methods"] = NEncoding::EJSONType_Array).f_Array();
			MethodsArray.f_Insert() = _MethodID;
			Internal.f_SendMessage(Message);
		}
	}

	void CDDPServerConnection::fp_SubscriptionError(NStr::CStr const &_SubscriptionID, NEncoding::CEJSON const &_Error)
	{
		auto &Internal = *mp_pInternal;

		NEncoding::CEJSON Message;

		Message["msg"] = "nosub";
		Message["id"] = _SubscriptionID;
		if (_Error.f_IsValid())
			Message["error"] = _Error;

		Internal.f_SendMessage(Message);
	}
}
