
#include "Malterlib_Web_DDP_Server.h"
#include "Malterlib_Web_DDP_Client.h"

#include <Mib/Concurrency/ActorCallbackManager>

namespace NMib
{
	namespace NWeb
	{
		struct CDDPServerConnection::CInternal
		{
			CDDPServerConnection *mp_pServerConnection;
			NPtr::TCUniquePointer<CWebSocketNewServerConnection> m_pNewWebsocketConnection;

			NConcurrency::TCActor<CWebSocketActor> m_WebSocket;
			NConcurrency::CActorCallback m_WebSocketCallbacks;
			
			NConcurrency::TCActorCallbackManager<void (CConnectionInfo const &_MethodInfo)> m_OnConnection;
			NConcurrency::TCActorCallbackManager<void (CMethodInfo const &_MethodInfo)> m_OnMethod;
			NConcurrency::TCActorCallbackManager<void (CSubscribeInfo const &_SubscribeInfo)> m_OnSubscribe;
			NConcurrency::TCActorCallbackManager<void (NStr::CStr const &_ID)> m_OnUnSubscribe;
			NConcurrency::TCActorCallbackManager<void (NStr::CStr const &_Error)> m_OnError;
			
			CInternal(CDDPServerConnection *_pDDPServerConnection, CWebSocketNewServerConnection &&_NewWebsocketConnection)
				: mp_pServerConnection(_pDDPServerConnection)
				, m_pNewWebsocketConnection(fg_Construct(fg_Move(_NewWebsocketConnection)))
				, m_WebSocket()
				, m_WebSocketCallbacks()
				, m_OnConnection(mp_pServerConnection, false)
				, m_OnMethod(mp_pServerConnection, false)
				, m_OnSubscribe(mp_pServerConnection, false)
				, m_OnUnSubscribe(mp_pServerConnection, false)
				, m_OnError(mp_pServerConnection, false)
			{
				m_pNewWebsocketConnection->m_fOnReceiveTextMessage = [this](NStr::CStr const &_Message)
					{
						fp_ReceiveMessage(_Message);
					}
				;
			}
			
			void f_SendMessage(NEncoding::CEJSON const &_Data);
			
			void fp_ReceiveMessage(NStr::CStr const &_Message);
			void fp_OnError(NStr::CStr const &_Message);
		};
		
		void CDDPServerConnection::CInternal::f_SendMessage(NEncoding::CEJSON const &_Data)
		{
			m_WebSocket(&CWebSocketActor::f_SendText, _Data.f_ToString(nullptr), 0) > NConcurrency::fg_DiscardResult();
		}
		
		void CDDPServerConnection::CInternal::fp_ReceiveMessage(NStr::CStr const &_Message)
		{
			try
			{
				NEncoding::CEJSON JSON = NEncoding::CEJSON::fs_FromString(_Message);

				auto *pValue = JSON.f_GetMember("msg");
				if (!pValue)
				{
					fp_OnError("No msg in DDP packet");
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
						m_OnConnection(ConnectionInfo);
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

					m_OnMethod(MethodInfo);
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
					
					m_OnSubscribe(SubscribeInfo);
				}
				else if (MessageType == "unsub")
				{
					auto pSubscriptionID = JSON.f_GetMember("id");
					if (!fg_ValidateType(pSubscriptionID, NEncoding::EJSONType_String) || pSubscriptionID->f_String().f_IsEmpty())
					{
						fp_OnError("(unsub) Invalid Subscription ID, either empty or not a String type");
						return;
					}

					m_OnUnSubscribe(pSubscriptionID->f_String());
				}
				else
				{
					fp_OnError(fg_Format("Unknown message: {}\n", MessageType));
				}
			}
			catch (NException::CException const &_Exception)
			{
				fp_OnError(fg_Format("Processing DDP message failed: {}", _Exception.f_GetErrorStr()));
			}
		}

