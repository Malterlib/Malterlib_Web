// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Web/WebSocket>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Cryptography/Certificate>

/*
URI invalid -> MUST fail
If /secure/ is true -> MUST performe TLS handshake after opening, but before handshake
MUST: "Server Name Indication" extension in the TLS handshake
MUST send an opening handshake
	Valid HTTP request
	GET, version 1.1 or later
	GET /chat HTTP/1.1
*/

using namespace NMib::NWeb;
using namespace NMib::NNetwork;
using namespace NMib;
using namespace NMib::NTest;
using namespace NMib::NThread;
using namespace NMib::NContainer;
using namespace NMib::NStr;
using namespace NMib::NCryptography;
using namespace NMib::NFunction;

namespace
{
	CPublicKeySetting gc_TestTestKeySetting = CPublicKeySettings_EC_secp256r1{};
	char const *g_pCloseMessage = "Malterlib Web closed connection";
	fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;
}

class CWebsocket_Tests : public NMib::NTest::CTest
{
public:

	void fp_Test
		(
			TCFunction<NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory> ()> const &_fGetFactories
			, CStr const &_AcceptError
			, CStr const &_ConnectError
		 	, bool _bTestTimeout = false
		)
	{
		DMibTestCategory("IP")
		{
			fp_TestImp(_fGetFactories, _AcceptError, _ConnectError, "localhost", _bTestTimeout);
		};
		DMibTestCategory("Unix")
		{
			fp_TestImp(_fGetFactories, _AcceptError, _ConnectError, "UNIX:" + NNetwork::fg_GetSafeUnixSocketPath("{}/Websocket.socket"_f << NFile::CFile::fs_GetProgramDirectory()), false);
		};
	}

	struct CState : public NStorage::TCSharedPointerIntrusiveBase<>
	{
		struct CServerConnection
		{
			NConcurrency::TCActor<CWebSocketActor> m_Actor;
			NConcurrency::CActorSubscription m_CallbacksReference;
		};

		CMutual m_Lock;
		CEventAutoReset m_Event;

		NConcurrency::TCActor<CWebSocketServerActor> m_ServerActor;
		NConcurrency::CActorSubscription m_ListenCallbackReference;

		NContainer::TCLinkedList<CServerConnection> m_ServerConnections;

		NStr::CStr m_AcceptError;
		bool m_bAcceptError = false;
		NStr::CStr m_ListenError;

		NConcurrency::TCActor<CWebSocketClientActor> m_ClientActor;

		NConcurrency::TCActor<CWebSocketActor> m_ClientSocket;
		NConcurrency::CActorSubscription m_ClientActorCallbacksReference;

		NStr::CStr m_ClientConnectionError;
		bool m_bClientConnectionResult = false;

		EWebSocketCloseOrigin m_ClientConnectionCloseOrigin = EWebSocketCloseOrigin_Local;
		EWebSocketCloseOrigin m_ServerConnectionCloseOrigin = EWebSocketCloseOrigin_Local;
		EWebSocketStatus m_ClientConnectionCloseStatus = EWebSocketStatus_None;
		EWebSocketStatus m_ServerConnectionCloseStatus = EWebSocketStatus_None;
		NStr::CStr m_ClientConnectionCloseMessage;
		NStr::CStr m_ServerConnectionCloseMessage;

		NContainer::TCVector<NStr::CStr> m_Messages;

		bool m_bCleared = false;

		void f_Clear()
		{
			DMibLock(m_Lock);
			m_bCleared = true;
			m_ClientActorCallbacksReference.f_Clear();
			m_ClientSocket.f_Clear();
			m_ClientActor.f_Clear();
			m_ListenCallbackReference.f_Clear();
			m_ServerConnections.f_Clear();
			if (m_ServerActor)
			{
				auto ServerActor = m_ServerActor;
				{
					DMibUnlock(m_Lock);
					ServerActor->f_BlockDestroy(); // Make sure to release listen socket
				}
				m_ServerActor.f_Clear();
			}
		}

