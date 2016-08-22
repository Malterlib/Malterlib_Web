#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Web/WebSocket>
#include <Mib/Encoding/EJSON>
#include <Mib/Storage/Variant>

namespace NMib
{
	namespace NWeb
	{
        class CDDPServerConnection : public NConcurrency::CActor
		{
			struct CInternal;

        public:
			enum EConnectionType
			{
				EConnectionType_WebSocket
				, EConnectionType_SockJSWebsocket
			};
			
            enum EChange
            {
                EChange_Added = 0
                , EChange_Changed
                , EChange_Removed
                , EChange_Ready
                , EChange_Updated
                , EChange_NoSub
            };

            struct CConnectionInfo
            {
                NStr::CStr m_Session;

                ~CConnectionInfo();
				
				CConnectionInfo(CConnectionInfo const &_Other);
				CConnectionInfo(CConnectionInfo &&_Other);
				
				CConnectionInfo &operator =(CConnectionInfo const &_Other);
				CConnectionInfo &operator =(CConnectionInfo &&_Other);
                
                void f_Accept(NStr::CStr const &_Session) const;
                void f_Reject() const;
				
				
			private:
				friend struct CDDPServerConnection::CInternal;
				CConnectionInfo(CDDPServerConnection *_pDDPConnection);

				struct CInternal;
				NIndirection::TCIndirection<CInternal> mp_Internal;
            };
			
            struct CMethodInfo
            {
                NStr::CStr m_Name;
                NStr::CStr m_ID;
				NContainer::TCVector<NEncoding::CEJSON> m_Parameters;
                NEncoding::CEJSON m_RandomSeed;

                ~CMethodInfo();

				CMethodInfo(CMethodInfo const &_Other);
				CMethodInfo(CMethodInfo &&_Other);
				
				CMethodInfo &operator =(CMethodInfo const &_Other);
				CMethodInfo &operator =(CMethodInfo &&_Other);
				
                void f_Result(NEncoding::CEJSON const &_Result, bool _bUpdated = true) const;
                void f_Error(NEncoding::CEJSON const &_Error) const;
				
			private:
				friend struct CDDPServerConnection::CInternal;
				CMethodInfo(CDDPServerConnection *_pDDPConnection);

				struct CInternal;
				NIndirection::TCIndirection<CInternal> mp_Internal;
            };
            
            struct CSubscribeInfo
            {
                NStr::CStr m_ID;
                NStr::CStr m_Name;
                NContainer::TCVector<NEncoding::CEJSON> m_Parameters;
                
                ~CSubscribeInfo();

				CSubscribeInfo(CSubscribeInfo const &_Other);
				CSubscribeInfo(CSubscribeInfo &&_Other);
				
				CSubscribeInfo &operator =(CSubscribeInfo const &_Other);
				CSubscribeInfo &operator =(CSubscribeInfo &&_Other);
				
                void f_Error(NEncoding::CEJSON const &_Error) const;
			
			private:
				friend struct CDDPServerConnection::CInternal;
				CSubscribeInfo(CDDPServerConnection *_pDDPConnection);

				struct CInternal;
				NIndirection::TCIndirection<CInternal> mp_Internal;
            };
            
            struct CAdded
            {
                NStr::CStr m_Collection;
                NStr::CStr m_DocumentID;
                NEncoding::CEJSON m_Fields;
            };
            
            struct CChanged
            {
                NStr::CStr m_Collection;
                NStr::CStr m_DocumentID;
                NEncoding::CEJSON m_Fields;
				NContainer::TCVector<NStr::CStr> m_Cleared;
            };
            
            struct CRemoved
            {
                NStr::CStr m_Collection;
                NStr::CStr m_DocumentID;
            };
            
            struct CReady
            {
                NContainer::TCVector<NStr::CStr> m_Subscriptions;
            };
            
            struct CNoSub
            {
                NStr::CStr m_SubscriptionID;
            };
            
            struct CUpdated
            {
				NContainer::TCVector<NStr::CStr> m_IDs;
            };
			
			using CChange
				= NContainer::TCStreamableVariant
				<
					EChange
					, CAdded, EChange_Added
					, CChanged, EChange_Changed
					, CRemoved, EChange_Removed
					, CReady, EChange_Ready
					, CUpdated, EChange_Updated
					, CNoSub, EChange_NoSub
				>
			;
            
			CDDPServerConnection(CWebSocketNewServerConnection &&_ServerConnection, EConnectionType _ConnectionType);
			~CDDPServerConnection();

			void f_Construct() override;
			NConcurrency::TCContinuation<void> f_Destroy() override;
        
			NConcurrency::CActorSubscription f_Register
				(
					NConcurrency::TCActor<CActor> const &_Actor
					, NFunction::TCFunction<void (CConnectionInfo const &_MethodInfo)> &&_fOnConnection
					, NFunction::TCFunction<void (CMethodInfo const &_MethodInfo)> &&_fOnMethod
                    , NFunction::TCFunction<void (CSubscribeInfo const &_SubscribeInfo)> &&_fOnSubscribe
                    , NFunction::TCFunction<void (NStr::CStr const &_ID)> &&_fOnUnSubscribe
					, NFunction::TCFunction<void (NStr::CStr const &_Error)> &&_fOnError
					, NFunction::TCFunction<void (EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)> &&_fOnClose
				)
			;
            
            void f_SendChanges(NContainer::TCVector<CChange> &&_Changes);

		private:
			void fp_AcceptConnection(NStr::CStr const &_SessionID);
			void fp_RejectConnection();
			void fp_MethodResult(NStr::CStr const &_MethodID, NEncoding::CEJSON const &_Result, bool _bUpdated);
			void fp_MethodError(NStr::CStr const &_MethodID, NEncoding::CEJSON const &_Error);
			void fp_SubscriptionError(NStr::CStr const &_SubscriptionID, NEncoding::CEJSON const &_Error);

			NPtr::TCUniquePointer<CInternal> mp_pInternal;
		};
	}
}
