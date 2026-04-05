// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Web/HTTP/Request>
#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Web_DDP_Client.h"

namespace NMib::NWeb
{
	struct CDDPClient::CCollection
	{
		NContainer::TCMap<NStr::CStr, NEncoding::CEJsonSorted> m_Documents;

		static NStr::CStr fs_IDParse(NStr::CStr const &_ID)
		{
			if (_ID.f_StartsWith("-") || _ID.f_StartsWith("~"))
				return _ID.f_Extract(1);

			return _ID;
		}
	};

	struct CDDPClient::CInternal : public NConcurrency::CActorInternal
	{
		enum EChangeOperation
		{
			EChangeOperation_Added
			, EChangeOperation_Changed
		};

		struct CSubscription
		{
			~CSubscription()
			{
				*m_pDestroyed = true;
			}

			NStr::CStr const &f_GetID() const
			{
				return NContainer::TCMap<NStr::CStr, CSubscription>::fs_GetKey(*this);
			}

			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (ESubscriptionNotification _Notification, NEncoding::CEJsonSorted _Message)> m_fCallback;
			ESubscriptionNotification m_NotifyOn;
			NFunction::TCFunctionMovable<void ()> m_fOnReady;
			NFunction::TCFunctionMovable<void (NStr::CStr const &_Error)> m_fOnError;
			bool m_bWasError = false;
			NStorage::TCSharedPointer<bool> m_pDestroyed = fg_Construct(false);
		};

		struct CCollectionObservations;
		struct CObservation
		{
			~CObservation()
			{
				*m_pDestroyed = true;
			}

			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EObserveNotification _Notification, NEncoding::CEJsonSorted _Message)> m_fCallback;
			EObserveNotification m_NotifyOn;
			NStorage::TCSharedPointer<bool> m_pDestroyed = fg_Construct(false);
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
				, NNetwork::FVirtualSocketFactory const &_SocketFactory
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
				NNetwork::CSSLSettings ClientSettings;
				ClientSettings.m_VerificationFlags |= NNetwork::CSSLSettings::EVerificationFlag_UseOSStoreIfNoCASpecified;
				NStorage::TCSharedPointer<NNetwork::CSSLContext> pClientContext = fg_Construct(NNetwork::CSSLContext::EType_Client, ClientSettings);
				m_SocketFactory = NNetwork::CSocket_SSL::fs_GetFactory(pClientContext);
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
		NConcurrency::CActorSubscription m_WebSocketCallbackSubscription;
		NConcurrency::TCActor<CWebSocketClientActor> m_ConnectionFactory;
		NNetwork::FVirtualSocketFactory m_SocketFactory;
		NConcurrency::TCPromise<CConnectInfo> m_ConnectPromise;

		NStr::CStr m_UserName;
		NStr::CStrSecure m_Password;
		NStr::CStrSecure m_LoginToken;

		NStr::CStr m_SessionID;

		uint64 m_LastMethodID = 0;

		NContainer::TCMap<uint64, NFunction::TCFunctionMovable<void (NStr::CStr const &_Error, NEncoding::CEJsonSorted &&_Result)>> m_PendingMethodCalls;
		NContainer::TCMap<NStr::CStr, CCollection> m_Collections;

		NContainer::TCMap<uint64, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> ()>> m_PendingMethodUpdated;

		NContainer::TCMap<NStr::CStr, CSubscription> m_Subscriptions;

		NContainer::TCMap<NStr::CStr, CCollectionObservations> m_Observations;

		NConcurrency::CActorSubscription m_ConnectTimeoutTimerRef;

		fp32 m_ConnectTimeout = 60.0;

		EClientOption m_ClientOptions;

		bool m_bConnectFinished = false;
		bool m_bConnectCalled = false;

		bool m_bDestructorCalled = false;

