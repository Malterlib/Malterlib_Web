
#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Test/Exception>
#include <Mib/Web/DDPServer>
#include <Mib/Web/DDPClient>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Encoding/EJSON>
#include <Mib/Cryptography/Certificate>
#include <Mib/Concurrency/DistributedActorTestHelpers>

using namespace NMib;
using namespace NMib::NAtomic;
using namespace NMib::NConcurrency;
using namespace NMib::NContainer;
using namespace NMib::NCryptography;
using namespace NMib::NEncoding;
using namespace NMib::NException;
using namespace NMib::NFunction;
using namespace NMib::NNetwork;
using namespace NMib::NStorage;
using namespace NMib::NStr;
using namespace NMib::NTest;
using namespace NMib::NTime;
using namespace NMib::NThread;
using namespace NMib::NWeb;

static fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;

class CDDP_Tests : public CTest
{
public:

	struct CServer : public CActor
	{
		CServer(FVirtualSocketFactory const &_ServerFactory)
			: m_ServerFactory(_ServerFactory)
		{
			auto &Collection = m_Data["testCollection"];

			for (mint i = 0; i < 10; ++i)
			{
				CStr DocumentID = fg_Format("id{}", i);
				auto &Document = Collection[DocumentID];
				Document["_id"] = DocumentID;
				Document["Value"] = fg_Format("Value{}", i);
			}
		}

		CServer()
		{
		}

		TCFuture<void> fp_Destroy() override
		{
			if (m_WebsocketServer)
				co_await fg_Move(m_WebsocketServer).f_Destroy();

			co_return {};
		}

		FVirtualSocketFactory m_ServerFactory;
		TCActor<CWebSocketServerActor> m_WebsocketServer;

		mutable CMutual m_Lock;
		CActorSubscription m_ListenCallbackReference;
		uint16 m_ListenPort = 0;

		uint16 f_GetListenPort() const
		{
			DMibLock(m_Lock);
			return m_ListenPort;
		}

		CEventAutoReset m_Event;
		CStr m_Error;

		TCMap<CStr, TCMap<CStr, CEJSONSorted>> m_Data;

		TCAtomic<mint> m_nUnsubscribe;

		CStr f_GetError() const
		{
			DMibLock(m_Lock);
			return m_Error;
		}

		void f_ReportError(CStr const &_Error)
		{
			{
				DMibLock(m_Lock);
				fg_AddStrSep(m_Error, _Error, "\n");
			}
			m_Event.f_Signal();
		}

		struct CConnection
		{
			~CConnection()
			{
				*m_pDeleted = true;
			}

			TCActor<CDDPServerConnection> m_Connection;
			CActorSubscription m_Callback;
			TCSharedPointer<bool> m_pDeleted = fg_Construct(false);
		};

		TCLinkedList<CConnection> m_Connections;

		CEJSONSorted fp_MethodError(CStr const &_Error, CStr const &_Reason, CStr const &_Details = "")
		{
			CEJSONSorted Ret;
			Ret["error"] = _Error;
			if (!_Reason.f_IsEmpty())
				Ret["reason"] = _Reason;
			if (!_Details.f_IsEmpty())
				Ret["details"] = _Details;
			return Ret;
		}

