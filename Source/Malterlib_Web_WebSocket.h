 // Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/ActorFunctorWeak>
#include <Mib/Web/HTTP/Request>
#include <Mib/Web/HTTP/Response>
#include <Mib/Network/Socket>
#include <Mib/Memory/Allocators/Secure>
#include <Mib/Network/ResolveActor>
#include <Mib/Network/DebugFlags>

namespace NMib::NWeb
{
	// RFC 6455 - 7.4.1.
	enum EWebSocketStatus : uint16
	{
		EWebSocketStatus_None = 0
		, EWebSocketStatus_NormalClosure			= 1000	/// Indicates a normal closure, meaning that the purpose for which the connection was established has been fulfilled.
		, EWebSocketStatus_GoingAway				= 1001	/// Indicates that an endpoint is "going away", such as a server going down or a browser having navigated away from a page.
		, EWebSocketStatus_ProtocolError			= 1002	/// Indicates that an endpoint is terminating the connection due to a protocol error.
		, EWebSocketStatus_UnsupportedData			= 1003	/// Indicates that an endpoint is terminating the connection because it has received a type of data it cannot accept (e.g., an
															///	 endpoint that understands only text data MAY send this if it receives a binary message).
		, EWebSocketStatus_Reserved0				= 1004	/// Reserved.  The specific meaning might be defined in the future.
		, EWebSocketStatus_NoStatusReceived			= 1005	/// Is a reserved value and MUST NOT be set as a status code in a Close control frame by an endpoint.  It is designated for use
															///  in applications expecting a status code to indicate that no status code was actually present.
		, EWebSocketStatus_AbnormalClosure			= 1006	/// Is a reserved value and MUST NOT be set as a status code in a Close control frame by an endpoint.  It is designated for use
															///  in applications expecting a status code to indicate that the connection was closed abnormally, e.g., without sending or
															///  receiving a Close control frame.
		, EWebSocketStatus_InvalidFramePayloadData	= 1007	/// Indicates that an endpoint is terminating the connection because it has received data within a message that was not consistent
															///  with the type of the message (e.g., non-UTF-8 [RFC3629] data within a text message).
		, EWebSocketStatus_PolicyViolation			= 1008	/// Indicates that an endpoint is terminating the connection because it has received a message that violates its policy. This is a
															///  generic status code that can be returned when there is no other more suitable status code (e.g., 1003 or 1009) or if there is
															///  a need to hide specific details about the policy.
		, EWebSocketStatus_MessageTooBig			= 1009	/// Indicates that an endpoint is terminating the connection because it has received a message that is too big for it to process.
		, EWebSocketStatus_MandatoryExtension		= 1010	/// Indicates that an endpoint (client) is terminating the connection because it has expected the server to negotiate one or more
															///  extension, but the server didn't return them in the response message of the WebSocket handshake.  The list of extensions that
															///  are needed SHOULD appear in the /reason/ part of the Close frame. Note that this status code is not used by the server,
															///  because it can fail the WebSocket handshake instead.
		, EWebSocketStatus_InternalError			= 1011	/// Indicates that a server is terminating the connection because it encountered an unexpected condition that prevented it from
															///  fulfilling the request.
		, EWebSocketStatus_ServiceRestart			= 1012	///
		, EWebSocketStatus_TryAgainLater			= 1013	///
		, EWebSocketStatus_TLSHandshakeFailed		= 1015	/// Is a reserved value and MUST NOT be set as a status code in a Close control frame by an endpoint. It is designated for use in
															///  applications expecting a status code to indicate that the connection was closed due to a failure to perform a TLS handshake
															///  (e.g., the server certificate can't be verified).
		, EWebSocketStatus_ReservedStart			= 1016	/// Status codes in the range 1000-2999 are reserved for definition by this protocol, its future revisions, and extensions
															///  specified in a permanent and readily available public specification.
		, EWebSocketStatus_ReservedEnd				= 2999
		, EWebSocketStatus_IANAStart				= 3000	/// Status codes in the range 3000-3999 are reserved for use by libraries, frameworks, and applications.  These status codes are
															/// registered directly with IANA.  The interpretation of these codes is undefined by this protocol.
		, EWebSocketStatus_IANAEnd					= 3999
		, EWebSocketStatus_Timeout					= 4000	/// Connection timed out
		, EWebSocketStatus_Rejected					= 4001	/// Closed because you rejected the connection
		, EWebSocketStatus_AlreadyClosed			= 4002	/// Already closed
		, EWebSocketStatus_PrivateStart				= 4000	/// Status codes in the range 4000-4999 are reserved for private use and thus can't be registered.  Such codes can be used by
															/// prior agreements between WebSocket applications.  The interpretation of these codes is undefined by this protocol.
		, EWebSocketStatus_PrivateEnd				= 4999

	};

