// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Web/WebSocket>
#include <Mib/Network/Sockets/SSL>

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
using namespace NMib::NNet;
using namespace NMib;
using namespace NMib::NTest;
using namespace NMib::NThread;
using namespace NMib::NContainer;
using namespace NMib::NStr;

namespace
{
	constexpr mint gc_TestCertificateSize = 1024;
}

class CWebsocket_Tests : public NMib::NTest::CTest
{
public:

	void fp_Test
		(
			NFunction::TCFunction<NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory> ()> const &_fGetFactories
			, CStr const &_AcceptError
			, CStr const &_ConnectError
		)
	{
		DMibTestCategory("IP")
		{
			fp_TestImp(_fGetFactories, _AcceptError, _ConnectError, "localhost");
		};
#ifndef DPlatformFamily_Windows
		DMibTestCategory("Unix")
		{
			fp_TestImp(_fGetFactories, _AcceptError, _ConnectError, fg_Format("UNIX:{}/Websocket.socket", NFile::CFile::fs_GetProgramDirectory()));
		};
#endif
	}
	
	void fp_TestImp
		(
			NFunction::TCFunction<NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory> ()> const &_fGetFactories
			, CStr const &_AcceptError
			, CStr const &_ConnectError
			, CStr const &_Address
		)
	{
		DMibTestSuite("Connection")
		{
			auto Factories = _fGetFactories();
			auto ServerFactory = fg_Get<0>(Factories);
			auto ClientFactory = fg_Get<1>(Factories); 
			
			static char const *s_pCloseMessage = "Malterlib Web closed connection";

			struct CState
			{
				struct CServerConnection
				{
					NConcurrency::TCActor<CWebSocketActor> m_Actor;
					NConcurrency::CActorCallback m_CallbacksReference;
				};
				
				CMutual m_Lock;
				CEventAutoReset m_Event;

				NConcurrency::TCActor<CWebSocketServerActor> m_ServerActor;
				NConcurrency::CActorCallback m_ListenCallbackReference;

				NContainer::TCLinkedList<CServerConnection> m_ServerConnections;
				
				NStr::CStr m_AcceptError;
				bool m_bAcceptError = false;
				NStr::CStr m_ListenError;

				NConcurrency::TCActor<CWebSocketClientActor> m_ClientActor;

				NConcurrency::TCActor<CWebSocketActor> m_ClientSocket;
				NConcurrency::CActorCallback m_ClientActorCallbacksReference;
				
				NStr::CStr m_ClientConnectionError;
				bool m_bClientConnectionResult = false;
				
				NStr::CStr m_ClientConnectionCloseMessage;
				NStr::CStr m_ServerConnectionCloseMessage;
				
				void f_Clear()
				{
					DMibLock(m_Lock);
					m_ClientActorCallbacksReference.f_Clear();
					m_ClientSocket.f_Clear();
					m_ClientActor.f_Clear();
					m_ListenCallbackReference.f_Clear();
					m_ServerConnections.f_Clear();
					m_ServerActor.f_Clear();
					m_ServerActor.f_Clear();
				}
			};
			
			NPtr::TCSharedPointer<CState> pState = fg_Construct();
			
			auto Cleanup 
				= g_OnScopeExit > [&]
				{
					pState->f_Clear();
				}
			;
			
			CNetAddress ListenAddress;
			
			if (_Address == "localhost")
			{
				CNetAddressTCPv4 Address;
				Address.m_Port = 10500;
				ListenAddress = Address;
			}
			else
				ListenAddress = CSocket::fs_ResolveAddress(_Address);
			
			pState->m_ServerActor = NConcurrency::fg_ConstructActor<CWebSocketServerActor>();
			
			pState->m_ServerActor
				(
					&CWebSocketServerActor::f_StartListenAddress
					, fg_CreateVector(ListenAddress)
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
										Connection.m_Actor(&CWebSocketActor::f_Close, EWebSocketStatus_NormalClosure, s_pCloseMessage) > NConcurrency::fg_DiscardResult();
								}
							}
						;
						
						ConnectionInfo.m_fOnClose
							= [pState, pServerConnection](EWebSocketStatus _Status, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)
							{
								DMibLock(pState->m_Lock);
								pState->m_ServerConnectionCloseMessage = _Message;
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
								, NMib::NConcurrency::fg_ConcurrentActor() / [pState, pServerConnection](NConcurrency::TCAsyncResult<NConcurrency::CActorCallback> &&_Callback)
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
					, fg_TempCopy(ServerFactory)
				)
				> NMib::NConcurrency::fg_ConcurrentActor() / [pState](NConcurrency::TCAsyncResult<NConcurrency::CActorCallback> &&_Result)
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

			pState->m_ClientActor = NConcurrency::fg_ConstructActor<CWebSocketClientActor>();
			
			pState->m_ClientActor
				(
					&CWebSocketClientActor::f_Connect
					, _Address
					, ""
					, NNet::ENetAddressType_None
					, 10500
					, "/Test"
					, fg_Format("http://{}", _Address)
					, NContainer::fg_CreateVector<NStr::CStr>("Test")
					, NHTTP::CRequest()
					, fg_TempCopy(ClientFactory)
				)
				> NMib::NConcurrency::fg_ConcurrentActor() / [pState](NConcurrency::TCAsyncResult<CWebSocketNewClientConnection> &&_Result)
				{
					DMibLock(pState->m_Lock);
					if (_Result)
					{
						auto &Result = *_Result;

						Result.m_fOnClose = [pState](EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)
							{
								DMibLock(pState->m_Lock);
								pState->m_ClientConnectionCloseMessage = _Message;
								pState->m_Event.f_Signal();
							}
						;

						pState->m_ClientSocket = Result.f_Accept
							(
								NMib::NConcurrency::fg_ConcurrentActor() / [pState](NConcurrency::TCAsyncResult<NConcurrency::CActorCallback> &&_Callback)
								{
									DMibLock(pState->m_Lock);
									if (_Callback)
										pState->m_ClientActorCallbacksReference = fg_Move(*_Callback);
								}
							)
						;
					}
					else
					{
						pState->m_ClientConnectionError = _Result.f_GetExceptionStr();
					}

					pState->m_bClientConnectionResult = true;
					pState->m_Event.f_Signal();
				}
			;
			
			{
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

				if (!_ConnectError.f_IsEmpty())
				{
					DMibExpect(pState->m_ClientConnectionError, ==, _ConnectError);
					return;
				}
				
				if (!_AcceptError.f_IsEmpty())
				{
					DMibTest(DMibExpr(pState->m_bAcceptError));
					DMibExpect(pState->m_AcceptError, ==, _AcceptError);
					return;
				}
				DMibTest(!DMibExpr(pState->m_bAcceptError));
				DMibTest(DMibExpr(pState->m_AcceptError) == DMibExpr(""));
				
				DMibTest(DMibExpr(pState->m_ClientConnectionError) == DMibExpr(""));
				
				DMibAssertFalse(pState->m_ServerConnections.f_IsEmpty());
				DMibAssertTrue(pState->m_ClientSocket);
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
				DMibExpect(pState->m_ClientConnectionCloseMessage, ==, s_pCloseMessage);
				DMibExpect(pState->m_ServerConnectionCloseMessage, ==, s_pCloseMessage);
				
			}
		};
	}
	
	void f_DoTests()
	{
		DMibTestCategory("TCP")
		{
			fp_Test
				(
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						return {nullptr, nullptr};
					}
					, ""
					, ""
				)
			;
		};
		DMibTestCategory("SSL")
		{
			fp_Test
				(
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions Options;
						Options.m_Subject = "Malterlib test Self Signed";
						Options.m_Hostnames = fg_CreateVector<CStr>("localhost");
						Options.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(Options, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				)
			;
		};
		DMibTestCategory("SSL Client Certificate")
		{
			fp_Test
				(
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						
						TCVector<uint8> CertificateRequestData;

						CSSLContext::CCertificateOptions ClientOptions;
						ClientOptions.m_Subject = "Test Client";
						ClientOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateClientCertificateRequest(ClientOptions, CertificateRequestData, ClientSettings.m_PrivateKeyData);
						CSSLContext::fs_SignClientCertificate(ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData, CertificateRequestData, ClientSettings.m_PublicCertificateData);

						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;

						CSSLContext::CCertificateOptions ClientOptions;
						ClientOptions.m_Subject = "Test Client";
						ClientOptions.m_KeyLength = gc_TestCertificateSize;
						
						TCVector<uint8> CertificateRequestData;
						CSSLContext::fs_GenerateClientCertificateRequest(ClientOptions, CertificateRequestData, ClientSettings.m_PrivateKeyData);
						CSSLContext::fs_SignClientCertificate(ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData, CertificateRequestData, ClientSettings.m_PublicCertificateData);
						
						CSSLSettings ClientSettings2;
						ClientSettings2.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings2.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CSSLContext::CCertificateOptions ClientOptions2;
						ClientOptions2.m_Subject = "Test Client";
						ClientOptions2.m_KeyLength = gc_TestCertificateSize;
						
						TCVector<uint8> CertificateRequestData2;
						CSSLContext::fs_GenerateClientCertificateRequest(ClientOptions2, CertificateRequestData2, ClientSettings2.m_PrivateKeyData);
						CSSLContext::fs_SignClientCertificate(ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData, CertificateRequestData2, ClientSettings2.m_PublicCertificateData);
						
						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings2);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						
						CSSLContext::CCertificateOptions ClientOptions;
						ClientOptions.m_Subject = "Test Client";
						ClientOptions.m_KeyLength = gc_TestCertificateSize;
						ClientOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ClientOptions, ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData);

						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: Peer did not return a certificate"
					, ""
				)
			;
		};
		DMibTestCategory("SSL Client Certificate Allow Missing")
		{
			fp_Test
				(
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						ServerSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_AllowMissingPeerCertificate;

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						ServerSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_AllowMissingPeerCertificate;

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						
						CSSLContext::CCertificateOptions ClientOptions;
						ClientOptions.m_Subject = "Test Client";
						ClientOptions.m_KeyLength = gc_TestCertificateSize;
						ClientOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ClientOptions, ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData);

						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						
						CSSLSettings ServerSettings2;
						CSSLContext::CCertificateOptions ServerOptions2;
						ServerOptions2.m_Subject = "Malterlib test Self Signed";
						ServerOptions2.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions2.m_KeyLength = gc_TestCertificateSize;
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions2, ServerSettings2.m_PublicCertificateData, ServerSettings2.m_PrivateKeyData);

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_CACertificateData = ServerSettings2.m_PublicCertificateData;
						
						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;
						
						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData, gc_TestCertificateSize);
						
						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseOSStoreIfNoCASpecified;
						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;
						TCVector<uint8> RootCertData;
						TCVector<uint8, NMem::CAllocator_HeapSecure> RootKeyData;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, RootCertData, RootKeyData, gc_TestCertificateSize);

						TCVector<uint8> ChildCertData;
						TCVector<uint8, NMem::CAllocator_HeapSecure> ChildKeyData;
						TCVector<uint8> RequestData;

						CSSLContext::CCertificateOptions RequestOptions;
						RequestOptions.m_Subject = "Malterlib test request";
						RequestOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						RequestOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateClientCertificateRequest(RequestOptions, RequestData, ChildKeyData);
						
						CSSLContext::fs_SignClientCertificate(RootCertData, RootKeyData, RequestData, ChildCertData);
						
						ServerSettings.m_PublicCertificateData = ChildCertData;
						ServerSettings.m_PrivateKeyData = ChildKeyData; 

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_CACertificateData = RootCertData;
						
						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
					[]() -> NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;
						TCVector<uint8> RootCertData;
						TCVector<uint8, NMem::CAllocator_HeapSecure> RootKeyData;

						CSSLContext::CCertificateOptions ServerOptions;
						ServerOptions.m_Subject = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateSelfSignedCertAndKey(ServerOptions, RootCertData, RootKeyData, gc_TestCertificateSize);

						TCVector<uint8> ChildCertData;
						TCVector<uint8, NMem::CAllocator_HeapSecure> ChildKeyData;
						TCVector<uint8> RequestData;

						CSSLContext::CCertificateOptions RequestOptions;
						RequestOptions.m_Subject = "Malterlib test request";
						RequestOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						RequestOptions.m_KeyLength = gc_TestCertificateSize;
						
						CSSLContext::fs_GenerateClientCertificateRequest(RequestOptions, RequestData, ChildKeyData);
						
						CSSLContext::fs_SignClientCertificate(RootCertData, RootKeyData, RequestData, ChildCertData);
						
						ServerSettings.m_PublicCertificateData = ChildCertData;
						ServerSettings.m_PrivateKeyData = ChildKeyData; 

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = RootCertData;
						
						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
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