		TCFuture<CServer *> f_Start()
		{
			TCPromise<CServer *> Promise;
			m_WebsocketServer = fg_ConstructActor<CWebSocketServerActor>();

			CNetAddressTCPv4 ToListenTo;
			ToListenTo.m_Port = 0;
			ToListenTo.f_SetLocalhost();

			m_WebsocketServer
				(
					&CWebSocketServerActor::f_StartListenAddress
					, TCVector<CNetAddress>{ToListenTo}
					, ENetFlag_None
					, g_ActorFunctorWeak / [this](CWebSocketNewServerConnection _ConnectionInfo)
					-> TCFuture<void>
					{
						auto &NewConnection = m_Connections.f_Insert();
						NewConnection.m_Connection = fg_ConstructActor<CDDPServerConnection>(fg_Move(_ConnectionInfo), CDDPServerConnection::EConnectionType_WebSocket);

						auto pDeleted = NewConnection.m_pDeleted;

						auto pConnection = &NewConnection;
						auto Subscription = co_await NewConnection.m_Connection
							(
								&CDDPServerConnection::f_Register
								, g_ActorFunctorWeak / [](CDDPServerConnection::CConnectionInfo _ConnectionInfo) -> TCFuture<void>
								// On connection
								{
									_ConnectionInfo.f_Accept(CStr()); // Empty sessions means use random ID

									co_return {};
								}
								, g_ActorFunctorWeak / [this, pConnection](CDDPServerConnection::CMethodInfo _MethodInfo) -> TCFuture<void>
								// On method call
								{
									if (_MethodInfo.m_Name == "login")
									{
										try
										{
											auto &LoginParams = _MethodInfo.m_Parameters[0];
											CStr Email = LoginParams["user"]["email"].f_AsString();
											CStr Algo = LoginParams["password"]["algorithm"].f_AsString();
											if (Algo != "sha-256")
											{
												_MethodInfo.f_Error(fp_MethodError("unknown-authentication-algorithm", fg_Format("Unknown algorithm: {}", Algo)));
												co_return {};
											}

											CStr PasswordDigest = LoginParams["password"]["digest"].f_AsString();
											CStr Password = "testpass";

											CStr RightDigest = CHash_SHA256::fs_DigestFromData((uint8 const *)Password.f_GetStr(), Password.f_GetLen()).f_GetString();

											if (PasswordDigest != RightDigest)
											{
												_MethodInfo.f_Error(fp_MethodError("invalid-password", "Invalid password"));
												co_return {};
											}
											CEJSONSorted Result;

											Result["id"] = "testuserid";
											Result["token"] = "testusertoken";
											Result["tokenExpires"] = CTime::fs_NowUTC() + CTimeSpanConvert::fs_CreateSpan(0, 3);

											_MethodInfo.f_Result(Result);
										}
										catch (CException const &_Exception)
										{
											_MethodInfo.f_Error(fp_MethodError("exception-logging-in", _Exception.f_GetErrorStr()));
										}
									}
									else if (_MethodInfo.m_Name == "testChanged")
									{
										TCVector<CDDPServerConnection::CChange> Changes;

										auto &Collection = m_Data["testCollection"];
										auto &ToChange = *Collection.f_FindSmallest();
										CStr DocumentID = Collection.fs_GetKey(ToChange);

										ToChange["Value2"] = "55";

										CDDPServerConnection::CChanged Changed;
										Changed.m_DocumentID = DocumentID;
										Changed.m_Fields["Value2"] = "55";
										Changed.m_Collection = "testCollection";

										Changes.f_Insert(fg_Move(Changed));

										CDDPServerConnection::CUpdated Updated;
										Updated.m_IDs.f_Insert(_MethodInfo.m_ID);

										Changes.f_Insert(fg_Move(Updated));

										_MethodInfo.f_Result(CEJSONSorted(EJSONType_Object));

										pConnection->m_Connection(&CDDPServerConnection::f_SendChanges, fg_Move(Changes)).f_DiscardResult();
									}
									else if (_MethodInfo.m_Name == "testRemoved")
									{
										TCVector<CDDPServerConnection::CChange> Changes;

										auto &Collection = m_Data["testCollection"];
										auto &ToChange = *Collection.f_FindSmallest();
										CStr DocumentID = Collection.fs_GetKey(ToChange);

										Collection.f_Remove(&ToChange);

										CDDPServerConnection::CRemoved Removed;
										Removed.m_DocumentID = DocumentID;
										Removed.m_Collection = "testCollection";

										Changes.f_Insert(fg_Move(Removed));

										_MethodInfo.f_Result(CEJSONSorted(EJSONType_Object));

										pConnection->m_Connection(&CDDPServerConnection::f_SendChanges, fg_Move(Changes)).f_DiscardResult();
									}
									else
										_MethodInfo.f_Error(fp_MethodError("method-not-found", "Method not found"));

									co_return {};
								}
								, g_ActorFunctorWeak / [this, pConnection](CDDPServerConnection::CSubscribeInfo _SubscribeInfo)
								-> TCFuture<void>
								// On subscribe
								{
									//DMibTrace("Subscription: {}\n", _SubscribeInfo.m_Name);
									if (_SubscribeInfo.m_Name == "testSub")
									{
										TCVector<CDDPServerConnection::CChange> Changes;
										for (auto iCollection = m_Data.f_GetIterator(); iCollection; ++iCollection)
										{
											for (auto iDocument = iCollection->f_GetIterator(); iDocument; ++iDocument)
											{
												CDDPServerConnection::CAdded Added;
												Added.m_Collection = iCollection.f_GetKey();
												Added.m_DocumentID = iDocument.f_GetKey();
												Added.m_Fields = *iDocument;
												Changes.f_Insert(fg_Move(Added));
											}
										}
										CDDPServerConnection::CReady Ready;
										Ready.m_Subscriptions.f_Insert(_SubscribeInfo.m_ID);
										Changes.f_Insert(fg_Move(Ready));
										pConnection->m_Connection(&CDDPServerConnection::f_SendChanges, fg_Move(Changes)).f_DiscardResult();
									}
									else
									{
										_SubscribeInfo.f_Error(CEJSONSorted());
									}

									co_return {};
								}
								, g_ActorFunctorWeak / [this](CStr _SubscriptionID)
								-> TCFuture<void>
								// On unsubscribe
								{
									++m_nUnsubscribe;
									m_Event.f_Signal();

									co_return {};
								}
								, g_ActorFunctorWeak / [this](CStr _Error)
								-> TCFuture<void>
								// On error
								{
									f_ReportError(_Error);

									co_return {};
								}
								, g_ActorFunctorWeak / [](EWebSocketStatus _Reason, CStr _Message, EWebSocketCloseOrigin _Origin)
								-> TCFuture<void>
								{
									co_return {};
								}
							)
						;

						if (*pDeleted)
							co_return DMibErrorInstance("Connection deleted");

						pConnection->m_Callback = fg_Move(Subscription);

						co_return {};
					}
					, g_ActorFunctorWeak / [this](CWebSocketActor::CConnectionInfo _ConnectionInfo)
					-> TCFuture<void>
					{
						f_ReportError(fg_Format("Rejected connection: {}", _ConnectionInfo.m_Error));

						co_return {};
					}
					, fg_TempCopy(m_ServerFactory)
				)
				> [this, Promise](TCAsyncResult<CWebSocketServerActor::CListenResult> &&_Result)
				{
					if (_Result)
					{
						m_ListenCallbackReference = fg_Move(_Result->m_Subscription);
						{
							DMibLock(m_Lock);
							m_ListenPort = _Result->m_ListenPorts[0];
						}
						m_Event.f_Signal();
						Promise.f_SetResult(this);
					}
					else
					{
						f_ReportError(_Result.f_GetExceptionStr());
						Promise.f_SetException(_Result);
					}
				}
			;

			co_return co_await Promise.f_MoveFuture();
		}
	};

