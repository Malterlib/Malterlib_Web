#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Web/WebSocket>
#include <Mib/Web/HTTP/URL>
#include <Mib/Encoding/EJSON>

namespace NMib::NWeb
{
	class CDDPClient : public NConcurrency::CActor
	{
		struct CInternal;
		struct CCollection;

	public:

		enum ESubscriptionNotification
		{
			ESubscriptionNotification_None = 0
			, ESubscriptionNotification_Ready = DMibBit(0)
			, ESubscriptionNotification_Error = DMibBit(1)
		};

		enum EObserveNotification
		{
			EObserveNotification_None = 0
			, EObserveNotification_Added = DMibBit(0)
			, EObserveNotification_Changed = DMibBit(1)
			, EObserveNotification_Removed = DMibBit(2)
			, EObserveNotification_NoSub = DMibBit(3)
		};

		enum EClientOption
		{
			EClientOption_None = 0
			, EClientOption_MaintainDatabase = DMibBit(0)
		};

		struct CConnectInfo
		{
			CConnectInfo()
			{
			}
			CConnectInfo(NStr::CStr const &_SessionID)
				: m_SessionID(_SessionID)
			{
			}

			NStr::CStr m_SessionID;
			NStr::CStr m_UserID;
			NTime::CTime m_TokenExpires;
			NStr::CStrSecure m_Token;
		};

		struct CCollectionAccessor
		{
			NContainer::TCMap<NStr::CStr, NEncoding::CEJSON>::CIteratorConst f_GetDocumentIterator() const;
			NEncoding::CEJSON const &f_GetDocument(NStr::CStr const &_Id) const;
			NStr::CStr const &f_GetRandomDocumentID() const;
			NContainer::TCMap<NStr::CStr, NEncoding::CEJSON>::CIteratorConst f_GetRandomDocumentIterator() const;

		private:
			friend CDDPClient;

			CCollectionAccessor(CCollection const *_pCollection);

			CCollection const *mp_pCollection;
		};

		struct CDataAccessor
		{
			CCollectionAccessor f_GetCollection(NStr::CStr const &_Name) const;
			NContainer::TCVector<NStr::CStr> f_GetCollectionNames() const;
			bool f_CollectionExists(NStr::CStr const &_Name) const;

		private:
			friend CDDPClient;

			CDataAccessor(CInternal *_pInternal);

			CInternal *mp_pInternal;
		};

		CDDPClient
			(
				NHTTP::CURL const &_ConnectTo
				, NStr::CStr const &_BindTo
				, NConcurrency::TCActor<CWebSocketClientActor> const &_ConnectionFactory = fg_Default()
				, NStr::CStr const &_Origin = NStr::CStr()
				, NNetwork::FVirtualSocketFactory const &_SocketFactory = nullptr
				, EClientOption _ClientOptions = EClientOption_MaintainDatabase
			)
		;
		~CDDPClient();

		NConcurrency::TCContinuation<CConnectInfo> f_Connect
			(
				NStr::CStr const &_UserName
				, NStr::CStrSecure const &_Password
				, NStr::CStrSecure const &_Token
				, NStr::CStr const &_SessionID
				, fp32 _Timeout
				, NConcurrency::TCActor<NConcurrency::CActor> &&_NotificationActor
				, NFunction::TCFunction<void (EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> &&_fOnClose
			)
		;
		NConcurrency::TCContinuation<NEncoding::CEJSON> f_Method(NStr::CStr const &_MethodName, NContainer::TCVector<NEncoding::CEJSON> const &_Params);
		NConcurrency::TCContinuation<NEncoding::CEJSON> f_MethodWithUpdated
			(
				NStr::CStr const &_MethodName
				, NContainer::TCVector<NEncoding::CEJSON> const &_Params
				, NConcurrency::TCActor<NConcurrency::CActor> const &_OnUpdatedActor
				, NFunction::TCFunction<void ()> &&_fOnUpdated
			)
		;
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_Subscribe
			(
				NConcurrency::TCActor<CActor> const &_Actor
				, NStr::CStr const &_SubscriptionName
				, NStr::CStr const &_SubscriptionID
				, NEncoding::CEJSON const &_Params
				, ESubscriptionNotification _NotifyOn
				, NFunction::TCFunction<void (ESubscriptionNotification _Notification, NEncoding::CEJSON const &_Message)> &&_Callback
				, bool _bWaitForResponse
			)
		;
		NConcurrency::CActorSubscription f_Observe
			(
				NConcurrency::TCActor<CActor> const &_Actor
				, NStr::CStr const &_CollectionName // Leave empty to observe all collections
				, EObserveNotification _NotifyOn
				, NFunction::TCFunction<void (EObserveNotification _Notification, NEncoding::CEJSON const &_Message)> &&_Callback
			)
		;

		void f_AccessData(NFunction::TCFunction<void (CDataAccessor const &_Accessor)> const &_ProcessData);

		static NStr::CStr fs_RandomID();
		static NStr::CStr fs_HighEntropyRandomID();

	private:

		NConcurrency::TCContinuation<void> fp_Destroy() override;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}