	enum EWebSocketCloseOrigin
	{
		EWebSocketCloseOrigin_Local
		, EWebSocketCloseOrigin_Remote
	};

	namespace NWebSocket
	{
		class CListenActor;
	}
	class CWebSocketServerActor;
	class CWebSocketClientActor;

	struct CWebSocketNewConnection;
	struct CWebSocketNewServerConnection;
	struct CWebSocketNewClientConnection;

	struct CWebsocketSettings
	{
		static constexpr umint mc_DefaultMaxMessageSize = 24 * 1024 * 1024;
		static constexpr umint mc_DefaultFragmentationSize = 32 * 1024;
		static constexpr pfp64 mc_DefaultTimeout = 60.0;

		umint m_MaxMessageSize = mc_DefaultMaxMessageSize;
		umint m_FragmentationSize = mc_DefaultFragmentationSize;
		fp64 m_Timeout = mc_DefaultTimeout;
		bool m_bTimeoutForUnixSockets = true;
	};

	class CWebSocketActor : public NConcurrency::CActor
	{
	public:
		static constexpr NConcurrency::EPriority mc_Priority = NConcurrency::EPriority_NormalHighCPU;

		struct CCloseInfo
		{
			EWebSocketStatus m_Status = EWebSocketStatus_None;
			NStr::CStr m_Reason;
		};

		struct CConnectionInfo
		{
			CConnectionInfo()
				: m_pRequest(fg_Construct())
			{
			}
			CConnectionInfo(CConnectionInfo &&) = default;
			CConnectionInfo(CConnectionInfo const &) = delete;

			NStr::CStr m_ID;
			NStr::CStr m_ProtocolVersion;
			NContainer::TCVector<NStr::CStr> m_Protocols;
			NStorage::TCSharedPointer<NHTTP::CRequest> m_pRequest;
			NStorage::TCUniquePointer<NNetwork::ICSocketConnectionInfo> m_pSocketInfo;
			NMib::NNetwork::CNetAddress m_PeerAddress;
			NStr::CStr m_Error;
			EWebSocketStatus m_ErrorStatus = EWebSocketStatus_None;
		};

		struct CClientConnectionInfo
		{
			CClientConnectionInfo()
				: m_pResponse(fg_Construct())
			{
			}

			NStr::CStr m_Protocol;
			NStorage::TCSharedPointer<NHTTP::CResponseHeader> m_pResponse;
			NStorage::TCUniquePointer<NNetwork::ICSocketConnectionInfo> m_pSocketInfo;
			NMib::NNetwork::CNetAddress m_PeerAddress;
			NStr::CStr m_Error;
			EWebSocketStatus m_ErrorStatus = EWebSocketStatus_None;
		};