	void fp_Test(TCFunction<TCTuple<FVirtualSocketFactory, FVirtualSocketFactory> ()> const &_fGetFactories)
	{
		DMibTestPath("Connection");
		CActorRunLoopTestHelper RunLoopHelper;

		auto Factories = _fGetFactories();
		auto ServerFactory = fg_Get<0>(Factories);
		auto ClientFactory = fg_Get<1>(Factories);
		TCActor<CServer> Server = fg_ConstructActor<CServer>(ServerFactory);

		auto Cleanup = g_OnScopeExit / [&]
			{
				Server->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
			}
		;

		TCActor<CActor> ProcessingActor = fg_Construct();

		auto &ServerInternal = *(Server(&CServer::f_Start).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout));

		CStr ConnectToURLString;
		if (ServerFactory)
			ConnectToURLString = "wss://localhost:{}/Test"_f << ServerInternal.f_GetListenPort();
		else
			ConnectToURLString = "ws://localhost:{}/Test"_f << ServerInternal.f_GetListenPort();

		TCActor<CDDPClient> Client = fg_ConstructActor<CDDPClient>(ConnectToURLString, "", fg_Default(), "", ClientFactory);

		CDDPClient::CConnectInfo ConnectionInfo = Client(&CDDPClient::f_Connect, "testuser", "testpass", "", "", 20.0, nullptr).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		DMibAssert(ConnectionInfo.m_UserID, ==, "testuserid");

