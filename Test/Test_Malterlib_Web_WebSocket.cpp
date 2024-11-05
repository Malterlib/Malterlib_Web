// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Web/WebSocket>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Cryptography/Certificate>
#include <Mib/Concurrency/ActorFunctorWeak>
#include <Mib/Concurrency/DistributedActorTestHelpers>

/*
URI invalid -> MUST fail
If /secure/ is true -> MUST performe TLS handshake after opening, but before handshake
MUST: "Server Name Indication" extension in the TLS handshake
MUST send an opening handshake
	Valid HTTP request
	GET, version 1.1 or later
	GET /chat HTTP/1.1
*/

using namespace NMib;
using namespace NMib::NAtomic;
using namespace NMib::NConcurrency;
using namespace NMib::NContainer;
using namespace NMib::NCryptography;
using namespace NMib::NFile;
using namespace NMib::NFunction;
using namespace NMib::NNetwork;
using namespace NMib::NStorage;
using namespace NMib::NStr;
using namespace NMib::NTest;
using namespace NMib::NTime;
using namespace NMib::NThread;
using namespace NMib::NWeb;

namespace
{
	CPublicKeySetting gc_TestTestKeySetting = CPublicKeySettings_EC_secp256r1{};
	constexpr static auto gc_CloseMessage = gc_Str<"Malterlib Web closed connection">;
	constexpr static auto gc_CloseMessageTooLong = gc_Str<"Malterlib Web closed connection. 012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789">;
	static_assert((gc_CloseMessageTooLong.m_StrData.mc_nChars - 1) > CWebSocketActor::mc_MaxCloseMessageLength);

	fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;
}

class CWebsocket_Tests : public CTest
{
public:
	void fp_Test
		(
			TCFunction<TCTuple<FVirtualSocketFactory, FVirtualSocketFactory> ()> const &_fGetFactories
			, CStr const &_AcceptError
			, CStr const &_ConnectError
			, bool _bTestTimeout = false
			, bool _bTestTooLongCloseMessage = false
		)
	{
		{
			DMibTestPath("IP");
			fp_TestImp(_fGetFactories, _AcceptError, _ConnectError, "localhost", _bTestTimeout, _bTestTooLongCloseMessage);
		}
		{
			DMibTestPath("Unix");
			fp_TestImp
				(
					_fGetFactories
					, _AcceptError
					, _ConnectError
					, "UNIX:" + fg_GetSafeUnixSocketPath("{}/Websocket.socket"_f << CFile::fs_GetProgramDirectory())
					, false
					, _bTestTooLongCloseMessage
				)
			;
		}
	}

	struct CState
	{
		struct CServerConnection
		{
			~CServerConnection()
			{
				m_pDeleted->f_Store(true);
			}

			TCActor<CWebSocketActor> m_Actor;
			CActorSubscription m_CallbacksReference;
			TCSharedPointer<TCAtomic<bool>> m_pDeleted = fg_Construct(false);
		};

		CIntrusiveRefCount m_RefCount;

		CMutual m_Lock;
		CEventAutoReset m_Event;

		TCActor<CWebSocketServerActor> m_ServerActor;
		CActorSubscription m_ListenCallbackReference;
		uint16 m_ListenPort = 0;

		TCLinkedList<CServerConnection> m_ServerConnections;

		CStr m_AcceptError;
		bool m_bAcceptError = false;
		CStr m_ListenError;

		TCActor<CWebSocketClientActor> m_ClientActor;

		TCActor<CWebSocketActor> m_ClientSocket;
		CActorSubscription m_ClientActorCallbacksReference;

		CStr m_ClientConnectionError;
		bool m_bClientConnectionResult = false;

		EWebSocketCloseOrigin m_ClientConnectionCloseOrigin = EWebSocketCloseOrigin_Local;
		EWebSocketCloseOrigin m_ServerConnectionCloseOrigin = EWebSocketCloseOrigin_Local;
		EWebSocketStatus m_ClientConnectionCloseStatus = EWebSocketStatus_None;
		EWebSocketStatus m_ServerConnectionCloseStatus = EWebSocketStatus_None;
		CStr m_ClientConnectionCloseMessage;
		CStr m_ServerConnectionCloseMessage;

		TCVector<CStr> m_Messages;

		bool m_bCleared = false;