		void f_StartListen(CNetAddress _ListenAddress, NNetwork::FVirtualSocketFactory const &_ServerFactory)
		{
			NStorage::TCSharedPointer<CState> pState = fg_Explicit(this);
			m_ServerActor
				(
					&CWebSocketServerActor::f_StartListenAddress
					, fg_CreateVector(_ListenAddress)
					, NMib::NNetwork::ENetFlag_None
					, NMib::NConcurrency::fg_ConcurrentActor()
					, [pState](CWebSocketNewServerConnection &&_ConnectionInfo)
					{
						CWebSocketNewServerConnection ConnectionInfo = fg_Move(_ConnectionInfo);
						DMibLock(pState->m_Lock);

						CState::CServerConnection *pServerConnection = &pState->m_ServerConnections.f_Insert();

						ConnectionInfo.m_fOnReceiveTextMessage
							= [pState](NStr::CStr const &_Message)
							{
								DMibLock(pState->m_Lock);
								for (auto &Connection : pState->m_ServerConnections)
								{
									Connection.m_Actor(&CWebSocketActor::f_SendText, _Message + "Reply", 0)
										> NConcurrency::fg_DiscardResult()
									;
								}

								if (_Message == "Disconnect")
								{
									DMibLock(pState->m_Lock);
									for (auto &Connection : pState->m_ServerConnections)
										Connection.m_Actor(&CWebSocketActor::f_Close, EWebSocketStatus_NormalClosure, g_pCloseMessage) > NConcurrency::fg_DiscardResult();
								}
							}
						;

						ConnectionInfo.m_fOnClose
							= [pState, pServerConnection](EWebSocketStatus _Status, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)
							{
								DMibLock(pState->m_Lock);
								if (pState->m_bCleared)
									return;
								pState->m_ServerConnectionCloseMessage = _Message;
								pState->m_ServerConnectionCloseStatus = _Status;
								pState->m_ServerConnectionCloseOrigin = _Origin;
								pState->m_ServerConnections.f_Remove(*pServerConnection);
								pState->m_Event.f_Signal();
							}
						;

						NStr::CStr Protocol;
						if (!ConnectionInfo.m_Protocols.f_IsEmpty())
							Protocol = ConnectionInfo.m_Protocols.f_GetFirst();

						pServerConnection->m_Actor = ConnectionInfo.f_Accept
							(
								Protocol
								, NMib::NConcurrency::fg_ConcurrentActor() / [pState, pServerConnection](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Callback)
								{
									DMibLock(pState->m_Lock);
									if (_Callback)
										pServerConnection->m_CallbacksReference = fg_Move(*_Callback);
								}
							)
						;

						pState->m_Event.f_Signal();
					}
					, [pState](CWebSocketActor::CConnectionInfo && _ConnectionInfo)
					{
						DMibLock(pState->m_Lock);
						pState->m_bAcceptError = true;
						pState->m_AcceptError = _ConnectionInfo.m_Error;
						pState->m_Event.f_Signal();
					}
					, fg_TempCopy(_ServerFactory)
				)
				> NMib::NConcurrency::fg_ConcurrentActor() / [pState](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Result)
				{
					DMibLock(pState->m_Lock);
					if (_Result)
						pState->m_ListenCallbackReference = fg_Move(*_Result);
					else
						pState->m_ListenError = _Result.f_GetExceptionStr();
					pState->m_Event.f_Signal();
				}
			;
			bool bTimedOutListenStart = pState->m_Event.f_WaitTimeout(20.0);
			DMibAssert(pState->m_ListenError, ==, "");
			DMibAssertFalse(bTimedOutListenStart);
			DMibAssertTrue(pState->m_ListenCallbackReference);
		}