		void fp_OnError(NStr::CStr const &_Error);
		void fp_ReceiveMessage(NStr::CStr const &_Message);
		void fp_HandleRemoved(NEncoding::CEJsonSorted const &_Message);
		void fp_HandleAddedChanged(NEncoding::CEJsonSorted const &_Message, EChangeOperation _Operation);
		void fp_HandleReady(NEncoding::CEJsonSorted const &_Message);
		void fp_HandleUpdated(NEncoding::CEJsonSorted const &_Message);
		void fp_HandleNoSub(NEncoding::CEJsonSorted const &_Message);
		void fp_NotifyObserve(NStr::CStr const &_Collection, NEncoding::CEJsonSorted const &_Message, EObserveNotification _Notification);
		uint64 fp_SendMethod
			(
				NStr::CStr const &_MethodName
				, NContainer::TCVector<NEncoding::CEJsonSorted> const &_Params
				, NFunction::TCFunctionMovable<void (NStr::CStr const &_Error, NEncoding::CEJsonSorted &&_Result)> &&_fOnResult
			)
		;
		void fp_SendLoginResumeMessage();
		void fp_SendLoginMessage();
		void fp_SendConnect(fp32 _Timeout, NStr::CStr const &_SessionID);
		void fp_SendMessage(NEncoding::CEJsonSorted const &_Message);

		CConnectInfo fp_ExtractConnectInfo(NEncoding::CEJsonSorted const &_Info);
	};


	NContainer::TCMap<NStr::CStr, NEncoding::CEJsonSorted>::CIteratorConst CDDPClient::CCollectionAccessor::f_GetDocumentIterator() const
	{
		return mp_pCollection->m_Documents.f_GetIterator();
	}

	NEncoding::CEJsonSorted const &CDDPClient::CCollectionAccessor::f_GetDocument(NStr::CStr const &_Id) const
	{
		auto pDocument = mp_pCollection->m_Documents.f_FindEqual(_Id);
		if (!pDocument)
			DMibError(fg_Format("No document with ID '{}' within Collection '{}' exists!", _Id, NContainer::TCMap<NStr::CStr, CCollection>::fs_GetKey(mp_pCollection)));

		return *pDocument;
	}

	NContainer::TCMap<NStr::CStr, NEncoding::CEJsonSorted>::CIteratorConst CDDPClient::CCollectionAccessor::f_GetRandomDocumentIterator() const
	{
		auto iDoc = mp_pCollection->m_Documents.f_GetIterator();
		umint nDocuments = mp_pCollection->m_Documents.f_GetLen();
		if (nDocuments <= 1)
			return iDoc;

		umint iIndex = NMisc::fg_Random().f_GetValue(nDocuments);
		for (umint i = 0; i < iIndex; ++i)
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
			, NNetwork::FVirtualSocketFactory const &_SocketFactory
			, EClientOption _ClientOptions
		)
		: mp_pInternal(fg_Construct(this, _ConnectTo, _BindTo, _ConnectionFactory, _Origin, _SocketFactory, _ClientOptions))
	{
	}

	CDDPClient::~CDDPClient()
	{
	}