		struct CState
		{
			CEventAutoReset m_Event;
			TCAtomic<mint> m_nReady{0};
			TCAtomic<mint> m_nError{0};
			TCAtomic<mint> m_nAdded{0};
			TCAtomic<mint> m_nChanged{0};
			TCAtomic<mint> m_nRemoved{0};
		};

		TCSharedPointer<CState> pState = fg_Construct();

		{
			CActorSubscription Observation = Client
				(
					&CDDPClient::f_Observe
					, "testCollection"
					, CDDPClient::EObserveNotification_Added
					| CDDPClient::EObserveNotification_Changed
					| CDDPClient::EObserveNotification_Removed
					, g_ActorFunctorWeak(ProcessingActor) / [pState](CDDPClient::EObserveNotification _Notification, CEJSONSorted _NotificationData)
					-> TCFuture<void>
					{
						if (_Notification & CDDPClient::EObserveNotification_Added)
							++pState->m_nAdded;
						if (_Notification & CDDPClient::EObserveNotification_Changed)
							++pState->m_nChanged;
						if (_Notification & CDDPClient::EObserveNotification_Removed)
							++pState->m_nRemoved;

						pState->m_Event.f_Signal();

						co_return {};
					}
				).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 6)
			;

			CActorSubscription Subscription = Client
				(
					&CDDPClient::f_Subscribe
					, "testSub"
					, ""
					, CEJSONSorted(fg_CreateVector<CEJSONSorted>())
					, CDDPClient::ESubscriptionNotification_Ready
					| CDDPClient::ESubscriptionNotification_Error
					, g_ActorFunctorWeak(ProcessingActor) / [pState](CDDPClient::ESubscriptionNotification _Notification, CEJSONSorted _NotificationData)
					-> TCFuture<void>
					{
						if (_Notification & CDDPClient::ESubscriptionNotification_Ready)
							++pState->m_nReady;
						if (_Notification & CDDPClient::ESubscriptionNotification_Error)
							++pState->m_nError;

						pState->m_Event.f_Signal();

						co_return {};
					}
					, true
				).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 6)
			;

			CClock Timeout;
			Timeout.f_Start();

			while (!pState->m_nReady.f_Load())
			{
				if (Timeout.f_GetTime() >= g_Timeout / 2)
					break;
				pState->m_Event.f_WaitTimeout(1.0);
			}

			DMibAssert(pState->m_nReady.f_Load(), ==, 1);
			DMibAssert(pState->m_nAdded.f_Load(), ==, 10);

			auto fGetDocuments = [&]() -> TCMap<CStr, CEJSONSorted>
				{
					TCMap<CStr, CEJSONSorted> Documents;
					CMutual Lock;

					Client
						(
							&CDDPClient::f_AccessData
							, [&Documents, &Lock] (CDDPClient::CDataAccessor const &_Accessor)
							{
								try
								{
									auto const& Collection = _Accessor.f_GetCollection("testCollection");
									DMibLock(Lock);
									for (auto iDocument = Collection.f_GetDocumentIterator(); iDocument; ++iDocument)
										Documents[iDocument.f_GetKey()] = *iDocument;
								}
								catch (CException const &_Exception)
								{
									(void)_Exception;
								}
							}
						)
						.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 6)
					;
					return Documents;
				}
			;

			// Test document accessor
			{
				TCMap<CStr, CEJSONSorted> Documents = fGetDocuments();

				DMibAssert(Documents.f_GetLen(), ==, 10);

				for (auto const& Document : Documents)
				{
					auto pID = Document.f_GetMember("_id");
					DMibAssert(pID, !=, nullptr)(ETestFlag_Aggregated);
					DMibAssert(pID->f_IsString(), ==, true)(ETestFlag_Aggregated);

					auto pValue = Document.f_GetMember("Value");
					DMibAssert(pValue, !=, nullptr)(ETestFlag_Aggregated);
					DMibAssert(pValue->f_IsString(), ==, true)(ETestFlag_Aggregated);

					CStr StrippedID = pID->f_AsString().f_Replace("id", "");
					CStr StrippedValue = pValue->f_AsString().f_Replace("Value", "");
					DMibExpect(StrippedID, ==, StrippedValue)(ETestFlag_Aggregated);
				}
				DMibExpect(Documents, ==, ServerInternal.m_Data["testCollection"]);
			}

			// Test no sub
			{
				auto fSubscribe = [&]
					{
						CActorSubscription Subscription = Client
							(
								&CDDPClient::f_Subscribe
								, "testFalseSub"
								, ""
								, CEJSONSorted(fg_CreateVector<CEJSONSorted>())
								, CDDPClient::ESubscriptionNotification_None
								, g_ActorFunctorWeak(ProcessingActor)
								/ [](CDDPClient::ESubscriptionNotification _Notification, CEJSONSorted _NotificationData)
								-> TCFuture<void>
								{
									co_return {};
								}
								, true
							).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout / 6)
						;
					}
				;
				DMibExpectException(fSubscribe(), DMibErrorInstance("sub-not-found: Subscription not found"));
			}

			DMibExpectException
				(
					(Client(&CDDPClient::f_Method, "testNoMethod", fg_CreateVector<CEJSONSorted>()).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout))
					, DMibErrorInstance("method-not-found: Method not found")
				)
			;

			DMibExpect(pState->m_nChanged, ==, 0);
			Client(&CDDPClient::f_Method, "testChanged", fg_CreateVector<CEJSONSorted>()).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			Timeout.f_Start();
			while (!pState->m_nChanged.f_Load())
			{
				if (Timeout.f_GetTime() >= g_Timeout / 6)
					break;
				pState->m_Event.f_WaitTimeout(1.0);
			}
			DMibExpect(pState->m_nChanged, ==, 1);

			auto DocumentsAfterChanged = fGetDocuments();
			DMibExpect(DocumentsAfterChanged, ==, ServerInternal.m_Data["testCollection"]);

			DMibExpect(pState->m_nRemoved, ==, 0);
			Client(&CDDPClient::f_Method, "testRemoved", fg_CreateVector<CEJSONSorted>()).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			Timeout.f_Start();
			while (!pState->m_nRemoved.f_Load())
			{
				if (Timeout.f_GetTime() >= g_Timeout / 6)
					break;
				pState->m_Event.f_WaitTimeout(1.0);
			}
			DMibExpect(pState->m_nRemoved, ==, 1);

			auto DocumentsAfterRemoved = fGetDocuments();
			DMibExpect(DocumentsAfterRemoved, ==, ServerInternal.m_Data["testCollection"]);

			DMibExpect(ServerInternal.m_nUnsubscribe.f_Load(), ==, 0);
		}
		{
			CClock Timeout;
			Timeout.f_Start();

			while (ServerInternal.m_nUnsubscribe.f_Load() == 0)
			{
				if (Timeout.f_GetTime() >= g_Timeout / 6)
					break;
				ServerInternal.m_Event.f_WaitTimeout(1.0);
			}

			DMibExpect(ServerInternal.m_nUnsubscribe.f_Load(), ==, 1);
		}
		
		DMibExpect(ServerInternal.f_GetError(), ==, "");
	}

	void f_DoTests()
	{
		DMibTestSuite("Tests")
		{
			{
				DMibTestPath("TCP");
				fp_Test
					(
						[]() -> TCTuple<FVirtualSocketFactory, FVirtualSocketFactory>
						{
							return {nullptr, nullptr};
						}
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

							CCertificateOptions ServerOptions;
							ServerOptions.m_CommonName = "Malterlib test Self Signed";
							ServerOptions.m_Hostnames = fg_CreateVector<CStr>("localhost");
							ServerOptions.m_KeySetting = CPublicKeySettings_EC_secp256r1{};

							CCertificate::fs_GenerateSelfSignedCertAndKey(ServerOptions, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);

							TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

							CSSLSettings ClientSettings;
							ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
							ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
							TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

							return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
						}
					)
				;
			};
		};
	}
};

DMibTestRegister(CDDP_Tests, Malterlib::Web);