		void f_Connect(CStr const &_Address, NNetwork::FVirtualSocketFactory const &_ClientFactory)
		{
			NStorage::TCSharedPointer<CState> pState = fg_Explicit(this);
			m_ClientActor
				(
					&CWebSocketClientActor::f_Connect
					, _Address
					, ""
					, NNetwork::ENetAddressType_None
					, 10500
					, "/Test"
					, fg_Format("http://{}", _Address)
					, NContainer::fg_CreateVector<NStr::CStr>("Test")
					, NHTTP::CRequest()
					, fg_TempCopy(_ClientFactory)
				)
				> NMib::NConcurrency::fg_ConcurrentActor() / [pState](NConcurrency::TCAsyncResult<CWebSocketNewClientConnection> &&_Result)
				{
					DMibLock(pState->m_Lock);
					if (_Result)
					{
						auto &Result = *_Result;

						Result.m_fOnClose = [pState](EWebSocketStatus _Status, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)
							{
								DMibLock(pState->m_Lock);
								pState->m_ClientConnectionCloseMessage = _Message;
								pState->m_ClientConnectionCloseStatus = _Status;
								pState->m_ClientConnectionCloseOrigin = _Origin;
								pState->m_Event.f_Signal();
							}
						;
						Result.m_fOnReceiveTextMessage = [pState](NStr::CStr const &_Message)
							{
								DMibLock(pState->m_Lock);
								pState->m_Messages.f_Insert(_Message);
								pState->m_Event.f_Signal();
							}
						;

						pState->m_ClientSocket = Result.f_Accept
							(
								NMib::NConcurrency::fg_ConcurrentActor() / [pState](NConcurrency::TCAsyncResult<NConcurrency::CActorSubscription> &&_Callback)
								{
									DMibLock(pState->m_Lock);
									if (_Callback)
										pState->m_ClientActorCallbacksReference = fg_Move(*_Callback);
								}
							)
						;
					}
					else
						pState->m_ClientConnectionError = _Result.f_GetExceptionStr();

					pState->m_bClientConnectionResult = true;
					pState->m_Event.f_Signal();
				}
			;
		}
	};

	bool fp_TestConnect(NStorage::TCSharedPointer<CState> const &_pState, CStr const &_AcceptError, CStr const &_ConnectError)
	{
		auto pState = _pState;
		DMibTestPath("Connect");

		bool bTimedOut = false;

		while (!bTimedOut)
		{
			{
				DMibLock(pState->m_Lock);
				if (pState->m_bAcceptError && pState->m_bClientConnectionResult)
					break; // Server accept failed
				if (pState->m_bClientConnectionResult)
				{
					if
						(
							!pState->m_ClientSocket
							&&
							(
								pState->m_bAcceptError
								|| !pState->m_ServerConnections.f_IsEmpty()
								|| (!pState->m_ClientConnectionError.f_IsEmpty() && _AcceptError.f_IsEmpty())
							)
						)
					{
						break; // Client connection failed
					}

					if (pState->m_ClientSocket && !pState->m_ServerConnections.f_IsEmpty())
						break; // Successfully done
				}
			}
			bTimedOut = pState->m_Event.f_WaitTimeout(20.0);
		}


		DMibTest(!DMibExpr(bTimedOut));

		DMibLock(pState->m_Lock);

		if (!_ConnectError.f_IsEmpty())
		{
			DMibExpect(pState->m_ClientConnectionError, ==, _ConnectError);
			return false;
		}

		if (!_AcceptError.f_IsEmpty())
		{
			DMibTest(DMibExpr(pState->m_bAcceptError));
			DMibExpect(pState->m_AcceptError, ==, _AcceptError);
			return false;
		}
		DMibTest(!DMibExpr(pState->m_bAcceptError));
		DMibTest(DMibExpr(pState->m_AcceptError) == DMibExpr(""));

		DMibTest(DMibExpr(pState->m_ClientConnectionError) == DMibExpr(""));

		DMibAssertFalse(pState->m_ServerConnections.f_IsEmpty());
		DMibAssertTrue(pState->m_ClientSocket);
		return true;
	}

	bool fp_WaitForCondition(TCFunction<bool ()> const &_fPredicate)
	{
		bool bTimedOut = false;

		NTime::CClock Clock;
		Clock.f_Start();

		while (!_fPredicate())
		{
			NSys::fg_Thread_Sleep(0.01f);
			if (Clock.f_GetTime() > 30.0)
			{
				bTimedOut = true;
				break;
			}
		}

		return bTimedOut;
	}