		void CDDPServerConnection::CInternal::fp_OnError(NStr::CStr const &_Message)
		{
			m_OnError(_Message);
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
		
		void CDDPServerConnection::CConnectionInfo::f_Accept(NStr::CStr const &_Session) const
		{
			if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
				Actor(&CDDPServerConnection::fp_AcceptConnection, _Session) > NConcurrency::fg_DiscardResult();
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
		
		CDDPServerConnection::CMethodInfo::~CMethodInfo()
		{
		}
		
		void CDDPServerConnection::CMethodInfo::f_Result(NEncoding::CEJSON const &_Result) const
		{
			if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
				Actor(&CDDPServerConnection::fp_MethodResult, m_ID, _Result) > NConcurrency::fg_DiscardResult();
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
		
		void CDDPServerConnection::CSubscribeInfo::f_Error(NEncoding::CEJSON const &_Error) const
		{
			if (auto Actor = mp_Internal.f_Get().mp_DDPConnection.f_Lock())
				Actor(&CDDPServerConnection::fp_SubscriptionError, m_ID, _Error) > NConcurrency::fg_DiscardResult();
		}
		
		CDDPServerConnection::CDDPServerConnection(CWebSocketNewServerConnection &&_ServerConnection)
			: mp_pInternal(fg_Construct(this, fg_Move(_ServerConnection)))
		{
		}

		CDDPServerConnection::~CDDPServerConnection()
		{
		}
		
		void CDDPServerConnection::f_Construct()
		{
			auto &Internal = *mp_pInternal;
			Internal.m_WebSocket = Internal.m_pNewWebsocketConnection->f_Accept
				(
					"" // check which protocol
					, fg_ThisActor(this) / [this](NConcurrency::TCAsyncResult<NConcurrency::CActorCallback> &&_Callback)
					{
						if (_Callback)
							mp_pInternal->m_WebSocketCallbacks = fg_Move(*_Callback);
					}
				)
			;
			Internal.m_pNewWebsocketConnection.f_Clear();
		}

		NConcurrency::TCContinuation<void> CDDPServerConnection::f_Destroy()
		{
			auto &Internal = *mp_pInternal;

			Internal.m_WebSocketCallbacks.f_Clear();

			NConcurrency::TCContinuation<void> Continuation;

			if (Internal.m_WebSocket)
			{
				Internal.m_WebSocket->f_Destroy
					(
						NConcurrency::fg_ConcurrentActor() / [Continuation](NConcurrency::TCAsyncResult<void> &&_Result)
						{
							Continuation.f_SetResult(fg_Move(_Result));
						}
					)
				;
			}
			else
				Continuation.f_SetResult();

			return Continuation;

		}
		
		NConcurrency::CActorCallback CDDPServerConnection::f_Register
			(
				NConcurrency::TCActor<CActor> const &_Actor
				, NFunction::TCFunction<void (CConnectionInfo const &_MethodInfo)> &&_fOnConnection
				, NFunction::TCFunction<void (CMethodInfo const &_MethodInfo)> &&_fOnMethod
				, NFunction::TCFunction<void (CSubscribeInfo const &_SubscribeInfo)> &&_fOnSubscribe
				, NFunction::TCFunction<void (NStr::CStr const &_ID)> &&_fOnUnSubscribe
				, NFunction::TCFunction<void (NStr::CStr const &_Error)> &&_fOnError
			)
		{
			auto &Internal = *mp_pInternal;

			NPtr::TCUniquePointer<NConcurrency::CCombinedCallbackReference> pCombinedReference = fg_Construct();

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
	
		void CDDPServerConnection::fp_MethodResult(NStr::CStr const &_MethodID, NEncoding::CEJSON const &_Result)
		{
			auto &Internal = *mp_pInternal;

			NEncoding::CEJSON Message;

			Message["msg"] = "result";
			Message["id"] = _MethodID;
			Message["result"] = _Result;
			
			Internal.f_SendMessage(Message);
		}
		
		void CDDPServerConnection::fp_MethodError(NStr::CStr const &_MethodID, NEncoding::CEJSON const &_Error)
		{
			auto &Internal = *mp_pInternal;

			NEncoding::CEJSON Message;

			Message["msg"] = "result";
			Message["id"] = _MethodID;
			Message["error"] = _Error;
			
			Internal.f_SendMessage(Message);
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
}