		struct CMaybeSecureByteVector : public NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>
		{
			CMaybeSecureByteVector(NContainer::CByteVector &&_Vector)
				: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{fg_Move(_Vector)}
			{
			}
			CMaybeSecureByteVector(NContainer::CSecureByteVector &&_Vector)
				: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{fg_Move(_Vector)}
			{
			}
			CMaybeSecureByteVector(NContainer::CSecureByteVector const &_Vector)
				: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{_Vector}
			{
			}
			CMaybeSecureByteVector(NContainer::CByteVector const &_Vector)
				: NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>{_Vector}
			{
			}

			using NStorage::TCVariant<NContainer::CByteVector, NContainer::CSecureByteVector>::operator =;

			umint f_GetLen() const
			{
				if (this->f_GetTypeID() == 0)
					return this->f_Get<0>().f_GetLen();
				else
					return this->f_Get<1>().f_GetLen();
			}

			uint8 const *f_GetArray() const
			{
				if (this->f_GetTypeID() == 0)
					return this->f_Get<0>().f_GetArray();
				else
					return this->f_Get<1>().f_GetArray();
			}
		};

		struct CMessageBuffers
		{
			CMaybeSecureByteVector m_Data = NContainer::CByteVector{};
			NContainer::TCVector<umint> m_Markers;
		};

		struct CDebugStats
		{
			uint64 m_nSentBytes = 0;
			uint64 m_nReceivedBytes = 0;
			uint64 m_IncomingDataBufferBytes = 0;
			uint64 m_OutgoingDataBufferBytes = 0;
			fp64 m_SecondsSinceLastSend = 0.0;
			fp64 m_SecondsSinceLastReceive = 0.0;
			uint8 m_State = 0;
		};

	public:
		CWebSocketActor(bool _bClient, CWebsocketSettings const &_Settings);
		~CWebSocketActor();

		NConcurrency::TCFuture<void> f_SetTimeout(fp64 _Seconds);

		NConcurrency::TCFuture<void> f_SendBinary(NStorage::TCSharedPointer<NContainer::CIOByteVector> _pMessage, uint32 _Priority);
		NConcurrency::TCFuture<void> f_SendText(NStr::CStr _Data, uint32 _Priority);
		NConcurrency::TCFuture<void> f_SendTextBuffer(NStorage::TCSharedPointer<CMaybeSecureByteVector> _pMessage, uint32 _Priority);
		NConcurrency::TCFuture<void> f_SendTextBuffers(NStorage::TCSharedPointer<CMessageBuffers> _pMessageBuffers, uint32 _Priority);
		NConcurrency::TCFuture<void> f_SendPing(NStorage::TCSharedPointer<NContainer::CIOByteVector> _ApplicationData);
		NConcurrency::TCFuture<void> f_SendPong(NStorage::TCSharedPointer<NContainer::CIOByteVector> _ApplicationData);

		NConcurrency::TCFuture<CCloseInfo> f_Close(EWebSocketStatus _Status, NStr::CStr _Reason);
		NConcurrency::TCFuture<CCloseInfo> f_CloseWithLinger(EWebSocketStatus _Status, NStr::CStr _Reason, fp64 _MaxLingerTime);

#if DMibConfig_Tests_Enable
		NConcurrency::TCFuture<void> f_DebugSetFlags(fp64 _Timeout, NNetwork::ESocketDebugFlag _Flags);
		NConcurrency::TCFuture<void> f_DebugSetMaxWriteOps(aint _nMaxWriteOps); // -1 = unlimited, >=0 = remaining write ops allowed (decrements each op)
#endif
		NConcurrency::TCFuture<CDebugStats> f_DebugGetStats();

		static bool fs_IsValidCloseStatus(EWebSocketStatus _Status);

		constexpr static umint mc_MaxCloseMessageLength = 125 - 2;

	private:
		friend class NWebSocket::CListenActor;
		friend class CWebSocketServerActor;
		friend struct CWebSocketNewConnection;
		friend struct CWebSocketNewServerConnection;
		friend struct CWebSocketNewClientConnection;
		friend class CWebSocketClientActor;