	void fp_TestImp
		(
			TCFunction<NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory> ()> const &_fGetFactories
			, CStr const &_AcceptError
			, CStr const &_ConnectError
			, CStr const &_Address
		 	, bool _bTestTimeout
		)
	{
		DMibTestSuite("Connection")
		{
			auto Factories = _fGetFactories();
			auto ServerFactory = fg_Get<0>(Factories);
			auto ClientFactory = fg_Get<1>(Factories);

			CNetAddress ListenAddress;
			if (_Address == "localhost")
			{
				CNetAddressTCPv4 Address;
				Address.f_SetLocalhost();
				Address.m_Port = 10500;
				ListenAddress = Address;
			}
			else
				ListenAddress = CSocket::fs_ResolveAddress(_Address);
			{
				NStorage::TCSharedPointer<CState> pState = fg_Construct();
				auto Cleanup
					= g_OnScopeExit / [&]
					{
						pState->f_Clear();
					}
				;

				pState->m_ServerActor = NConcurrency::fg_ConstructActor<CWebSocketServerActor>();
				pState->f_StartListen(ListenAddress, ServerFactory);

				pState->m_ClientActor = NConcurrency::fg_ConstructActor<CWebSocketClientActor>();
				pState->f_Connect(_Address, ClientFactory);

				if (!fp_TestConnect(pState, _AcceptError, _ConnectError))
					return;
				{
					DMibTestPath("Messages");

					pState->m_ClientSocket(&CWebSocketActor::f_SendText, "TestText", 0).f_CallSync(g_Timeout / 3);
					NContainer::CByteVector Buffer = {'T', 'e', 's', 't', 'B', 'u', 'f', 'f'};
					NStorage::TCSharedPointer<CWebSocketActor::CMaybeSecureByteVector> pMessage = fg_Construct(Buffer);
					pState->m_ClientSocket(&CWebSocketActor::f_SendTextBuffer, pMessage, 0).f_CallSync(g_Timeout / 3);

					NStorage::TCSharedPointer<CWebSocketActor::CMessageBuffers> pMessageBuffers = fg_Construct();
					pMessageBuffers->m_Data = Buffer.f_ToSecure();
					pMessageBuffers->m_Markers = {0, 4};
					pState->m_ClientSocket(&CWebSocketActor::f_SendTextBuffers, pMessageBuffers, 0).f_CallSync(g_Timeout / 3);

					bool bTimedOut = false;
					while (!bTimedOut)
					{
						{
							DMibLock(pState->m_Lock);
							if (pState->m_Messages.f_GetLen() >= 4)
								break;
						}
						bTimedOut = pState->m_Event.f_WaitTimeout(20.0);
					}

					DMibExpectFalse(bTimedOut);
					DMibExpect(pState->m_Messages.f_GetLen(), ==, 4)(NTest::ETest_FailAndStop);
					DMibExpect(pState->m_Messages[0], ==, "TestTextReply");
					DMibExpect(pState->m_Messages[1], ==, "TestBuffReply");
					DMibExpect(pState->m_Messages[2], ==, "TestReply");
					DMibExpect(pState->m_Messages[3], ==, "BuffReply");
				}

 				{
					DMibTestPath("Disconnect");

					pState->m_ClientSocket(&CWebSocketActor::f_SendText, "Disconnect", 0) > NConcurrency::fg_DiscardResult();

					bool bTimedOut = false;
					while (!bTimedOut)
					{
						{
							DMibLock(pState->m_Lock);
							if (!pState->m_ClientConnectionCloseMessage.f_IsEmpty() && pState->m_ServerConnections.f_IsEmpty())
								break; // Successfully disconnected from the server
						}
						bTimedOut = pState->m_Event.f_WaitTimeout(20.0);
					}

					DMibTest(!DMibExpr(bTimedOut));
					DMibExpect(pState->m_ClientConnectionCloseMessage, ==, g_pCloseMessage);
					DMibExpect(pState->m_ServerConnectionCloseMessage, ==, g_pCloseMessage);

				}
			}

			if (_bTestTimeout)
			{
				DMibTestPath("Timeout");
				NStorage::TCSharedPointer<CState> pState = fg_Construct();
				auto Cleanup
					= g_OnScopeExit / [&]
					{
						pState->f_Clear();
					}
				;

				pState->m_ServerActor = NConcurrency::fg_ConstructActor<CWebSocketServerActor>();
				pState->m_ServerActor(&CWebSocketServerActor::f_SetDefaultTimeout, 1.0).f_CallSync(g_Timeout / 3);
				pState->f_StartListen(ListenAddress, ServerFactory);

				pState->m_ClientActor = NConcurrency::fg_ConstructActor<CWebSocketClientActor>();
				pState->m_ClientActor(&CWebSocketClientActor::f_SetDefaultTimeout, 1.0).f_CallSync(g_Timeout / 3);
				pState->f_Connect(_Address, ClientFactory);

				if (!fp_TestConnect(pState, _AcceptError, _ConnectError))
					return;
				{
					DMibTestPath("Non timeout");
					NSys::fg_Thread_Sleep(2.0);

					DMibLock(pState->m_Lock);
					DMibExpect(pState->m_ServerConnectionCloseStatus, ==, EWebSocketStatus_None);
					DMibExpect(pState->m_ClientConnectionCloseStatus, ==, EWebSocketStatus_None);
				}
				{
					DMibTestPath("Timeout");
					pState->m_ClientSocket(&CWebSocketActor::f_DebugStopProcessing, 1.0).f_CallSync(g_Timeout / 3);

					bool bTimedOut = fp_WaitForCondition
						(
							[&]
							{
								DMibLock(pState->m_Lock);
								return pState->m_ServerConnectionCloseStatus == EWebSocketStatus_Timeout || pState->m_ClientConnectionCloseStatus == EWebSocketStatus_Timeout;
							}
						)
					;

					DMibExpectFalse(bTimedOut);
					DMibLock(pState->m_Lock);
					DMibTest
						(
							DMibExpr(pState->m_ServerConnectionCloseStatus) == DMibExpr(EWebSocketStatus_Timeout)
							|| DMibExpr(pState->m_ClientConnectionCloseStatus) == DMibExpr(EWebSocketStatus_Timeout)
						)
					;
				}
			}
		};
	}

