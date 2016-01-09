
#include <Mib/Core/Core>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Web/HTTP/Request>
#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Concurrency/ConcurrencyDefines>

#include "Malterlib_Web_DDP_Client.h"

namespace NMib
{
	namespace NWeb
	{

		struct CDDPClient::CCollection
		{
			NContainer::TCMap<NStr::CStr, NEncoding::CEJSON> m_Documents;
			
			static NStr::CStr fs_IDParse(NStr::CStr const &_ID)
			{
				if (_ID.f_StartsWith("-") || _ID.f_StartsWith("~"))
					return _ID.f_Extract(1);

				return _ID;
			}
		};
		
		struct CDDPClient::CInternal
		{
			enum EChangeOperation
			{
				EChangeOperation_Added
				, EChangeOperation_Changed
			};
			
			struct CSubscription
			{
				NStr::CStr const &f_GetID() const
				{
					return NContainer::TCMap<NStr::CStr, CSubscription>::fs_GetKey(*this);
				}
				
				struct COnCallbackRemoved
				{
					COnCallbackRemoved(CInternal *_pInternal, CSubscription *_pSubscription)
						: mp_pInternal(_pInternal)
						, mp_pSubscription(_pSubscription)
					{
					}
					
					~COnCallbackRemoved()
					{
						mp_pInternal->f_OnSubscriptionRemoved(mp_pSubscription);
					}
					
				private:
					CInternal *mp_pInternal;
					CSubscription *mp_pSubscription;
				};
				
				CSubscription(CActor *_pActor)
					: m_Callback(_pActor, false)
				{
				}
				
				NConcurrency::TCActorCallbackManager<void (ESubscriptionNotification _Notification, NEncoding::CEJSON const &_Message), false, COnCallbackRemoved> m_Callback;
				ESubscriptionNotification m_NotifyOn;
				NFunction::TCFunction<void ()> m_OnReady;
				NFunction::TCFunction<void (NStr::CStr const &_Error)> m_OnError;
				bool m_bWasError = false;
			};

			struct CCollectionObservations;
			struct CObservation
			{
				
				struct COnCallbackRemoved
				{
					COnCallbackRemoved(CInternal *_pInternal, CObservation *_pObservation, CCollectionObservations *_pColletionObservations)
						: mp_pInternal(_pInternal)
						, mp_pObservation(_pObservation)
						, mp_pColletionObservations(_pColletionObservations)
					{
					}
					
					~COnCallbackRemoved()
					{
						mp_pInternal->f_OnObservationRemoved(mp_pObservation, mp_pColletionObservations);
					}
					
				private:
					CInternal *mp_pInternal;
					CObservation *mp_pObservation;
					CCollectionObservations *mp_pColletionObservations;
				};
				
				CObservation(CActor *_pActor)
					: m_Callback(_pActor, false)
				{
				}
				
				NConcurrency::TCActorCallbackManager<void (EObserveNotification _Notification, NEncoding::CEJSON const &_Message), false, COnCallbackRemoved> m_Callback;
				EObserveNotification m_NotifyOn;
			};
			
			struct CCollectionObservations
			{
				NContainer::TCLinkedList<CObservation> m_Observations;
				NStr::CStr const &f_GetID() const
				{
					return NContainer::TCMap<NStr::CStr, CCollectionObservations>::fs_GetKey(*this);
				}
			};
			
			CInternal
				(
					CDDPClient *_pThis
					, NHTTP::CURL const &_ConnectTo
					, NStr::CStr const &_BindTo
					, NConcurrency::TCActor<CWebSocketClientActor> const &_ConnectionFactory
					, NStr::CStr const &_Origin
					, NNet::FVirtualSocketFactory const &_SocketFactory
					, EClientOption _ClientOptions
				)
				: m_pThis(_pThis)
				, m_ConnectTo(_ConnectTo)
				, m_BindTo(_BindTo)
				, m_ConnectionFactory(_ConnectionFactory)
				, m_SocketFactory(_SocketFactory)
				, m_ClientOptions(_ClientOptions)
			{
				if (!_ConnectionFactory)
					m_ConnectionFactory = NConcurrency::fg_ConstructActor<CWebSocketClientActor>();

				if (!_SocketFactory && _ConnectTo.f_GetScheme() == "wss")
				{
					NNet::CSSLSettings ClientSettings;
					ClientSettings.m_VerificationFlags |= NNet::CSSLSettings::EVerificationFlag_UseOSStoreIfNoCASpecified;
					NPtr::TCSharedPointer<NNet::CSSLContext> pClientContext = fg_Construct(NNet::CSSLContext::EType_Client, ClientSettings);
					m_SocketFactory = NNet::CSocket_SSL::fs_GetFactory(pClientContext);
				}

				if (_Origin.f_IsEmpty())
				{
					auto Origin = _ConnectTo;
					if (_ConnectTo.f_GetScheme() == "wss")
						Origin.f_SetScheme("https");
					else
						Origin.f_SetScheme("http");

					Origin.f_ClearFragment();
					m_Origin = Origin.f_Encode();
				}
			}
			