		enum EFinishConnectionResult
		{
			EFinishConnectionResult_Error
			, EFinishConnectionResult_Success
		};

		struct CCallbacks
		{
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<NContainer::CIOByteVector> _pMessage)> m_fOnReceiveBinaryMessage;
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStr::CStr _Message)> m_fOnReceiveTextMessage;
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<NContainer::CIOByteVector> _ApplicationData)> m_fOnReceivePing;
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (NStorage::TCSharedPointer<NContainer::CIOByteVector> _ApplicationData)> m_fOnReceivePong;
			NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EWebSocketStatus _Reason, NStr::CStr _Message, EWebSocketCloseOrigin _Origin)> m_fOnClose;
		};

	private:
		NConcurrency::TCFuture<void> fp_Destroy() override;

		void fp_StateAdded(NNetwork::ENetTCPState _StateAdded);
		void fp_Disconnect(EWebSocketStatus _Status, NStr::CStr const &_Reason, bool _bFatal, EWebSocketCloseOrigin _Origin);
		void fp_SetSocket(NStorage::TCUniquePointer<NNetwork::ICSocket> _pSocket);
		void fp_ProcessIncoming();
		bool fp_ProcessIncomingMessage();
		void fp_ProcessState(NNetwork::ENetTCPState _StateAdded);
		void fp_UpdateSend();
		void fp_Shutdown();
		NConcurrency::CActorSubscription fp_AcceptServerConnection
			(
				NStr::CStr _Protocol
				, NHTTP::CResponseHeader _ResponseHeader
				, CCallbacks _Callbacks
			)
		;
		void fp_RejectServerConnection
			(
				NStr::CStr _Error
				, NHTTP::CResponseHeader _ResponseHeader = NHTTP::CResponseHeader()
				, NStr::CStr _Content = NStr::CStr()
			)
		;
		NConcurrency::CActorSubscription fp_AcceptClientConnection(CCallbacks _Callbacks);
		void fp_StopDeferring();
		void fp_TryStopDeferring();
		void fp_RejectClientConnection(NStr::CStr _Error);
		NConcurrency::CActorSubscription fp_OnFinishServerConnection
			(
				NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EFinishConnectionResult _Result, CConnectionInfo _ConnectionInfo)> _fOnFinishConnection
			)
		;
		NConcurrency::CActorSubscription fp_OnFinishClientConnection
			(
				NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (EFinishConnectionResult _Result, CClientConnectionInfo _ConnectionInfo)> _fOnFinishConnection
				, NHTTP::CRequest _RequestHeader
				, NStr::CStr _ConnectToAddress
				, NStr::CStr _URI
				, NStr::CStr _Origin
				, NContainer::TCVector<NStr::CStr> _Protocols
			)
		;

	public:
		struct CInternal;

	private:
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};

	struct CWebSocketNewConnection : public CWebSocketActor::CCallbacks
	{
		CWebSocketNewConnection(CWebSocketNewConnection &&_Other) = default;
		CWebSocketNewConnection(NConcurrency::TCActor<CWebSocketActor> const &_Connection);
	protected:
		NConcurrency::TCActor<CWebSocketActor> mp_Connection;
	};

	struct CWebSocketNewClientConnection : public CWebSocketNewConnection
	{
		NHTTP::CResponseHeader m_Response;
		NStr::CStr m_Protocol;
		NStorage::TCUniquePointer<NNetwork::ICSocketConnectionInfo> m_pSocketInfo;
		NMib::NNetwork::CNetAddress m_PeerAddress;

		template <typename tf_CResultCall>
		NConcurrency::TCActor<CWebSocketActor> f_Accept(tf_CResultCall &&_CallbackResultCall);
		void f_Reject(NStr::CStr const &_Error) const;

		CWebSocketNewClientConnection
			(
				NHTTP::CResponseHeader &&_Response
				, NStr::CStr &&_Protocol
				, NConcurrency::TCActor<CWebSocketActor> const &_Connection
				, NStorage::TCUniquePointer<NNetwork::ICSocketConnectionInfo> &&_pSocketInfo
				, NMib::NNetwork::CNetAddress const &_PeerAddress
			)
		;
		~CWebSocketNewClientConnection();
		CWebSocketNewClientConnection(CWebSocketNewClientConnection &&_Other);

	private:
		CWebSocketNewClientConnection &operator =(CWebSocketNewClientConnection const &);
		CWebSocketNewClientConnection &operator =(CWebSocketNewClientConnection &&);
		CWebSocketNewClientConnection(CWebSocketNewClientConnection const &_Other);

		struct CRepliedHelper
		{
			NConcurrency::TCActor<CWebSocketActor> m_Connection;
			NAtomic::TCAtomic<bool> m_bRepliedToConnection;
			CRepliedHelper(NConcurrency::TCActor<CWebSocketActor> const &_Connection);
			~CRepliedHelper();
		};
		NStorage::TCSharedPointer<CRepliedHelper> mp_pHelper;
	};

	struct CWebSocketNewServerConnection : public CWebSocketNewConnection
	{
		CWebSocketActor::CConnectionInfo m_Info;
		NContainer::TCVector<NStr::CStr> m_Protocols;

		template <typename tf_CResultCall>
		NConcurrency::TCActor<CWebSocketActor> f_Accept(NStr::CStr const &_Protocol, tf_CResultCall &&_CallbackResultCall, NHTTP::CResponseHeader &&_ResponseHeader = NHTTP::CResponseHeader());
		void f_Reject(NStr::CStr const &_Error, NHTTP::CResponseHeader &&_ResponseHeader = NHTTP::CResponseHeader()) const;

		CWebSocketNewServerConnection(CWebSocketActor::CConnectionInfo &&_ConnectionInfo, NContainer::TCVector<NStr::CStr> &&_Protocols, NConcurrency::TCActor<CWebSocketActor> const &_Connection);
		~CWebSocketNewServerConnection();

		CWebSocketNewServerConnection(CWebSocketNewServerConnection &&_Other)
			: CWebSocketNewConnection(fg_Move(_Other))
			, m_Info(fg_Move(_Other.m_Info))
			, m_Protocols(fg_Move(_Other.m_Protocols))
			, mp_pHelper(fg_Move(_Other.mp_pHelper))
		{
		}

	private:
		CWebSocketNewServerConnection &operator =(CWebSocketNewServerConnection const &);
		CWebSocketNewServerConnection &operator =(CWebSocketNewServerConnection &&);
		CWebSocketNewServerConnection(CWebSocketNewServerConnection const &_Other);

		struct CRepliedHelper
		{
			NConcurrency::TCActor<CWebSocketActor> m_Connection;
			NAtomic::TCAtomic<bool> m_bRepliedToConnection;
			CRepliedHelper(NConcurrency::TCActor<CWebSocketActor> const &_Connection);
			~CRepliedHelper();
		};
		NStorage::TCSharedPointer<CRepliedHelper> mp_pHelper;
	};

	class CWebSocketClientActor : public NConcurrency::CActor
	{
	public:
		CWebSocketClientActor(CWebsocketSettings const &_DefaultSettings = {});
		~CWebSocketClientActor();

		void f_SetDefaultMaxMessageSize(umint _MaxMessageSize);
		void f_SetDefaultFragmentationSize(umint _FragmentationSize);
		void f_SetDefaultTimeout(fp64 _Timeout);

		NConcurrency::TCFuture<CWebSocketNewClientConnection> f_Connect
			(
				NStr::CStr _ConnectToAddress	// The server to connect to
				, NStr::CStr _BindAddress	// The src address to bind to. Leave empty to not bind
				, NMib::NNetwork::ENetAddressType _PreferAddress // The preferred type of address to connect to
				, uint16 _Port	// The port to connect to
				, NStr::CStr _URI // The server path: /chat
				, NStr::CStr _Origin	// The server origin: http://example.com
				, NContainer::TCVector<NStr::CStr> _Protocols	// The protocols to ask the server to talk with
				, NHTTP::CRequest _Request // Can be used to specify additional fields you want to sent to server initial handshake request to the server. The request line is ignored
				, NNetwork::FVirtualSocketFactory _SocketFactory // The factory to use for creating the sockets. If empty/nullptr it will default to CSocket_TCP::fs_GetFactory()
			)
		; // You will receive an exception if connection fails

	private:
		NConcurrency::TCFuture<void> fp_Destroy() override;

		struct CPendingConnection
		{
			~CPendingConnection();

			NStorage::TCUniquePointer<NNetwork::ICSocket> m_pSocket;
			NConcurrency::CActorSubscription m_OnFinishConnectionSubscription;
			NStorage::TCSharedPointer<NAtomic::TCAtomic<bool>> m_pDeleted = fg_Construct(false);
		};
		NContainer::TCLinkedList<CPendingConnection> mp_PendingConnects;
		NConcurrency::TCActor<NNetwork::CResolveActor> mp_AddressResolver;
		CWebsocketSettings mp_DefaultSettings;
	};

	class CWebSocketServerActor : public NConcurrency::CActor
	{
		friend class NWebSocket::CListenActor;
	public:

		CWebSocketServerActor(CWebsocketSettings const &_DefaultSettings = {});
		~CWebSocketServerActor();

		struct CListenResult
		{
			NConcurrency::CActorSubscription m_Subscription;
			NContainer::TCVector<uint16> m_ListenPorts;
		};

		NConcurrency::TCFuture<CListenResult> f_StartListen
			(
				uint16 _StartListen		// The port to listen to
				, uint16 _nListen		// The number of ports to listen to. In consecutive order from the _StartListen port
				, NMib::NNetwork::ENetFlag _ListenFlags
				, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketNewServerConnection _Connection)> _fNewConnection	// The functor called on the actor for each new connection
				, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketActor::CConnectionInfo _ConnectionInfo)> _fFailedConnection	// The functor called on the actor for each connection attempt that failed
				, NNetwork::FVirtualSocketFactory _SocketFactory // The factory to use for creating the sockets. If empty/nullptr it will default to CSocket_TCP::fs_GetFactory()
			)
		;

		NConcurrency::TCFuture<CListenResult> f_StartListenAddress
			(
				NContainer::TCVector<NNetwork::CNetAddress> _AddressesToListenTo // The addresses to listen to
				, NMib::NNetwork::ENetFlag _ListenFlags
				, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketNewServerConnection _Connection)> _fNewConnection	// The functor called on the actor for each new connection
				, NConcurrency::TCActorFunctorWeak<NConcurrency::TCFuture<void> (CWebSocketActor::CConnectionInfo _ConnectionInfo)> _fFailedConnection	// The functor called on the actor for each connection attempt that failed
				, NNetwork::FVirtualSocketFactory _SocketFactory // The factory to use for creating the sockets. If empty/nullptr it will default to CSocket_TCP::fs_GetFactory()
			)
		;

		void f_SetDefaultMaxMessageSize(umint _MaxMessageSize);
		void f_SetDefaultFragmentationSize(umint _FragmentationSize);
		void f_SetDefaultTimeout(fp64 _Timeout);

#if DMibConfig_Tests_Enable
		void f_Debug_SetBroken(bool _bBroken);
#endif

	private:
		NConcurrency::TCFuture<void> fp_Destroy() override;

		void fp_AddConnection(NConcurrency::TCActor<CWebSocketActor> _Connection);

	public:
		struct CInternal;

	private:
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#include "Malterlib_Web_WebSocket.hpp"

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