	void f_DoTests()
	{
		DMibTestCategory("TCP")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						return {nullptr, nullptr};
					}
					, ""
					, ""
				 	, true
				)
			;
		};
		DMibTestCategory("SSL")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions Options;
						Options.m_CommonName = "Malterlib test Self Signed";
						Options.m_Hostnames = fg_CreateVector<CStr>("localhost");
						Options.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(Options, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				 	, true
				)
			;
		};
		DMibTestCategory("SSL Client Certificate")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CByteVector CertificateRequestData;

						CCertificateOptions ClientOptions;
						ClientOptions.m_CommonName = "Test Client";
						ClientOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateClientCertificateRequest(ClientOptions, CertificateRequestData, ClientSettings.m_PrivateKeyData);
						CCertificate::fs_SignClientCertificate(ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData, CertificateRequestData, ClientSettings.m_PublicCertificateData);

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				)
			;
		};
		DMibTestCategory("SSL Client Certificate Chain Without Intermediate CA")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;

						CCertificateOptions ClientOptions;
						ClientOptions.m_CommonName = "Test Client";
						ClientOptions.m_KeySetting = gc_TestTestKeySetting;

						CByteVector CertificateRequestData;
						CCertificate::fs_GenerateClientCertificateRequest(ClientOptions, CertificateRequestData, ClientSettings.m_PrivateKeyData);
						CCertificate::fs_SignClientCertificate(ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData, CertificateRequestData, ClientSettings.m_PublicCertificateData);

						CSSLSettings ClientSettings2;
						ClientSettings2.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings2.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CCertificateOptions ClientOptions2;
						ClientOptions2.m_CommonName = "Test Client";
						ClientOptions2.m_KeySetting = gc_TestTestKeySetting;

						CByteVector CertificateRequestData2;
						CCertificate::fs_GenerateClientCertificateRequest(ClientOptions2, CertificateRequestData2, ClientSettings2.m_PrivateKeyData);
						CCertificate::fs_SignClientCertificate(ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData, CertificateRequestData2, ClientSettings2.m_PublicCertificateData);

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings2);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
					, ""
				)
			;
		};
		DMibTestCategory("SSL Client Certificate Incorrect")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CCertificateOptions ClientOptions;
						ClientOptions.m_CommonName = "Test Client";
						ClientOptions.m_KeySetting = gc_TestTestKeySetting;
						ClientOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");

						CCertificate::fs_GenerateSelfSignedCertAndKey(ClientOptions, ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData);

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
					, ""
				)
			;
		};
		DMibTestCategory("SSL Client Certificate Missing")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: PEER_DID_NOT_RETURN_A_CERTIFICATE"
					, ""
				)
			;
		};
		DMibTestCategory("SSL Client Certificate Allow Missing")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						ServerSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_AllowMissingPeerCertificate;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				)
			;
		};
		DMibTestCategory("SSL Client Certificate Incorrect Allow Missing")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						ServerSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_AllowMissingPeerCertificate;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CCertificateOptions ClientOptions;
						ClientOptions.m_CommonName = "Test Client";
						ClientOptions.m_KeySetting = gc_TestTestKeySetting;
						ClientOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");

						CCertificate::fs_GenerateSelfSignedCertAndKey(ClientOptions, ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData);

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
					, ""
				)
			;
		};
		DMibTestCategory("SSL Server Certificate Incorrect")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						CSSLSettings ServerSettings2;
						CCertificateOptions ServerOptions2;
						ServerOptions2.m_CommonName = "Malterlib test Self Signed";
						ServerOptions2.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions2.m_KeySetting = gc_TestTestKeySetting;
						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions2, ServerSettings2.m_PublicCertificateData, ServerSettings2.m_PrivateKeyData);

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_CACertificateData = ServerSettings2.m_PublicCertificateData;

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
				)
			;
		};
		DMibTestCategory("SSL Server Certificate Self Signed")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseOSStoreIfNoCASpecified;
						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
				)
			;
		};
		DMibTestCategory("SSL Server Certificate Child Cert")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;
						CByteVector RootCertData;
						CSecureByteVector RootKeyData;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, RootCertData, RootKeyData);

						CByteVector ChildCertData;
						CSecureByteVector ChildKeyData;
						CByteVector RequestData;

						CCertificateOptions RequestOptions;
						RequestOptions.m_CommonName = "Malterlib test request";
						RequestOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						RequestOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateClientCertificateRequest(RequestOptions, RequestData, ChildKeyData);

						CCertificate::fs_SignClientCertificate(RootCertData, RootKeyData, RequestData, ChildCertData);

						ServerSettings.m_PublicCertificateData = ChildCertData;
						ServerSettings.m_PrivateKeyData = ChildKeyData;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_CACertificateData = RootCertData;

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				)
			;
		};
		DMibTestCategory("SSL Server Certificate Incorrect Specific")
		{
			fp_Test
				(
					[]() -> NStorage::TCTuple<NNetwork::FVirtualSocketFactory, NNetwork::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;
						CByteVector RootCertData;
						CSecureByteVector RootKeyData;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, RootCertData, RootKeyData);

						CByteVector ChildCertData;
						CSecureByteVector ChildKeyData;
						CByteVector RequestData;

						CCertificateOptions RequestOptions;
						RequestOptions.m_CommonName = "Malterlib test request";
						RequestOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						RequestOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateClientCertificateRequest(RequestOptions, RequestData, ChildKeyData);

						CCertificate::fs_SignClientCertificate(RootCertData, RootKeyData, RequestData, ChildCertData);

						ServerSettings.m_PublicCertificateData = ChildCertData;
						ServerSettings.m_PrivateKeyData = ChildKeyData;

						NStorage::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = RootCertData;

						NStorage::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, "Socket closed: Mismatching specific certificate"
				)
			;
		};
	}
};

DMibTestRegister(CWebsocket_Tests, Malterlib::Web);