			~CInternal()
			{
				m_bDestructorCalled = true;
				m_Subscriptions.f_Clear();
			}

			CDDPClient *m_pThis;

			NHTTP::CURL m_ConnectTo;
			NStr::CStr m_BindTo;
			NStr::CStr m_Origin;
			NConcurrency::TCActor<CWebSocketActor> m_WebSocket;
			NConcurrency::CActorCallback m_WebSocketCallbackSubscription;
			NConcurrency::TCActor<CWebSocketClientActor> m_ConnectionFactory;
			NNet::FVirtualSocketFactory m_SocketFactory;
			NConcurrency::TCContinuation<CConnectInfo> m_ConnectContinuation;

			NStr::CStr m_UserName;
			NStr::CStrSecure m_Password;
			NStr::CStrSecure m_LoginToken;

			NStr::CStr m_SessionID;

			uint64 m_LastMethodID = 0;

			NContainer::TCMap<uint64, NFunction::TCFunction<void (NStr::CStr const &_Error, NEncoding::CEJSON &&_Result)>> m_PendingMethodCalls;
			NContainer::TCMap<NStr::CStr, CCollection> m_Collections;

			NContainer::TCMap<uint64, NFunction::TCFunction<void (NFunction::CThisTag &)>> m_PendingMethodUpdated;

			NContainer::TCMap<NStr::CStr, CSubscription> m_Subscriptions;
			
			NContainer::TCMap<NStr::CStr, CCollectionObservations> m_Observations;

			NConcurrency::CActorCallback m_ConnectTimeoutTimerRef;
			
			fp32 m_ConnectTimeout = 60.0;

			EClientOption m_ClientOptions;

			bool m_bConnectFinished = false;
			bool m_bConnectCalled = false;

			bool m_bDestroyed = false;
			bool m_bDestructorCalled = false;

			void fp_OnError(NStr::CStr const &_Error);
			void fp_ReceiveMessage(NStr::CStr const &_Message);
			void fp_HandleRemoved(NEncoding::CEJSON const &_Message);
			void fp_HandleAddedChanged(NEncoding::CEJSON const &_Message, EChangeOperation _Operation);
			void fp_HandleReady(NEncoding::CEJSON const &_Message);
			void fp_HandleUpdated(NEncoding::CEJSON const &_Message);
			void fp_HandleNoSub(NEncoding::CEJSON const &_Message);
			void fp_NotifyObserve(NStr::CStr const &_Collection, NEncoding::CEJSON const &_Message, EObserveNotification _Notification);
			uint64 fp_SendMethod
				(
					NStr::CStr const &_MethodName
					, NContainer::TCVector<NEncoding::CEJSON> const &_Params
					, NFunction::TCFunction<void (NStr::CStr const &_Error, NEncoding::CEJSON &&_Result)> &&_fOnResult
				)
			;
			void fp_SendLoginResumeMessage();
			void fp_SendLoginMessage();
			void fp_SendConnect(fp32 _Timeout, NStr::CStr const &_SessionID);
			void f_OnSubscriptionRemoved(CSubscription *_pSubscription);
			void f_OnObservationRemoved(CObservation *_pObservation, CCollectionObservations *_pColletionObservations);
			void fp_SendMessage(NEncoding::CEJSON const &_Message);

			CConnectInfo fp_ExtractConnectInfo(NEncoding::CEJSON const &_Info);
		};

		
		NContainer::TCMap<NStr::CStr, NEncoding::CEJSON>::CIteratorConst CDDPClient::CCollectionAccessor::f_GetDocumentIterator() const
		{
			return mp_pCollection->m_Documents.f_GetIterator();
		}
		
		NEncoding::CEJSON const &CDDPClient::CCollectionAccessor::f_GetDocument(NStr::CStr const &_Id) const
		{
			auto pDocument = mp_pCollection->m_Documents.f_FindEqual(_Id);
			if (!pDocument)
				DMibError(fg_Format("No document with ID '{}' within Collection '{}' exists!", _Id, NContainer::TCMap<NStr::CStr, CCollection>::fs_GetKey(mp_pCollection)));

			return *pDocument;
		}
		
		NContainer::TCMap<NStr::CStr, NEncoding::CEJSON>::CIteratorConst CDDPClient::CCollectionAccessor::f_GetRandomDocumentIterator() const
		{
			auto iDoc = mp_pCollection->m_Documents.f_GetIterator();
			mint nDocuments = mp_pCollection->m_Documents.f_GetLen();
			if (nDocuments == 1)
				return iDoc;
				
			mint iIndex = NMisc::fg_Random().f_GetValue(nDocuments);
			for (mint i = 0; i < iIndex; ++i)
				++iDoc;
			
			return iDoc;
		}
		
		NStr::CStr const &CDDPClient::CCollectionAccessor::f_GetRandomDocumentID() const
		{
			return f_GetRandomDocumentIterator().f_GetKey();
		}

		CDDPClient::CCollectionAccessor::CCollectionAccessor(CCollection const *_pCollection)
			: mp_pCollection(_pCollection)
		{
		}
		