	NConcurrency::TCFuture<CDDPClient::CConnectInfo> CDDPClient::f_Connect
		(
			NStr::CStr _UserName
			, NStr::CStrSecure _Password
			, NStr::CStrSecure _Token
			, NStr::CStr _SessionID
			, fp32 _Timeout
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EWebSocketStatus _Reason, NStr::CStr _Message, EWebSocketCloseOrigin _Origin)> _fOnClose
		)
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_bConnectCalled)
			co_return DMibErrorInstance("The DDP client can only be connected once");

		Internal.m_bConnectCalled = true;
		Internal.m_UserName = _UserName;
		Internal.m_Password = _Password;
		Internal.m_LoginToken = _Token;

		Internal.m_ConnectPromise = {};

		auto ConnectResult = co_await Internal.m_ConnectionFactory
			(
				&CWebSocketClientActor::f_Connect
				, Internal.m_ConnectTo.f_GetHost()
				, Internal.m_BindTo
				, NNetwork::ENetAddressType_None
				, Internal.m_ConnectTo.f_GetPortFromScheme()
				, Internal.m_ConnectTo.f_GetFullPath()
				, Internal.m_Origin
				, NContainer::fg_CreateVector<NStr::CStr>()
				, NHTTP::CRequest()
				, fg_TempCopy(Internal.m_SocketFactory)
			)
			.f_Wrap()
		;

		if (!ConnectResult)
			co_return ConnectResult.f_GetException();

		auto &Result = *ConnectResult;

		Result.m_fOnReceiveTextMessage = NConcurrency::g_ActorFunctorWeak / [this](NStr::CStr _Message) -> NConcurrency::TCFuture<void>
			{
				mp_pInternal->fp_ReceiveMessage(_Message);

				co_return {};
			}
		;

		if (_fOnClose)
		{
			Result.m_fOnClose = NConcurrency::g_ActorFunctorWeak / [fOnClose = fg_Move(_fOnClose)]
				(EWebSocketStatus _Reason, NStr::CStr _Message, EWebSocketCloseOrigin _Origin) mutable -> NConcurrency::TCFuture<void>
				{
					co_await fOnClose(_Reason, _Message, _Origin).f_Timeout(60.0, "Timed out waiting for on close");

					co_return {};
				}
			;
		}

		Internal.m_WebSocket = Result.f_Accept
			(
				fg_ThisActor(this) / [this](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
				{
					auto &Internal = *mp_pInternal;
					if (_Result)
						mp_pInternal->m_WebSocketCallbackSubscription = fg_Move(_Result.f_Get());
					else
					{
						if (!Internal.m_ConnectPromise.f_IsSet())
							Internal.m_ConnectPromise.f_SetException(fg_Move(_Result));
					}
				}
			)
		;

		Internal.fp_SendConnect(_Timeout, _SessionID);

		co_return co_await Internal.m_ConnectPromise.f_Future();
	}

	NConcurrency::TCFuture<void> CDDPClient::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		NConcurrency::CLogError LogError("DDPClient");

		Internal.m_ConnectTimeoutTimerRef.f_Clear();

		if (!Internal.m_ConnectPromise.f_IsSet())
			Internal.m_ConnectPromise.f_SetException(DMibErrorInstance("Destroy called before connection finished"));

		if (Internal.m_WebSocket)
			co_await fg_Move(Internal.m_WebSocket).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy Websocket");

		co_return {};
	}

	NConcurrency::TCFuture<NEncoding::CEJsonSorted> CDDPClient::f_Method(NStr::CStr _MethodName, NContainer::TCVector<NEncoding::CEJsonSorted> _Params)
	{
		NConcurrency::TCPromiseFuturePair<NEncoding::CEJsonSorted> Promise;
		auto &Internal = *mp_pInternal;
		Internal.fp_SendMethod
			(
				_MethodName
				, _Params
				, [Promise = fg_Move(Promise.m_Promise)](NStr::CStr const &_Error, NEncoding::CEJsonSorted &&_Result)
				{
					if (!_Error.f_IsEmpty())
						Promise.f_SetException(DMibErrorInstance(_Error));
					else
						Promise.f_SetResult(fg_Move(_Result));
				}
			)
		;
		co_return co_await fg_Move(Promise.m_Future);
	}

	NConcurrency::TCFuture<NEncoding::CEJsonSorted> CDDPClient::f_MethodWithUpdated
		(
			NStr::CStr _MethodName
			, NContainer::TCVector<NEncoding::CEJsonSorted> _Params
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> ()> _fOnUpdated
		)
	{
		NConcurrency::TCPromiseFuturePair<NEncoding::CEJsonSorted> Promise;
		auto &Internal = *mp_pInternal;
		uint64 MethodID = Internal.fp_SendMethod
			(
				_MethodName
				, _Params
				, [Promise = fg_Move(Promise.m_Promise)](NStr::CStr const &_Error, NEncoding::CEJsonSorted &&_Result)
				{
					if (!_Error.f_IsEmpty())
						Promise.f_SetException(DMibErrorInstance(_Error));
					else
						Promise.f_SetResult(fg_Move(_Result));
				}
			)
		;

		Internal.m_PendingMethodUpdated[MethodID] = fg_Move(_fOnUpdated);

		co_return co_await fg_Move(Promise.m_Future);

	}

	NConcurrency::CActorSubscription CDDPClient::f_Observe
		(
			NStr::CStr _CollectionName // Leave empty to observe all collections
			, EObserveNotification _NotifyOn
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EObserveNotification _Notification, NEncoding::CEJsonSorted _Message)> _Callback
		)
	{
		auto &Internal = *mp_pInternal;
		auto &CollectionObservations = Internal.m_Observations[_CollectionName];
		auto &Observation = CollectionObservations.m_Observations.f_Insert();

		if (_Callback)
			Observation.m_NotifyOn = _NotifyOn;
		else
			Observation.m_NotifyOn = EObserveNotification_None;

		Observation.m_fCallback = fg_Move(_Callback);

		return NConcurrency::g_ActorSubscription / [this, pDestroyed = Observation.m_pDestroyed, pObservation = &Observation, pCollectionObservations = &CollectionObservations]()
			-> NConcurrency::TCFuture<void>
			{
				if (*pDestroyed)
					co_return {};

				auto &Internal = *mp_pInternal;

				auto ToDestroy = fg_Move(pObservation->m_fCallback);

				pCollectionObservations->m_Observations.f_Remove(*pObservation);
				if (pCollectionObservations->m_Observations.f_IsEmpty())
					Internal.m_Observations.f_Remove(pCollectionObservations);

				co_await fg_Move(ToDestroy).f_Destroy();

				co_return {};
			}
		;
	}

	NConcurrency::TCFuture<NConcurrency::CActorSubscription> CDDPClient::f_Subscribe
		(
			NStr::CStr _SubscriptionName
			, NStr::CStr _SubscriptionID
			, NEncoding::CEJsonSorted _Params
			, ESubscriptionNotification _NotifyOn
			, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (ESubscriptionNotification _Notification, NEncoding::CEJsonSorted _Message)> _Callback
			, bool _bWaitForResponse
		)
	{
		NStr::CStr SubscriptionID = _SubscriptionID;

		if (SubscriptionID.f_IsEmpty())
			SubscriptionID = fs_RandomID();
		auto &Internal = *mp_pInternal;
		auto &Subscription = *Internal.m_Subscriptions(SubscriptionID);

		if (_Callback)
			Subscription.m_NotifyOn = _NotifyOn;
		else
			Subscription.m_NotifyOn = ESubscriptionNotification_None;

		NEncoding::CEJsonSorted Message;

		Message["msg"] = "sub";
		Message["id"] = SubscriptionID;
		Message["name"] = _SubscriptionName;
		if (_Params.f_IsValid())
			Message["params"] = fg_Move(_Params);

		Internal.fp_SendMessage(Message);

		Subscription.m_fCallback = fg_Move(_Callback);

		NConcurrency::CActorSubscription CleanupSubscription = NConcurrency::g_ActorSubscription / [this, pDestroyed = Subscription.m_pDestroyed, pSubscription = &Subscription]()
			-> NConcurrency::TCFuture<void>
			{
				if (*pDestroyed)
					co_return {};

				auto &Internal = *mp_pInternal;

				if (!pSubscription->m_bWasError)
				{
					NEncoding::CEJsonSorted Message;

					Message["msg"] = "unsub";
					Message["id"] = pSubscription->f_GetID();

					Internal.fp_SendMessage(Message);
				}

				auto ToDestroy = fg_Move(pSubscription->m_fCallback);

				Internal.m_Subscriptions.f_Remove(pSubscription);

				co_await fg_Move(ToDestroy).f_Destroy();

				co_return {};
			}
		;

		if (!_bWaitForResponse)
			co_return fg_Move(CleanupSubscription);

		NConcurrency::TCPromiseFuturePair<NConcurrency::CActorSubscription> Promise;

		Subscription.m_fOnReady = [Promise = Promise.m_Promise, CleanupSubscription = fg_Move(CleanupSubscription)]() mutable
			{
				if (!Promise.f_IsSet())
					Promise.f_SetResult(fg_Move(CleanupSubscription));
			}
		;
		Subscription.m_fOnError = [Promise = fg_Move(Promise.m_Promise)](NStr::CStr const &_Error)
			{
				if (!Promise.f_IsSet())
					Promise.f_SetException(DMibErrorInstance(_Error));
			}
		;

		co_return co_await fg_Move(Promise.m_Future);
	}

	void CDDPClient::f_AccessData(NFunction::TCFunctionMovable<void (CDataAccessor const &_Accessor)> &&_ProcessData)
	{
		_ProcessData(CDataAccessor(mp_pInternal.f_Get()));
	}

	void CDDPClient::CInternal::fp_OnError(NStr::CStr const &_Error)
	{
		if (!m_ConnectPromise.f_IsSet())
			m_ConnectPromise.f_SetException(DMibErrorInstance(_Error));
	}

	void CDDPClient::CInternal::fp_SendMessage(NEncoding::CEJsonSorted const &_Message)
	{
		if (m_WebSocket)
			m_WebSocket.f_Bind<&CWebSocketActor::f_SendText>(_Message.f_ToString(nullptr), 0).f_DiscardResult();
	}

	void CDDPClient::CInternal::fp_NotifyObserve(NStr::CStr const &_Collection, NEncoding::CEJsonSorted const &_Message, EObserveNotification _Notification)
	{
		auto fNotify = [&](CCollectionObservations &_CollectionObservations)
			{
				for (auto &Observation : _CollectionObservations.m_Observations)
				{
					if (Observation.m_NotifyOn & _Notification)
					{
						if (Observation.m_fCallback)
							Observation.m_fCallback.f_CallDiscard(_Notification, _Message);
					}
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

	void CDDPClient::CInternal::fp_HandleRemoved(NEncoding::CEJsonSorted const &_Message)
	{
		auto pCollection = _Message.f_GetMember("collection");
		auto pID = _Message.f_GetMember("id");

		if (!pCollection || pCollection->f_Type() != NEncoding::EJsonType_String)
			return;
		if (!pID || pID->f_Type() != NEncoding::EJsonType_String)
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

	void CDDPClient::CInternal::fp_HandleAddedChanged(NEncoding::CEJsonSorted const &_Message, EChangeOperation _Operation)
	{
		auto pCollection = _Message.f_GetMember("collection");
		auto pID = _Message.f_GetMember("id");
		auto pFields = _Message.f_GetMember("fields");

		if (!pCollection || pCollection->f_Type() != NEncoding::EJsonType_String)
			return;
		if (!pID || pID->f_Type() != NEncoding::EJsonType_String)
			return;
		if (pFields && pFields->f_Type() != NEncoding::EJsonType_Object)
			return;

		auto const ID = CCollection::fs_IDParse(pID->f_String());
		if (ID.f_IsEmpty())
			return;

		if (m_ClientOptions & EClientOption_MaintainDatabase)
		{
			auto &Collection = m_Collections[pCollection->f_String()];
			auto &Object = Collection.m_Documents[ID];

			if (pFields)
			{
				if (_Operation == EChangeOperation_Changed)
				{
					for (auto iField = pFields->f_Object().f_OrderedIterator(); iField; ++iField)
						Object[iField->f_Name()] = iField->f_Value();

					{
						auto pCleared = _Message.f_GetMember("cleared");
						if (pCleared && pCleared->f_Type() == NEncoding::EJsonType_Array)
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
		}
		if (_Operation == EChangeOperation_Changed)
			fp_NotifyObserve(pCollection->f_String(), _Message, EObserveNotification_Changed);
		else if (_Operation == EChangeOperation_Added)
			fp_NotifyObserve(pCollection->f_String(), _Message, EObserveNotification_Added);
	}

	void CDDPClient::CInternal::fp_HandleReady(NEncoding::CEJsonSorted const &_Message)
	{
		auto pSubscriptions = _Message.f_GetMember("subs");
		if (!pSubscriptions || pSubscriptions->f_Type() != NEncoding::EJsonType_Array)
			return;

		for (auto iSubId = pSubscriptions->f_Array().f_GetIterator(); iSubId; ++iSubId)
		{
			auto pSub = m_Subscriptions.f_FindEqual(iSubId->f_AsString(""));
			if (!pSub)
				continue;

			if (pSub->m_fOnReady)
			{
				pSub->m_fOnReady();
				pSub->m_fOnReady.f_Clear();
				pSub->m_fOnReady.f_Clear();
			}

			if (pSub->m_NotifyOn & ESubscriptionNotification_Ready)
			{
				if (pSub->m_fCallback)
					pSub->m_fCallback.f_CallDiscard(ESubscriptionNotification_Ready, NEncoding::CEJsonSorted());
			}
		}
	}

	void CDDPClient::CInternal::fp_HandleUpdated(NEncoding::CEJsonSorted const &_Message)
	{
		auto pMethods = _Message.f_GetMember("methods", NEncoding::EJsonType_Array);
		if (!pMethods)
			return;

		for (auto iMethod = pMethods->f_Array().f_GetIterator(); iMethod; ++iMethod)
		{
			uint64 MethodID = iMethod->f_AsString("").f_ToInt(uint64(0));
			auto pMethodUpdated = m_PendingMethodUpdated.f_FindEqual(MethodID);
			if (!pMethodUpdated)
				continue;

			auto Cleanup = g_OnScopeExit / [&]()
				{
					m_PendingMethodUpdated.f_Remove(MethodID);
				}
			;

			pMethodUpdated->f_CallDiscard();
		}
	}

	namespace
	{
		NStr::CStr fg_DecodeError(NEncoding::CEJsonSorted const &_Error)
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

	void CDDPClient::CInternal::fp_HandleNoSub(NEncoding::CEJsonSorted const &_Message)
	{
		auto pID = _Message.f_GetMember("id");
		if (!pID || pID->f_Type() != NEncoding::EJsonType_String)
			return;

		auto pError = _Message.f_GetMember("error");

		if (pError)
		{
			if (auto pSub = m_Subscriptions.f_FindEqual(CCollection::fs_IDParse(pID->f_String())))
			{
				pSub->m_bWasError = true;
				if (pSub->m_fOnError)
				{
					NStr::CStr ErrorMessage;
					ErrorMessage = fg_DecodeError(*pError);

					pSub->m_fOnError(ErrorMessage);

					pSub->m_fOnError.f_Clear();
					pSub->m_fOnReady.f_Clear();
				}

				if (pSub->m_NotifyOn & ESubscriptionNotification_Error)
				{
					if (pSub->m_fCallback)
						pSub->m_fCallback.f_CallDiscard(ESubscriptionNotification_Error, _Message);
				}
			}
		}

		if (!pError)
			fp_NotifyObserve(NStr::CStr(), _Message, EObserveNotification_NoSub);
	}

	void CDDPClient::CInternal::fp_ReceiveMessage(NStr::CStr const &_Message)
	{
		try
		{
			NEncoding::CEJsonSorted Json = NEncoding::CEJsonSorted::fs_FromString(_Message);

			auto *pValue = Json.f_GetMember("msg");
			if (!pValue)
			{
				if (Json.f_GetMember("server_id"))
					return;

				fp_OnError(NStr::fg_Format("No msg in DDP packet: {}", Json));
				return;
			}

			if (pValue->f_Type() != NEncoding::EJsonType_String)
			{
				fp_OnError("Invalid type for msg (should be String)");
				return;
			}

			auto &MessageType = pValue->f_String();
			if (MessageType == "result")
			{
				auto pID = Json.f_GetMember("id");
				if (!pID)
					return;

				uint64 MessageID = pID->f_AsString().f_ToInt(uint64(0));
				if (!MessageID)
					return;

				auto pPending = m_PendingMethodCalls.f_FindEqual(MessageID);
				if (!pPending)
					return;

				auto Cleanup = g_OnScopeExit / [&]()
					{
						m_PendingMethodCalls.f_Remove(pPending);
					}
				;
				NStr::CStr ErrorMessage;
				NEncoding::CEJsonSorted Result;

				auto pError = Json.f_GetMember("error");
				if (pError && pError->f_IsObject())
					ErrorMessage = fg_DecodeError(*pError);

				if (auto pResult = Json.f_GetMember("result"))
					Result = fg_Move(*pResult);

				(*pPending)(ErrorMessage, fg_Move(Result));
			}
			else if (MessageType == "added")
			{
				fp_HandleAddedChanged(Json, EChangeOperation_Added);
			}
			else if (MessageType == "updated")
			{
				fp_HandleUpdated(Json);
			}
			else if (MessageType == "changed")
			{
				fp_HandleAddedChanged(Json, EChangeOperation_Changed);
			}
			else if (MessageType == "removed")
			{
				fp_HandleRemoved(Json);
			}
			else if (MessageType == "ready")
			{
				fp_HandleReady(Json);
			}
			else if (MessageType == "nosub")
			{
				fp_HandleNoSub(Json);
			}
			else if (MessageType == "ping")
			{
				NEncoding::CEJsonSorted Reply;
				Reply["msg"] = "pong";

				if (auto pID = Json.f_GetMember("id"))
					Reply["id"] = *pID;

				fp_SendMessage(Reply);
			}
			else if (MessageType == "connected")
			{
				m_bConnectFinished = true;
				m_ConnectTimeoutTimerRef.f_Clear();

				if (auto pSessionID = Json.f_GetMember("session"))
					m_SessionID = pSessionID->f_String();
				else
					m_SessionID = NStr::CStr();

				if (m_UserName.f_IsEmpty())
				{
					if (!m_ConnectPromise.f_IsSet())
						m_ConnectPromise.f_SetResult(m_SessionID);
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
				if (auto pError = Json.f_GetMember("reason"))
					Error = pError->f_AsString();
				fp_OnError(Error);
			}
			else if (MessageType == "failed")
			{
				m_bConnectFinished = true;
				m_ConnectTimeoutTimerRef.f_Clear();

				NStr::CStr SuggestedVersion = "Unknown";
				if (auto pVersion = Json.f_GetMember("version"))
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
			, NContainer::TCVector<NEncoding::CEJsonSorted> const &_Params
			, NFunction::TCFunctionMovable<void (NStr::CStr const &_Error, NEncoding::CEJsonSorted &&_Result)> &&_fOnResult
		)
	{
		uint64 MethodID = ++m_LastMethodID;

		NEncoding::CEJsonSorted Message;

		Message["msg"] = "method";
		Message["method"] = _MethodName;
		Message["params"] = _Params;
		Message["id"] = NStr::CStr::fs_ToStr(MethodID);

		fp_SendMessage(Message);;

		m_PendingMethodCalls[MethodID] = fg_Move(_fOnResult);

		return MethodID;
	}

	auto CDDPClient::CInternal::fp_ExtractConnectInfo(NEncoding::CEJsonSorted const &_Info) -> CConnectInfo
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
		NEncoding::CEJsonSorted LoginParams;
		LoginParams["resume"] = m_LoginToken;

		fp_SendMethod
			(
				"login"
				, NContainer::fg_CreateVector<NEncoding::CEJsonSorted>(LoginParams)
				, [this](NStr::CStr const &_Error, NEncoding::CEJsonSorted &&_Result)
				{
					if (!_Error.f_IsEmpty())
						fp_SendLoginMessage();
					else if (!m_ConnectPromise.f_IsSet())
						m_ConnectPromise.f_SetResult(fp_ExtractConnectInfo(_Result));
				}
			)
		;
	}

	void CDDPClient::CInternal::fp_SendLoginMessage()
	{
		NEncoding::CEJsonSorted LoginParams;
		LoginParams["user"]["email"] = m_UserName;
		auto &Password = LoginParams["password"];
		Password["digest"] = NCryptography::CHash_SHA256::fs_DigestFromData((uint8 const *)m_Password.f_GetStr(), m_Password.f_GetLen()).f_GetString();
		Password["algorithm"] = "sha-256";

		fp_SendMethod
			(
				"login"
				, NContainer::fg_CreateVector<NEncoding::CEJsonSorted>(LoginParams)
				, [this](NStr::CStr const &_Error, NEncoding::CEJsonSorted &&_Result)
				{
					if (!m_ConnectPromise.f_IsSet())
					{
						if (!_Error.f_IsEmpty())
							m_ConnectPromise.f_SetException(DMibErrorInstance(_Error));
						else
							m_ConnectPromise.f_SetResult(fp_ExtractConnectInfo(_Result));
					}
				}
			)
		;
	}

	void CDDPClient::CInternal::fp_SendConnect(fp32 _Timeout, NStr::CStr const &_SessionID)
	{
		NEncoding::CEJsonSorted Message;

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
				, [this]() -> NConcurrency::TCFuture<void>
				{
					if (!m_bConnectFinished)
					{
						if (!m_ConnectPromise.f_IsSet())
							m_ConnectPromise.f_SetException(DMibErrorInstance("Timed out waiting for reply to connect message"));
					}

					co_return {};
				}
			)
			> fg_ThisActor(m_pThis) / [this](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_TimerReference)
			{
				if (!m_bConnectFinished && !m_pThis->f_IsDestroyed())
					m_ConnectTimeoutTimerRef = fg_Move(*_TimerReference);
			}
		;

		fp_SendMessage(Message);
	}

	NStr::CStr CDDPClient::fs_RandomID()
	{
		return NCryptography::fg_RandomID();
	}

	NStr::CStr CDDPClient::fs_HighEntropyRandomID()
	{
		return NCryptography::fg_HighEntropyRandomID();
	}
}