		void f_Clear(TCSharedPointer<CDefaultRunLoop> const &_pRunLoop)
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
					ServerActor->f_BlockDestroy(_pRunLoop->f_ActorDestroyLoop()); // Make sure to release listen socket
				}
				m_ServerActor.f_Clear();
			}
		}

		uint16 f_StartListen(CNetAddress _ListenAddress, FVirtualSocketFactory const &_ServerFactory)
		{
			TCSharedPointer<CState> pState = fg_Explicit(this);
			m_ServerActor
				(
					&CWebSocketServerActor::f_StartListenAddress
					, fg_CreateVector(_ListenAddress)
					, ENetFlag_None
					, g_ActorFunctorWeak(fg_ConcurrentActor()) / [pState](CWebSocketNewServerConnection _ConnectionInfo) -> TCFuture<void>
					{
						CWebSocketNewServerConnection ConnectionInfo = fg_Move(_ConnectionInfo);
						DMibLock(pState->m_Lock);

						CState::CServerConnection *pServerConnection = &pState->m_ServerConnections.f_Insert();

						ConnectionInfo.m_fOnReceiveTextMessage = g_ActorFunctorWeak / [pState](CStr _Message) -> TCFuture<void>
							{
								DMibLock(pState->m_Lock);
								for (auto &Connection : pState->m_ServerConnections)
									Connection.m_Actor(&CWebSocketActor::f_SendText, _Message + "Reply", 0).f_DiscardResult();

								if (_Message == "Disconnect")
								{
									DMibLock(pState->m_Lock);
									for (auto &Connection : pState->m_ServerConnections)
										Connection.m_Actor(&CWebSocketActor::f_Close, EWebSocketStatus_NormalClosure, gc_CloseMessage).f_DiscardResult();
								}

								if (_Message == "DisconnectLongClose")
								{
									DMibLock(pState->m_Lock);
									for (auto &Connection : pState->m_ServerConnections)
										Connection.m_Actor(&CWebSocketActor::f_Close, EWebSocketStatus_NormalClosure, gc_CloseMessageTooLong).f_DiscardResult();
								}

								co_return {};
							}
						;

						ConnectionInfo.m_fOnClose = g_ActorFunctorWeak / [pState, pServerConnection, pDeleted = pServerConnection->m_pDeleted]
							(EWebSocketStatus _Status, CStr _Message, EWebSocketCloseOrigin _Origin) -> TCFuture<void>
							{
								DMibLock(pState->m_Lock);
								if (pState->m_bCleared)
									co_return {};

								pState->m_ServerConnectionCloseMessage = _Message;
								pState->m_ServerConnectionCloseStatus = _Status;
								pState->m_ServerConnectionCloseOrigin = _Origin;
								if (!*pDeleted)
									pState->m_ServerConnections.f_Remove(*pServerConnection);
								pState->m_Event.f_Signal();

								co_return {};
							}
						;

						CStr Protocol;
						if (!ConnectionInfo.m_Protocols.f_IsEmpty())
							Protocol = ConnectionInfo.m_Protocols.f_GetFirst();

						pServerConnection->m_Actor = ConnectionInfo.f_Accept
							(
								Protocol
								, fg_ConcurrentActor() / [pState, pServerConnection, pDeleted = pServerConnection->m_pDeleted]
								(TCAsyncResult<CActorSubscription> &&_Callback)
								{
									DMibLock(pState->m_Lock);
									if (_Callback && !*pDeleted)
										pServerConnection->m_CallbacksReference = fg_Move(*_Callback);
								}
							)
						;

						pState->m_Event.f_Signal();

						co_return {};
					}
					, g_ActorFunctorWeak(fg_ConcurrentActor()) / [pState](CWebSocketActor::CConnectionInfo _ConnectionInfo) -> TCFuture<void>
					{
						DMibLock(pState->m_Lock);
						pState->m_bAcceptError = true;
						pState->m_AcceptError = _ConnectionInfo.m_Error;
						pState->m_Event.f_Signal();

						co_return {};
					}
					, fg_TempCopy(_ServerFactory)
				)
				> fg_ConcurrentActor() / [pState](TCAsyncResult<CWebSocketServerActor::CListenResult> &&_Result)
				{
					DMibLock(pState->m_Lock);
					if (_Result)
					{
						pState->m_ListenCallbackReference = fg_Move(_Result->m_Subscription);
						pState->m_ListenPort = _Result->m_ListenPorts[0];
					}
					else
						pState->m_ListenError = _Result.f_GetExceptionStr();
					pState->m_Event.f_Signal();
				}
			;
			bool bTimedOutListenStart = pState->m_Event.f_WaitTimeout(20.0);
			DMibAssert(pState->m_ListenError, ==, "");
			DMibAssertFalse(bTimedOutListenStart);
			DMibAssertTrue(pState->m_ListenCallbackReference);

			return pState->m_ListenPort;
		}

		void f_Connect(CStr const &_Address, FVirtualSocketFactory const &_ClientFactory, uint16 _Port)
		{
			TCSharedPointer<CState> pState = fg_Explicit(this);
			m_ClientActor
				(
					&CWebSocketClientActor::f_Connect
					, _Address
					, ""
					, ENetAddressType_None
					, _Port
					, "/Test"
					, fg_Format("http://{}", _Address)
					, fg_CreateVector<CStr>("Test")
					, NHTTP::CRequest()
					, fg_TempCopy(_ClientFactory)
				)
				> fg_ConcurrentActor() / [pState](TCAsyncResult<CWebSocketNewClientConnection> &&_Result)
				{
					DMibLock(pState->m_Lock);
					if (_Result)
					{
						auto &Result = *_Result;

						Result.m_fOnClose = g_ActorFunctorWeak / [pState](EWebSocketStatus _Status, CStr _Message, EWebSocketCloseOrigin _Origin) -> TCFuture<void>
							{
								DMibLock(pState->m_Lock);
								pState->m_ClientConnectionCloseMessage = _Message;
								pState->m_ClientConnectionCloseStatus = _Status;
								pState->m_ClientConnectionCloseOrigin = _Origin;
								pState->m_Event.f_Signal();

								co_return {};
							}
						;
						Result.m_fOnReceiveTextMessage = g_ActorFunctorWeak / [pState](CStr _Message) -> TCFuture<void>
							{
								DMibLock(pState->m_Lock);
								pState->m_Messages.f_Insert(_Message);
								pState->m_Event.f_Signal();

								co_return {};
							}
						;

						pState->m_ClientSocket = Result.f_Accept
							(
								fg_ConcurrentActor() / [pState](TCAsyncResult<CActorSubscription> &&_Callback)
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

	bool fp_TestConnect(TCSharedPointer<CState> const &_pState, CStr const &_AcceptError, CStr const &_ConnectError)
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

		CClock Clock;
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
			TCFunction<TCTuple<FVirtualSocketFactory, FVirtualSocketFactory> ()> const &_fGetFactories
			, CStr const &_AcceptError
			, CStr const &_ConnectError
			, CStr const &_Address
			, bool _bTestTimeout
			, bool _bTestTooLongCloseMessage
		)
	{
		{
			DMibTestPath("Connection");
			CActorRunLoopTestHelper RunLoopHelper;

			auto Factories = _fGetFactories();
			auto ServerFactory = fg_Get<0>(Factories);
			auto ClientFactory = fg_Get<1>(Factories);

			CNetAddress ListenAddress;
			if (_Address == "localhost")
			{
				CNetAddressTCPv4 Address;
				Address.f_SetLocalhost();
				Address.m_Port = 0;
				ListenAddress = Address;
			}
			else
				ListenAddress = CSocket::fs_ResolveAddress(_Address);
			{
				TCSharedPointer<CState> pState = fg_Construct();
				auto Cleanup
					= g_OnScopeExit / [&]
					{
						pState->f_Clear(RunLoopHelper.m_pRunLoop);
					}
				;

				pState->m_ServerActor = fg_ConstructActor<CWebSocketServerActor>();
				pState->m_ServerActor(&CWebSocketServerActor::f_SetDefaultFragmentationSize, m_CurrentFragmentationSize).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
				auto ListenPort = pState->f_StartListen(ListenAddress, ServerFactory);

				pState->m_ClientActor = fg_ConstructActor<CWebSocketClientActor>();
				pState->m_ClientActor(&CWebSocketClientActor::f_SetDefaultFragmentationSize, m_CurrentFragmentationSize).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
				pState->f_Connect(_Address, ClientFactory, ListenPort);

				if (!fp_TestConnect(pState, _AcceptError, _ConnectError))
					return;
				{
					DMibTestPath("Messages");

					mint nMessages = 0;
					pState->m_ClientSocket(&CWebSocketActor::f_SendText, "TestText", 0).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
					++nMessages;

					CByteVector Buffer = {'T', 'e', 's', 't', 'B', 'u', 'f', 'f'};
					TCSharedPointer<CWebSocketActor::CMaybeSecureByteVector> pMessage = fg_Construct(Buffer);
					pState->m_ClientSocket(&CWebSocketActor::f_SendTextBuffer, pMessage, 0).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
					++nMessages;

					TCSharedPointer<CWebSocketActor::CMessageBuffers> pMessageBuffers = fg_Construct();
					pMessageBuffers->m_Data = Buffer.f_ToSecure();
					pMessageBuffers->m_Markers = {0, 4};
					pState->m_ClientSocket(&CWebSocketActor::f_SendTextBuffers, pMessageBuffers, 0).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
					nMessages += 2;

					CStr BigText;
					for (mint i = 0; i < 1024 * 8 * 2; ++i) // 2 MiB
						BigText += gc_Str<"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF">.m_Str;

					BigText = BigText.f_Left(m_CurrentFragmentationSize * 128);

					pState->m_ClientSocket(&CWebSocketActor::f_SendText, BigText, 0).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
					++nMessages;

					for (mint i = 0; i < 32; ++i)
					{
						CStr Message = BigText.f_Left(i);
						pState->m_ClientSocket(&CWebSocketActor::f_SendText, Message, 0).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
						++nMessages;
					}

					bool bTimedOut = false;
					while (!bTimedOut)
					{
						{
							DMibLock(pState->m_Lock);
							if (pState->m_Messages.f_GetLen() >= nMessages)
								break;
						}
						bTimedOut = pState->m_Event.f_WaitTimeout(20.0);
					}

					DMibExpectFalse(bTimedOut);
					DMibExpect(pState->m_Messages.f_GetLen(), ==, nMessages)(ETest_FailAndStop);
					DMibExpect(pState->m_Messages[0], ==, "TestTextReply");
					DMibExpect(pState->m_Messages[1], ==, "TestBuffReply");
					DMibExpect(pState->m_Messages[2], ==, "TestReply");
					DMibExpect(pState->m_Messages[3], ==, "BuffReply");
					DMibExpect(pState->m_Messages[4], ==, BigText + "Reply")(ETestFlag_NoValues);

					for (mint i = 0; i < 32; ++i)
					{
						CStr Message = BigText.f_Left(i);
						DMibExpect(pState->m_Messages[5 + i], ==, Message + "Reply")(ETestFlag_Aggregated);
					}
				}

				{
					DMibTestPath("Disconnect");

					pState->m_ClientSocket(&CWebSocketActor::f_SendText, _bTestTooLongCloseMessage ? "DisconnectLongClose" : "Disconnect", 0).f_DiscardResult();

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

					auto ExpectedCloseMessage = _bTestTooLongCloseMessage ? gc_CloseMessageTooLong.m_Str.f_Left(CWebSocketActor::mc_MaxCloseMessageLength) : gc_CloseMessage.m_Str;

					DMibTest(!DMibExpr(bTimedOut));
					DMibExpect(pState->m_ClientConnectionCloseMessage, ==, ExpectedCloseMessage);
					DMibExpect(pState->m_ServerConnectionCloseMessage, ==, ExpectedCloseMessage);

				}
			}

			if (_bTestTimeout)
			{
				DMibTestPath("Timeout");
				TCSharedPointer<CState> pState = fg_Construct();
				auto Cleanup
					= g_OnScopeExit / [&]
					{
						pState->f_Clear(RunLoopHelper.m_pRunLoop);
					}
				;

				pState->m_ServerActor = fg_ConstructActor<CWebSocketServerActor>();
				pState->m_ServerActor(&CWebSocketServerActor::f_SetDefaultFragmentationSize, m_CurrentFragmentationSize).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
				pState->m_ServerActor(&CWebSocketServerActor::f_SetDefaultTimeout, 1.0).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
				auto ListenPort = pState->f_StartListen(ListenAddress, ServerFactory);

				pState->m_ClientActor = fg_ConstructActor<CWebSocketClientActor>();
				pState->m_ClientActor(&CWebSocketClientActor::f_SetDefaultFragmentationSize, m_CurrentFragmentationSize).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
				pState->m_ClientActor(&CWebSocketClientActor::f_SetDefaultTimeout, 1.0).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);
				pState->f_Connect(_Address, ClientFactory, ListenPort);

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
					pState->m_ClientSocket(&CWebSocketActor::f_DebugSetFlags, 1.0, ESocketDebugFlag_StopProcessing).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 3);

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

	void fp_TestProtocols(mint _FragmentationSize)
	{
		DMibTestPath("Fragmentation {}"_f << _FragmentationSize);
		m_CurrentFragmentationSize = _FragmentationSize;
		{
			DMibTestPath("TCP");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						return {nullptr, nullptr};
					}
					, ""
					, ""
					, m_CurrentFragmentationSize == CWebsocketSettings::mc_DefaultFragmentationSize
				)
			;
		}

		{
			DMibTestPath("SSL");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions Options;
						Options.m_CommonName = "Malterlib test Self Signed";
						Options.m_Hostnames = fg_CreateVector<CStr>("localhost");
						Options.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(Options, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
					, m_CurrentFragmentationSize == CWebsocketSettings::mc_DefaultFragmentationSize
				)
			;
		}

		if (m_CurrentFragmentationSize != CWebsocketSettings::mc_DefaultFragmentationSize)
			return;

		{
			DMibTestPath("TCP Long Close");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						return {nullptr, nullptr};
					}
					, ""
					, ""
					, false
					, true
				)
			;
		}
		{
			DMibTestPath("SSL Client Certificate");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CByteVector CertificateRequestData;

						CCertificateOptions ClientOptions;
						ClientOptions.m_CommonName = "Test Client";
						ClientOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateClientCertificateRequest(ClientOptions, CertificateRequestData, ClientSettings.m_PrivateKeyData);
						CCertificate::fs_SignClientCertificate(ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData, CertificateRequestData, ClientSettings.m_PublicCertificateData);

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				)
			;
		}
		{
			DMibTestPath("SSL Client Certificate Chain Without Intermediate CA");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

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

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings2);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
					, ""
				)
			;
		}
		{
			DMibTestPath("SSL Client Certificate Incorrect");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CCertificateOptions ClientOptions;
						ClientOptions.m_CommonName = "Test Client";
						ClientOptions.m_KeySetting = gc_TestTestKeySetting;
						ClientOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");

						CCertificate::fs_GenerateSelfSignedCertAndKey(ClientOptions, ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData);

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
					, ""
				)
			;
		}
		{
			DMibTestPath("SSL Client Certificate Missing");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: PEER_DID_NOT_RETURN_A_CERTIFICATE"
					, ""
				)
			;
		}
		{
			DMibTestPath("SSL Client Certificate Allow Missing");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						ServerSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_AllowMissingPeerCertificate;

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				)
			;
		}
		{
			DMibTestPath("SSL Client Certificate Incorrect Allow Missing");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						ServerSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						ServerSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_AllowMissingPeerCertificate;

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;

						CCertificateOptions ClientOptions;
						ClientOptions.m_CommonName = "Test Client";
						ClientOptions.m_KeySetting = gc_TestTestKeySetting;
						ClientOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");

						CCertificate::fs_GenerateSelfSignedCertAndKey(ClientOptions, ClientSettings.m_PublicCertificateData, ClientSettings.m_PrivateKeyData);

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
					, ""
				)
			;
		}
		{
			DMibTestPath("SSL Server Certificate Incorrect");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
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

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_CACertificateData = ServerSettings2.m_PublicCertificateData;

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
				)
			;
		}
		{
			DMibTestPath("SSL Server Certificate Self Signed");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
					{
						CSSLSettings ServerSettings;

						CCertificateOptions ServerOptions;
						ServerOptions.m_CommonName = "Malterlib test Self Signed";
						ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
						ServerOptions.m_KeySetting = gc_TestTestKeySetting;

						CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseOSStoreIfNoCASpecified;
						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, "Socket closed: The certificate is self signed and cannot be found in the list of trusted certificates"
				)
			;
		}
		{
			DMibTestPath("SSL Server Certificate Child Cert");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
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

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_CACertificateData = RootCertData;

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, ""
				)
			;
		}
		{
			DMibTestPath("SSL Server Certificate Incorrect Specific");
			fp_Test
				(
					[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
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

						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = RootCertData;

						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
					, ""
					, "Socket closed: Mismatching specific certificate"
				)
			;
		}
	}

	void f_DoTests()
	{
		DMibTestSuite("Tests")
		{
			for (mint i = 1; i < 16; ++i)
				fp_TestProtocols(i);

			for (mint i = 32; i <= CWebsocketSettings::mc_DefaultFragmentationSize; i = i * 2)
				fp_TestProtocols(i);
		};

		DMibTestSuite("AutobahnClient" << CTestGroup("Manual")) -> TCFuture<void>
		{
			/* https://github.com/crossbario/autobahn-testsuite

			Install test suite

				rm -rf ~/wstest
				rm -rf ~/Library/Caches/pip
				export BREW_HOME=`brew --prefix`
				brew install pyenv
				brew install openssl@1.1 --force
				pyenv install 2.7.18 -f
				pyenv global 2.7.18
				PATH="$(pyenv root)/shims:$PATH"
				which python
				pip install virtualenv
				virtualenv ~/wstest
				source ~/wstest/bin/activate
				python --version
				pip install typing
				CPPFLAGS="-I${BREW_HOME}/opt/openssl@1.1/include" CFLAGS="-I${BREW_HOME}/opt/openssl@1.1/include" pip install autobahntestsuite

			To test client

				In tab 1
					source ~/wstest/bin/activate
					cd ~/wstest
					mkdir MalterlibClient
					cd MalterlibClient
					wstest -m fuzzingserver

				In tab 2
					cd /opt/Deploy/Tests
					./Test_Malterlib_Web -g Manual --logs
					open ~/wstest/MalterlibClient/reports/clients/index.html

			To test server

				Enable Malterlib_App_Enable_WebSocketEcho in user settings
				Build Apps_Malterlib_Web

				In tab 1
					cd /opt/Deploy/Malterlib_Web
					./WebSocketEcho --daemon-run-debug --log-to-stderr

				In tab 2
					source ~/wstest/bin/activate
					cd ~/wstest
					mkdir MalterlibServer
					cd MalterlibServer
					wstest -m fuzzingclient
					open ~/wstest/MalterlibServer/reports/servers/index.html
			*/

			struct CConnection
			{
				TCActor<CWebSocketActor> m_WebSocket;
				CActorSubscription m_Subscription;
			};

			struct CState
			{
				TCActor<CWebSocketClientActor> m_WebSocketClient = fg_Construct();
				TCMap<mint, CConnection> m_Connections;
				mint m_SocketID = 0;
			};

			TCSharedPointer<CState> pState = fg_Construct();

			auto fOpenConnection = [](TCSharedPointer<CState> _pState, CStr _Path, TCActorFunctor<TCFuture<void> (CStr _Text)> _fOnMessage = {}) -> TCUnsafeFuture<TCFuture<void>>
				{
					auto SocketID = _pState->m_SocketID++;

					NHTTP::CRequest Request;
					Request.f_GetRequestFields().f_SetUserAgent("MalterlibWebSocket");
					auto NewConnection = co_await _pState->m_WebSocketClient
						(
							&CWebSocketClientActor::f_Connect
							, "127.0.0.1:9001"
							, ""
							, ENetAddressType_None
							, 9001
							, _Path
							, "http://127.0.0.1/"
							, fg_CreateVector<CStr>()
							, fg_Move(Request)
							, FVirtualSocketFactory()
						)
					;

					auto Address = NewConnection.m_PeerAddress;

					NewConnection.m_fOnReceiveBinaryMessage = g_ActorFunctorWeak / [_pState, SocketID, Address](TCSharedPointer<CSecureByteVector> _pMessage) -> TCFuture<void>
						{
							DMibLog(Info, "{} Binary '{}': {}", SocketID, Address, _pMessage->f_GetLen());
							auto *pClient = _pState->m_Connections.f_FindEqual(SocketID);
							if (pClient)
								co_await pClient->m_WebSocket(&CWebSocketActor::f_SendBinary, _pMessage, 0);

							co_return {};
						}
					;
					NewConnection.m_fOnReceiveTextMessage = g_ActorFunctorWeak / [_pState, SocketID, Address, fOnMessage = fg_Move(_fOnMessage)](CStr _Message) -> TCFuture<void>
						{
							DMibLog(Info, "{} Text '{}': {}", SocketID, Address, _Message.f_GetLen());

							if (fOnMessage)
								co_await fOnMessage(_Message);
							else
							{
								auto *pClient = _pState->m_Connections.f_FindEqual(SocketID);
								if (pClient)
									co_await pClient->m_WebSocket(&CWebSocketActor::f_SendText, _Message, 0);
							}

							co_return {};
						}
					;
					NewConnection.m_fOnReceivePing = g_ActorFunctorWeak / [_pState, SocketID, Address](TCSharedPointer<CSecureByteVector> _ApplicationData) -> TCFuture<void>
						{
							DMibLog(Info, "{} Ping '{}': {}", SocketID, Address, _ApplicationData->f_GetLen());
							auto *pClient = _pState->m_Connections.f_FindEqual(SocketID);
							if (pClient)
								co_await pClient->m_WebSocket(&CWebSocketActor::f_SendPong, _ApplicationData);

							co_return {};
						}
					;
					NewConnection.m_fOnReceivePong = g_ActorFunctorWeak / [SocketID, Address](TCSharedPointer<CSecureByteVector> _ApplicationData) -> TCFuture<void>
						{
							DMibLog(Info, "{} Pong '{}': {}", SocketID, Address, _ApplicationData->f_GetLen());
							co_return {};
						}
					;

					TCPromise<void> ClosedPromise;
					NewConnection.m_fOnClose = g_ActorFunctorWeak / [_pState, SocketID, Address, ClosedPromise](EWebSocketStatus _Reason, CStr _Message, EWebSocketCloseOrigin _Origin) -> TCFuture<void>
						{
							DMibLog(Info, "{} Close '{}': {}", SocketID, Address, _Message);
							auto *pClient = _pState->m_Connections.f_FindEqual(SocketID);
							if (pClient)
							{
								TCFutureVector<void> Destroys;

								if (pClient->m_Subscription)
									pClient->m_Subscription->f_Destroy() > Destroys;

								fg_Move(pClient->m_WebSocket).f_Destroy() > Destroys;

								_pState->m_Connections.f_Remove(pClient);

								co_await fg_AllDoneWrapped(Destroys);
							}

							ClosedPromise.f_SetResult();

							co_return {};
						}
					;

					TCPromise<void> AcceptedPromise;
					auto WebSocket = NewConnection.f_Accept
						(
							fg_CurrentActor() / [_pState, SocketID, Address, AcceptedPromise](TCAsyncResult<CActorSubscription> &&_Subscription)
							{
								if (!_Subscription)
								{
									DMibLog(Info, "{} Failed to accept connection '{}': {}", SocketID, Address, _Subscription.f_GetExceptionStr());
									_pState->m_Connections.f_Remove(SocketID);
									AcceptedPromise.f_SetException(_Subscription.f_GetException());
									return;
								}

								auto *pClient = _pState->m_Connections.f_FindEqual(SocketID);
								if (pClient)
								{
									DMibLog(Info, "{} Accepted connection '{}'", SocketID, Address);
									pClient->m_Subscription = fg_Move(*_Subscription);
								}

								AcceptedPromise.f_SetResult();
							}
						)
					;

					_pState->m_Connections[SocketID].m_WebSocket = WebSocket;

					co_await AcceptedPromise.f_Future();

					co_return ClosedPromise.f_Future();
				}
			;

			mint nCases = 0;
			co_await co_await fOpenConnection
				(
					pState
					, "/getCaseCount"
					, g_ActorFunctor / [&](CStr _Message) -> TCFuture<void>
					{
						DMibLog(Info, "Case count message: {}", _Message);
						nCases = _Message.f_ToInt(0);

						co_return {};
					}
				)
			;

			for (mint i = 0; i < nCases; ++i)
				co_await co_await fOpenConnection(pState, "/runCase?case={}&agent=MalterlibWebSocket"_f << (i + 1));

			co_await co_await fOpenConnection(pState, "/updateReports?agent=MalterlibWebSocket");

			co_return {};
		};
	}

	mint m_CurrentFragmentationSize = CWebsocketSettings::mc_DefaultFragmentationSize;
};

DMibTestRegister(CWebsocket_Tests, Malterlib::Web);