		CDDPClient::CCollectionAccessor CDDPClient::CDataAccessor::f_GetCollection(NStr::CStr const &_Name) const
		{
			auto pCollection = mp_pInternal->m_Collections.f_FindEqual(_Name);
			if (!pCollection)
				DMibError(fg_Format("Collection '{}' does not exist!", _Name));

			return CDDPClient::CCollectionAccessor(pCollection);
		}
		
		NContainer::TCVector<NStr::CStr> CDDPClient::CDataAccessor::f_GetCollectionNames() const
		{
			auto &Collections = mp_pInternal->m_Collections;

			NContainer::TCVector<NStr::CStr> Result;
			Result.f_SetLen(Collections.f_GetLen());

			auto iName = 0u;
			for (auto iCollection = Collections.f_GetIterator(); iCollection; ++iCollection, ++iName)
				Result[iName] = iCollection.f_GetKey();

			return Result;
		}

		bool CDDPClient::CDataAccessor::f_CollectionExists(NStr::CStr const &_Name) const
		{
			auto pCollection = mp_pInternal->m_Collections.f_FindEqual(_Name);
			return pCollection != nullptr;
		}
		
		CDDPClient::CDataAccessor::CDataAccessor(CInternal *_pInternal)
			: mp_pInternal(_pInternal)
		{
		}
		
		CDDPClient::CDDPClient
			(
				NHTTP::CURL const &_ConnectTo
				, NStr::CStr const &_BindTo
				, NConcurrency::TCActor<CWebSocketClientActor> const &_ConnectionFactory
				, NStr::CStr const &_Origin
				, NNet::FVirtualSocketFactory const &_SocketFactory
				, EClientOption _ClientOptions
			)
			: mp_pInternal(fg_Construct(this, _ConnectTo, _BindTo, _ConnectionFactory, _Origin, _SocketFactory, _ClientOptions))
		{
		}

		CDDPClient::~CDDPClient()
		{
		}

		NConcurrency::TCContinuation<CDDPClient::CConnectInfo> CDDPClient::f_Connect
			(
				NStr::CStr const &_UserName
				, NStr::CStrSecure const &_Password
				, NStr::CStrSecure const &_Token
				, NStr::CStr const &_SessionID
				, fp32 _Timeout
				, NConcurrency::TCActor<NConcurrency::CActor> &&_NotificationActor
				, NFunction::TCFunction<void (EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> &&_fOnClose
			)
		{
			DMibRequire(!_fOnClose || _NotificationActor);
			
			auto &Internal = *mp_pInternal;

			NConcurrency::TCContinuation<CConnectInfo> Continuation;

			if (Internal.m_bConnectCalled)
			{
				Continuation.f_SetException(DMibErrorInstance("The DDP client can only be connected once"));
				return Continuation;
			}

			Internal.m_bConnectCalled = true;
			Internal.m_UserName = _UserName;
			Internal.m_Password = _Password;
			Internal.m_LoginToken = _Token;
			
			NConcurrency::TCWeakActor<NConcurrency::CActor> WeakNotificationActor;
			if (_NotificationActor)
				WeakNotificationActor = _NotificationActor.f_Weak();

			Internal.m_ConnectContinuation = Continuation;
			Internal.m_ConnectionFactory
				(
					&CWebSocketClientActor::f_Connect
					, Internal.m_ConnectTo.f_GetHost()
					, Internal.m_BindTo
					, Internal.m_ConnectTo.f_GetPortFromScheme()
					, Internal.m_ConnectTo.f_GetFullPath()
					, Internal.m_Origin
					, NContainer::fg_CreateVector<NStr::CStr>()
					, NHTTP::CRequest()
					, fg_TempCopy(Internal.m_SocketFactory)
				)
				> [this, _SessionID, _Timeout, fOnClose = fg_Move(_fOnClose), WeakNotificationActor = fg_Move(WeakNotificationActor)](NConcurrency::TCAsyncResult<CWebSocketNewClientConnection> &&_Result)
				{
					auto &Internal = *mp_pInternal;
					if (!_Result)
					{
						Internal.m_ConnectContinuation.f_SetException(_Result);
						return;
					}

					CWebSocketNewClientConnection& Result = _Result.f_Get();

					Result.m_fOnReceiveTextMessage
						= [this](NStr::CStr const &_Message)
						{
							mp_pInternal->fp_ReceiveMessage(_Message);
						}
					;
					
					if (fOnClose && WeakNotificationActor)
					{
						Result.m_fOnClose =
							[
								WeakNotificationActor = fg_Move(WeakNotificationActor)
								, fOnClose = fg_Move(fOnClose)
							]
							(EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)
							{
								auto NotificationActor = WeakNotificationActor.f_Lock();
								
								if (NotificationActor)
								{
									NotificationActor
										(
											&CActor::f_Dispatch
											, [fOnClose, _Reason, _Message, _Origin]
											{
												fOnClose(_Reason, _Message, _Origin);
											}
										)
										> NConcurrency::fg_DiscardResult()
									;
								}
							}
						;
					}

					Internal.m_WebSocket = Result.f_Accept
						(
							fg_ThisActor(this) / [this](NConcurrency::TCAsyncResult<NConcurrency::CActorCallback> &&_Result)
							{
								auto &Internal = *mp_pInternal;
								if (_Result)
									mp_pInternal->m_WebSocketCallbackSubscription = fg_Move(_Result.f_Get());
								else
								{
									if (!Internal.m_ConnectContinuation.f_IsSet())
										Internal.m_ConnectContinuation.f_SetException(fg_Move(_Result));
								}
							}
						)
					;

					Internal.fp_SendConnect(_Timeout, _SessionID);
				}
			;

			return Internal.m_ConnectContinuation;
		}

		NConcurrency::TCContinuation<void> CDDPClient::f_Destroy()
		{
			auto &Internal = *mp_pInternal;

			Internal.m_bDestroyed = true;
			Internal.m_ConnectTimeoutTimerRef.f_Clear();

			if (!Internal.m_ConnectContinuation.f_IsSet())
				Internal.m_ConnectContinuation.f_SetException(DMibErrorInstance("Destroy called before connection finished"));
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

		NConcurrency::TCContinuation<NEncoding::CEJSON> CDDPClient::f_Method(NStr::CStr const &_MethodName, NContainer::TCVector<NEncoding::CEJSON> const &_Params)
		{
			auto &Internal = *mp_pInternal;
			NConcurrency::TCContinuation<NEncoding::CEJSON> Continuation;
			Internal.fp_SendMethod
				(
					_MethodName
					, _Params
					, [Continuation](NStr::CStr const &_Error, NEncoding::CEJSON &&_Result)
					{
						if (!_Error.f_IsEmpty())
							Continuation.f_SetException(DMibErrorInstance(_Error));
						else
							Continuation.f_SetResult(fg_Move(_Result));
					}
				)
			;
			return Continuation;
		}
		
		NConcurrency::TCContinuation<NEncoding::CEJSON> CDDPClient::f_MethodWithUpdated
			(
				NStr::CStr const &_MethodName
				, NContainer::TCVector<NEncoding::CEJSON> const &_Params
				, NConcurrency::TCActor<NConcurrency::CActor> const &_OnUpdatedActor
				, NFunction::TCFunction<void ()> &&_fOnUpdated
			)
		{
			auto &Internal = *mp_pInternal;
			NConcurrency::TCContinuation<NEncoding::CEJSON> Continuation;
			uint64 MethodID = Internal.fp_SendMethod
				(
					_MethodName
					, _Params
					, [Continuation](NStr::CStr const &_Error, NEncoding::CEJSON &&_Result)
					{
						if (!_Error.f_IsEmpty())
							Continuation.f_SetException(DMibErrorInstance(_Error));
						else
							Continuation.f_SetResult(fg_Move(_Result));
					}
				)
			;
			
			Internal.m_PendingMethodUpdated[MethodID] = [fOnUpdated = fg_Move(_fOnUpdated), WeakActor = _OnUpdatedActor.f_Weak()]() mutable
				{
					auto Actor = WeakActor.f_Lock();
					if (Actor)
						Actor(&NConcurrency::CActor::f_Dispatch, fg_Move(fOnUpdated)) > NConcurrency::fg_DiscardResult();
				}
			;
			
			return Continuation;
			
		}
		
		NConcurrency::CActorCallback CDDPClient::f_Observe
			(
				NConcurrency::TCActor<CActor> const &_Actor
				, NStr::CStr const &_CollectionName // Leave empty to observe all collections
				, EObserveNotification _NotifyOn
				, NFunction::TCFunction<void (EObserveNotification _Notification, NEncoding::CEJSON const &_Message)> &&_Callback
			)
		{
			auto &Internal = *mp_pInternal;
			auto &CollectionObservations = Internal.m_Observations[_CollectionName];
			auto &Observation = CollectionObservations.m_Observations.f_Insert(fg_Construct(this));
			
			if (_Callback)
				Observation.m_NotifyOn = _NotifyOn;
			else
				Observation.m_NotifyOn = EObserveNotification_None;
			
			auto CallbackHandle = Observation.m_Callback.f_Register(_Actor, fg_Move(_Callback), &Internal, &Observation, &CollectionObservations);
			
			return CallbackHandle;
		}
	
		NConcurrency::TCContinuation<NConcurrency::CActorCallback> CDDPClient::f_Subscribe
			(
				NConcurrency::TCActor<CActor> const &_Actor
				, NStr::CStr const &_SubscriptionName
				, NStr::CStr const &_SubscriptionID
				, NEncoding::CEJSON const &_Params
				, ESubscriptionNotification _NotifyOn
				, NFunction::TCFunction<void (ESubscriptionNotification _Notification, NEncoding::CEJSON const &_Message)> &&_Callback
				, bool _bWaitForResponse
			)
		{
			NStr::CStr SubscriptionID = _SubscriptionID;
			
			if (SubscriptionID.f_IsEmpty())
				SubscriptionID = fs_RandomID();
			auto &Internal = *mp_pInternal;
			auto &Subscription = *Internal.m_Subscriptions(SubscriptionID, this);
			
			if (_Callback)
				Subscription.m_NotifyOn = _NotifyOn;
			else
				Subscription.m_NotifyOn = ESubscriptionNotification_None;
			
			NEncoding::CEJSON Message;

			Message["msg"] = "sub";
			Message["id"] = SubscriptionID;
			Message["name"] = _SubscriptionName;
			if (_Params.f_IsValid())
				Message["params"] = _Params;

			Internal.fp_SendMessage(Message);
			
			auto CallbackHandle = Subscription.m_Callback.f_Register(_Actor, fg_Move(_Callback), &Internal, &Subscription);
			NConcurrency::TCContinuation<NConcurrency::CActorCallback> Continuation;
			if (!_bWaitForResponse)
			{
				Continuation.f_SetResult(fg_Move(CallbackHandle));
				return Continuation;
			}
			
			auto CallbackHandleMove = fg_LambdaMove(CallbackHandle);
			Subscription.m_OnReady = [Continuation, CallbackHandleMove]
				{
					Continuation.f_SetResult(fg_Move(*CallbackHandleMove));
				}
			;
			Subscription.m_OnError = [Continuation](NStr::CStr const &_Error)
				{
					Continuation.f_SetException(DMibErrorInstance(_Error));
				}
			;

			return Continuation;
		}
		
		void CDDPClient::f_AccessData(NFunction::TCFunction<void (CDataAccessor const &_Accessor)> const &_ProcessData)
		{
			_ProcessData(CDataAccessor(mp_pInternal.f_Get()));
		}

		void CDDPClient::CInternal::fp_OnError(NStr::CStr const &_Error)
		{
			if (!m_ConnectContinuation.f_IsSet())
				m_ConnectContinuation.f_SetException(DMibErrorInstance(_Error));
		}

		void CDDPClient::CInternal::fp_SendMessage(NEncoding::CEJSON const &_Message)
		{
			m_WebSocket(&CWebSocketActor::f_SendText, _Message.f_ToString(nullptr), 0) > NConcurrency::fg_DiscardResult();
		}
		
		void CDDPClient::CInternal::fp_NotifyObserve(NStr::CStr const &_Collection, NEncoding::CEJSON const &_Message, EObserveNotification _Notification)
		{
			auto fNotify = [&](CCollectionObservations &_CollectionObservations)
				{
					for (auto &Observation : _CollectionObservations.m_Observations)
					{
						if (Observation.m_NotifyOn & _Notification)
							Observation.m_Callback(_Notification, _Message);
					}
				}
			;

			auto *pCollectionObservations = m_Observations.f_FindEqual(NStr::CStr());
			if (pCollectionObservations)
				fNotify(*pCollectionObservations);
			if (!_Collection.f_IsEmpty())
			{
				pCollectionObservations = m_Observations.f_FindEqual(_Collection);
				if (pCollectionObservations)
					fNotify(*pCollectionObservations);
			}
		}
		
		void CDDPClient::CInternal::fp_HandleRemoved(NEncoding::CEJSON const &_Message)
		{
			auto pCollection = _Message.f_GetMember("collection");
			auto pID = _Message.f_GetMember("id");
			
			if (!pCollection || pCollection->f_Type() != NEncoding::EJSONType_String)
				return;
			if (!pID || pID->f_Type() != NEncoding::EJSONType_String)
				return;

			if (m_ClientOptions & EClientOption_MaintainDatabase)
			{
				auto CollectionKey = pCollection->f_String();
				auto& Collection = m_Collections[CollectionKey];
				Collection.m_Documents.f_Remove(CCollection::fs_IDParse(pID->f_String()));
				if (Collection.m_Documents.f_IsEmpty())
					m_Collections.f_Remove(CollectionKey);
			}
			
			fp_NotifyObserve(pCollection->f_String(), _Message, EObserveNotification_Removed);
		}

		void CDDPClient::CInternal::fp_HandleAddedChanged(NEncoding::CEJSON const &_Message, EChangeOperation _Operation)
		{
			auto pCollection = _Message.f_GetMember("collection");
			auto pID = _Message.f_GetMember("id");
			auto pFields = _Message.f_GetMember("fields");
			
			if (!pCollection || pCollection->f_Type() != NEncoding::EJSONType_String)
				return;
			if (!pID || pID->f_Type() != NEncoding::EJSONType_String)
				return;
			if (!pFields || pFields->f_Type() != NEncoding::EJSONType_Object)
				return;
			
			auto const ID = CCollection::fs_IDParse(pID->f_String());
			if (ID.f_IsEmpty())
				return;

			if (m_ClientOptions & EClientOption_MaintainDatabase)
			{
				auto &Collection = m_Collections[pCollection->f_String()];
				auto &Object = Collection.m_Documents[ID];
				
				if (_Operation == EChangeOperation_Changed)
				{
					for (auto iField = pFields->f_Object().f_OrderedIterator(); iField; ++iField)
						Object[iField->f_Name()] = iField->f_Value();
					
					{
						auto pCleared = _Message.f_GetMember("cleared");
						if (pCleared && pCleared->f_Type() == NEncoding::EJSONType_Array)
						{
							for (auto iField = pCleared->f_Array().f_GetIterator(); iField; ++iField)
								Object.f_Object().f_RemoveMember(iField->f_AsString(""));
						}
					}
				}
				else if (_Operation == EChangeOperation_Added)
				{
					Object = pFields->f_Object();
				}
			}
			if (_Operation == EChangeOperation_Changed)
				fp_NotifyObserve(pCollection->f_String(), _Message, EObserveNotification_Changed);
			else if (_Operation == EChangeOperation_Added)
				fp_NotifyObserve(pCollection->f_String(), _Message, EObserveNotification_Added);				
		}
		
		void CDDPClient::CInternal::fp_HandleReady(NEncoding::CEJSON const &_Message)
		{
			auto pSubscriptions = _Message.f_GetMember("subs");
			if (!pSubscriptions || pSubscriptions->f_Type() != NEncoding::EJSONType_Array)
				return;
			
			for (auto iSubId = pSubscriptions->f_Array().f_GetIterator(); iSubId; ++iSubId)
			{
				auto pSub = m_Subscriptions.f_FindEqual(iSubId->f_AsString(""));
				if (!pSub)
					continue;

				if (pSub->m_OnReady)
				{
					pSub->m_OnReady();
					pSub->m_OnReady.f_Clear();
					pSub->m_OnError.f_Clear();
				}

				if (pSub->m_NotifyOn & ESubscriptionNotification_Ready)
					pSub->m_Callback(ESubscriptionNotification_Ready, NEncoding::CEJSON());
			}
		}
		
		void CDDPClient::CInternal::fp_HandleUpdated(NEncoding::CEJSON const &_Message)
		{
			auto pMethods = _Message.f_GetMember("methods", NEncoding::EJSONType_Array);
			if (!pMethods)
				return;
			
			for (auto iMethod = pMethods->f_Array().f_GetIterator(); iMethod; ++iMethod)
			{
				uint64 MethodID = iMethod->f_AsString("").f_ToInt(uint64(0));
				auto pMethodUpdated = m_PendingMethodUpdated.f_FindEqual(MethodID);
				if (!pMethodUpdated)
					continue;
			
				auto Cleanup = g_OnScopeExit > [&]()
					{
						m_PendingMethodUpdated.f_Remove(MethodID);
					}
				;
				
				(*pMethodUpdated)();
			}
		}
		
		namespace
		{
			NStr::CStr fg_DecodeError(NEncoding::CEJSON const &_Error)
			{
				NStr::CStr ErrorMessage;
				if (auto pMessage = _Error.f_GetMember("error"))
					ErrorMessage += fg_Format("{}: ", pMessage->f_AsString());
				else
					ErrorMessage += "unknown: ";
				
				if (auto pMessage = _Error.f_GetMember("reason"))
					ErrorMessage += pMessage->f_AsString();
				
				if (auto pMessage = _Error.f_GetMember("message"))
				{
					ErrorMessage += "(";
					ErrorMessage += pMessage->f_AsString();
					ErrorMessage += ")";
				}
				
				return ErrorMessage;
			}
		}

		void CDDPClient::CInternal::fp_HandleNoSub(NEncoding::CEJSON const &_Message)
		{
			auto pID = _Message.f_GetMember("id");
			if (!pID || pID->f_Type() != NEncoding::EJSONType_String)
				return;
			
			auto pError = _Message.f_GetMember("error");
			
			if (pError)
			{
				if (auto pSub = m_Subscriptions.f_FindEqual(CCollection::fs_IDParse(pID->f_String())))
				{
					pSub->m_bWasError = true;
					if (pSub->m_OnError)
					{
						NStr::CStr ErrorMessage;
						ErrorMessage = fg_DecodeError(*pError);
						
						pSub->m_OnError(ErrorMessage);

						pSub->m_OnError.f_Clear();
						pSub->m_OnReady.f_Clear();
					}

					if (pSub->m_NotifyOn & ESubscriptionNotification_Error)
						pSub->m_Callback(ESubscriptionNotification_Error, _Message);
				}
			}

			if (!pError)
				fp_NotifyObserve(NStr::CStr(), _Message, EObserveNotification_NoSub);
		}
		
		void CDDPClient::CInternal::fp_ReceiveMessage(NStr::CStr const &_Message)
		{
			try
			{
				NEncoding::CEJSON JSON = NEncoding::CEJSON::fs_FromString(_Message);

				auto *pValue = JSON.f_GetMember("msg");
				if (!pValue)
				{
					if (JSON.f_GetMember("server_id"))
						return;
					
					fp_OnError(NStr::fg_Format("No msg in DDP packet: {}", JSON));
					return;
				}
				
				if (pValue->f_Type() != NEncoding::EJSONType_String)
				{
					fp_OnError("Invalid type for msg (should be String)");
					return;
				}
				
				auto &MessageType = pValue->f_String();
				if (MessageType == "result")
				{
					auto pID = JSON.f_GetMember("id");
					if (!pID)
						return;

					uint64 MessageID = pID->f_AsString().f_ToInt(uint64(0));
					if (!MessageID)
						return;

					auto pPending = m_PendingMethodCalls.f_FindEqual(MessageID);
					if (!pPending)
						return;

					auto Cleanup = g_OnScopeExit > [&]()
						{
							m_PendingMethodCalls.f_Remove(pPending);
						}
					;
					NStr::CStr ErrorMessage;
					NEncoding::CEJSON Result;

					auto pError = JSON.f_GetMember("error");
					if (pError && pError->f_IsObject())
						ErrorMessage = fg_DecodeError(*pError);

					if (auto pResult = JSON.f_GetMember("result"))
						Result = fg_Move(*pResult);

					(*pPending)(ErrorMessage, fg_Move(Result));
				}
				else if (MessageType == "added")
				{
					fp_HandleAddedChanged(JSON, EChangeOperation_Added);
				}
				else if (MessageType == "updated")
				{
					fp_HandleUpdated(JSON);
				}
				else if (MessageType == "changed")
				{
					fp_HandleAddedChanged(JSON, EChangeOperation_Changed);
				}
				else if (MessageType == "removed")
				{
					fp_HandleRemoved(JSON);
				}
				else if (MessageType == "ready")
				{
					fp_HandleReady(JSON);
				}
				else if (MessageType == "nosub")
				{
					fp_HandleNoSub(JSON);
				}
				else if (MessageType == "ping")
				{
					NEncoding::CEJSON Reply;
					Reply["msg"] = "pong";

					if (auto pID = JSON.f_GetMember("id"))
						Reply["id"] = *pID;

					fp_SendMessage(Reply);
				}
				else if (MessageType == "connected")
				{
					m_bConnectFinished = true;
					m_ConnectTimeoutTimerRef.f_Clear();

					if (auto pSessionID = JSON.f_GetMember("session"))
						m_SessionID = pSessionID->f_String();
					else
						m_SessionID = NStr::CStr();

					if (m_UserName.f_IsEmpty())
					{
						if (!m_ConnectContinuation.f_IsSet())
							m_ConnectContinuation.f_SetResult(m_SessionID);
					}
					else
					{
						if (!m_LoginToken.f_IsEmpty())
							fp_SendLoginResumeMessage();
						else
							fp_SendLoginMessage();
					}
				}
				else if (MessageType == "error")
				{
					m_bConnectFinished = true;
					m_ConnectTimeoutTimerRef.f_Clear();

					NStr::CStr Error;
					if (auto pError = JSON.f_GetMember("reason"))
						Error = pError->f_AsString();
					fp_OnError(Error);
				}
				else if (MessageType == "failed")
				{
					m_bConnectFinished = true;
					m_ConnectTimeoutTimerRef.f_Clear();

					NStr::CStr SuggestedVersion = "Unknown";
					if (auto pVersion = JSON.f_GetMember("version"))
						SuggestedVersion = pVersion->f_AsString();
					fp_OnError(fg_Format("Unsupported version for DDP connection. Suggested version: {}", SuggestedVersion));
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

		uint64 CDDPClient::CInternal::fp_SendMethod
			(
				NStr::CStr const &_MethodName
				, NContainer::TCVector<NEncoding::CEJSON> const &_Params
				, NFunction::TCFunction<void (NStr::CStr const &_Error, NEncoding::CEJSON &&_Result)> &&_fOnResult
			)
		{
			uint64 MethodID = ++m_LastMethodID;

			NEncoding::CEJSON Message;

			Message["msg"] = "method";
			Message["method"] = _MethodName;
			Message["params"] = _Params;
			Message["id"] = NStr::CStr::fs_ToStr(MethodID);

			fp_SendMessage(Message);;

			m_PendingMethodCalls[MethodID] = _fOnResult;
			
			return MethodID;
		}

		auto CDDPClient::CInternal::fp_ExtractConnectInfo(NEncoding::CEJSON const &_Info) -> CConnectInfo
		{
			CConnectInfo Info(m_SessionID);

			if (auto pID = _Info.f_GetMember("id"))
				Info.m_UserID = pID->f_AsString();

			if (auto pToken = _Info.f_GetMember("token"))
				Info.m_Token = pToken->f_AsString();

			if (auto pExpires = _Info.f_GetMember("tokenExpires"))
			{
				if (pExpires->f_IsDate())
					Info.m_TokenExpires = pExpires->f_Date();
			}

			return Info;
		}

		void CDDPClient::CInternal::fp_SendLoginResumeMessage()
		{
			NEncoding::CEJSON LoginParams;
			LoginParams["resume"] = m_LoginToken;

			fp_SendMethod
				(
					"login"
					, NContainer::fg_CreateVector<NEncoding::CEJSON>(LoginParams)
					, [this](NStr::CStr const &_Error, NEncoding::CEJSON &&_Result)
					{
						if (!_Error.f_IsEmpty())
							fp_SendLoginMessage();
						else if (!m_ConnectContinuation.f_IsSet())
							m_ConnectContinuation.f_SetResult(fp_ExtractConnectInfo(_Result));
					}
				)
			;
		}

		void CDDPClient::CInternal::fp_SendLoginMessage()
		{
			NEncoding::CEJSON LoginParams;
			LoginParams["user"]["email"] = m_UserName;
			auto &Password = LoginParams["password"];
			Password["digest"] = NDataProcessing::CHash_SHA256::fs_DigestFromData((uint8 const *)m_Password.f_GetStr(), m_Password.f_GetLen()).f_GetString();
			Password["algorithm"] = "sha-256";

			fp_SendMethod
				(
					"login"
					, NContainer::fg_CreateVector<NEncoding::CEJSON>(LoginParams)
					, [this](NStr::CStr const &_Error, NEncoding::CEJSON &&_Result)
					{
						if (!m_ConnectContinuation.f_IsSet())
						{
							if (!_Error.f_IsEmpty())
								m_ConnectContinuation.f_SetException(DMibErrorInstance(_Error));
							else
								m_ConnectContinuation.f_SetResult(fp_ExtractConnectInfo(_Result));
						}
					}
				)
			;
		}

		void CDDPClient::CInternal::fp_SendConnect(fp32 _Timeout, NStr::CStr const &_SessionID)
		{
			NEncoding::CEJSON Message;

			Message["msg"] = "connect";
			Message["version"] = "1";
			if (!_SessionID.f_IsEmpty())
				Message["session"] = _SessionID;

			auto &SupportArray = Message["support"];
			SupportArray.f_Insert("1");

			m_ConnectTimeout = _Timeout;
			m_ConnectTimeoutTimerRef.f_Clear();

			NConcurrency::fg_TimerActor()
				(
					&NConcurrency::CTimerActor::f_OneshotTimerAbortable
					, _Timeout
					, fg_ThisActor(m_pThis)
					, [this]()
					{
						if (!m_bConnectFinished)
						{
							if (!m_ConnectContinuation.f_IsSet())
								m_ConnectContinuation.f_SetException(DMibErrorInstance("Timed out waiting for reply to connect message"));
						}
					}
				)
				> fg_ThisActor(m_pThis) / [this](NConcurrency::TCAsyncResult<NConcurrency::CActorCallback> &&_TimerReference)
				{
					if (!m_bConnectFinished && !m_bDestroyed)
						m_ConnectTimeoutTimerRef = fg_Move(*_TimerReference);
				}
			;

			fp_SendMessage(Message);
		}
		
		void CDDPClient::CInternal::f_OnSubscriptionRemoved(CSubscription *_pSubscription)
		{
			if (m_bDestructorCalled)
				return;
			
			if (!_pSubscription->m_bWasError)
			{
				NEncoding::CEJSON Message;

				Message["msg"] = "unsub";
				Message["id"] = _pSubscription->f_GetID();

				fp_SendMessage(Message);
			}

			m_Subscriptions.f_Remove(_pSubscription);
		}
		
		void CDDPClient::CInternal::f_OnObservationRemoved(CObservation *_pObservation, CCollectionObservations *_pColletionObservations)
		{
			if (m_bDestructorCalled)
				return;
			
			_pColletionObservations->m_Observations.f_Remove(*_pObservation);
			if (_pColletionObservations->m_Observations.f_IsEmpty())
				m_Observations.f_Remove(_pColletionObservations);
		}		

		namespace
		{
			ch8 g_UnmistakableChars[] = "23456789ABCDEFGHJKLMNPQRSTWXYZabcdefghijkmnopqrstuvwxyz";
		}

		NStr::CStr CDDPClient::fs_RandomID()
		{
			const mint c_nCharsInRandomID = 17;
			const mint c_nChars = sizeof(g_UnmistakableChars) / sizeof(g_UnmistakableChars[0]) - 1;
			const mint c_BufferSize = ((c_nCharsInRandomID + 3) / 4) * 4;
			uint8 RandomData[c_BufferSize];

			for (mint i = 0; i < sizeof(RandomData) / 4; ++i)
				*((uint32 *)(RandomData + i*4)) = NMisc::fg_GetRandomUnsigned();

			NStr::CStr Return;

			for (mint i = 0; i < c_nCharsInRandomID; ++i)
				Return.f_AddChar(g_UnmistakableChars[RandomData[i] % c_nChars]);

			return Return;
		}

		NStr::CStr CDDPClient::fs_HighEntropyRandomID()
		{
			const mint c_nCharsInRandomID = 17;
			const mint c_nChars = sizeof(g_UnmistakableChars) / sizeof(g_UnmistakableChars[0]) - 1;
			const mint c_BufferSize = ((c_nCharsInRandomID + 3) / 4) * 4;
			uint8 RandomData[c_BufferSize];
			
			NSys::fg_Security_GenerateHighEntropyData(RandomData, c_BufferSize);

			NStr::CStr Return;

			for (mint i = 0; i < c_nCharsInRandomID; ++i)
				Return.f_AddChar(g_UnmistakableChars[RandomData[i] % c_nChars]);

			return Return;
		}
	}
}
