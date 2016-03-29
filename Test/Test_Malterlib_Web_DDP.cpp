
#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Test/Exception>
#include <Mib/Web/DDPServer>
#include <Mib/Web/DDPClient>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Encoding/EJSON>

using namespace NMib::NWeb;
using namespace NMib::NNet;
using namespace NMib;
using namespace NMib::NTest;
using namespace NMib::NAtomic;
using namespace NMib::NThread;
using namespace NMib::NContainer;
using namespace NMib::NConcurrency;
using namespace NMib::NStr;
using namespace NMib::NEncoding;
using namespace NMib::NTime;

class CDDP_Tests : public NMib::NTest::CTest
{
public:
	
	struct CServer : public CActor
	{
		CServer(NNet::FVirtualSocketFactory const &_ServerFactory)
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

		NNet::FVirtualSocketFactory m_ServerFactory;
		TCActor<CWebSocketServerActor> m_WebsocketServer;

		CMutual m_Lock;
		CActorCallback m_ListenCallbackReference;
		
		CEventAutoReset m_Event;
		CStr m_Error;
		
		TCMap<CStr, TCMap<CStr, CEJSON>> m_Data;
		
		TCAtomic<mint> m_nUnsubscribe;
		
		void f_ReportError(CStr const &_Error)
		{
			{
				DMibLock(m_Lock);
				m_Error = _Error;
			}
			m_Event.f_Signal();
		}
		
		struct CConnection
		{
			TCActor<CDDPServerConnection> m_Connection;
			CActorCallback m_Callback;
		};
		
		TCLinkedList<CConnection> m_Connections;
		
		CEJSON fp_MethodError(CStr const &_Error, CStr const &_Reason, CStr const &_Details = "")
		{
			CEJSON Ret;
			Ret["error"] = _Error;
			if (!_Reason.f_IsEmpty())
				Ret["reason"] = _Reason;
			if (!_Details.f_IsEmpty())
				Ret["details"] = _Details;
			return Ret;
		}
		
		TCContinuation<CServer *> f_Start()
		{
			TCContinuation<CServer *> Continuation;
			m_WebsocketServer = fg_ConstructActor<CWebSocketServerActor>();
			
			m_WebsocketServer
				(
					&CWebSocketServerActor::f_StartListen
					, 10501
					, 1
					, fg_ConcurrentActor()
					, [this](CWebSocketNewServerConnection &&_ConnectionInfo)
					{
						auto &NewConnection = m_Connections.f_Insert();
						NewConnection.m_Connection = fg_ConstructActor<CDDPServerConnection>(fg_Move(_ConnectionInfo), CDDPServerConnection::EConnectionType_WebSocket);

						auto pConnection = &NewConnection;
						NewConnection.m_Connection
							(
								&CDDPServerConnection::f_Register
								, fg_ThisActor(this)
								, [this](CDDPServerConnection::CConnectionInfo const &_ConnectionInfo) // On connection
								{
									_ConnectionInfo.f_Accept(CStr()); // Empty sessions means use random ID
								}
								, [this, pConnection](CDDPServerConnection::CMethodInfo const &_MethodInfo) // On method call
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
												return;
											}
											
											CStr PasswordDigest = LoginParams["password"]["digest"].f_AsString();
											CStr Password = "testpass";
											
											CStr RightDigest = NDataProcessing::CHash_SHA256::fs_DigestFromData((uint8 const *)Password.f_GetStr(), Password.f_GetLen()).f_GetString();
											
											if (PasswordDigest != RightDigest)
											{
												_MethodInfo.f_Error(fp_MethodError("invalid-password", "Invalid password"));
												return;
											}
											CEJSON Result;
											
											Result["id"] = "testuserid";
											Result["token"] = "testusertoken";
											Result["tokenExpires"] = CTime::fs_NowUTC() + CTimeSpanConvert::fs_CreateSpan(0, 3);
											
											_MethodInfo.f_Result(Result);
										}
										catch (NException::CException const &_Exception)
										{
											_MethodInfo.f_Error(fp_MethodError("excption-logging-in", _Exception.f_GetErrorStr()));
										}
									}
									else if (_MethodInfo.m_Name == "testChanged")
									{
										NContainer::TCVector<CDDPServerConnection::CChange> Changes;
										
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
										
										_MethodInfo.f_Result(CEJSON(EJSONType_Object));
										
										pConnection->m_Connection(&CDDPServerConnection::f_SendChanges, fg_Move(Changes)) > fg_DiscardResult();
									}
									else if (_MethodInfo.m_Name == "testRemoved")
									{
										NContainer::TCVector<CDDPServerConnection::CChange> Changes;
										
										auto &Collection = m_Data["testCollection"];
										auto &ToChange = *Collection.f_FindSmallest();
										CStr DocumentID = Collection.fs_GetKey(ToChange);
										
										Collection.f_Remove(&ToChange);
										
										CDDPServerConnection::CRemoved Removed;
										Removed.m_DocumentID = DocumentID;
										Removed.m_Collection = "testCollection";

										Changes.f_Insert(fg_Move(Removed));

										_MethodInfo.f_Result(CEJSON(EJSONType_Object));
										
										pConnection->m_Connection(&CDDPServerConnection::f_SendChanges, fg_Move(Changes)) > fg_DiscardResult();
									}
									else
										_MethodInfo.f_Error(fp_MethodError("method-not-found", "Method not found"));
								}
								, [this, pConnection](CDDPServerConnection::CSubscribeInfo const &_SubscribeInfo) // On subscribe
								{
									//DMibTrace("Subscription: {}\n", _SubscribeInfo.m_Name);
									if (_SubscribeInfo.m_Name == "testSub")
									{
										NContainer::TCVector<CDDPServerConnection::CChange> Changes;
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
										pConnection->m_Connection(&CDDPServerConnection::f_SendChanges, fg_Move(Changes)) > fg_DiscardResult();
									}
									else
									{
										_SubscribeInfo.f_Error(CEJSON());
									}
								}
								, [this](NStr::CStr const &_SubscriptionID) // On unsubscribe
								{
									++m_nUnsubscribe;
									m_Event.f_Signal();
								}
								, [this](NStr::CStr const &_Error) // On error
								{
									f_ReportError(_Error);
								}
								, [this](EWebSocketStatus _Reason, NStr::CStr const& _Message, EWebSocketCloseOrigin _Origin)
								{
								}
							)
							> [pConnection](TCAsyncResult<CActorCallback> &&_Callback)
							{
								if (_Callback)
									pConnection->m_Callback = fg_Move(*_Callback);
							}
						;

					}
					, [this](CWebSocketActor::CConnectionInfo && _ConnectionInfo)
					{
						f_ReportError(fg_Format("Rejected connection: {}", _ConnectionInfo.m_Error));
					}
					, fg_TempCopy(m_ServerFactory)
				)
				> fg_ConcurrentActor() / [this, Continuation](NConcurrency::TCAsyncResult<CActorCallback> &&_Result)
				{
					DMibLock(m_Lock);
					if (_Result)
					{
						m_ListenCallbackReference = fg_Move(*_Result);
						m_Event.f_Signal();
						Continuation.f_SetResult(this);
					}
					else
					{
						f_ReportError(_Result.f_GetExceptionStr());
						Continuation.f_SetException(_Result);
					}
				}
			;
			
			return Continuation;
		}
	};
	
	void fp_Test(NFunction::TCFunction<NContainer::TCTuple<NNet::FVirtualSocketFactory, NNet::FVirtualSocketFactory> ()> const &_fGetFactories)
	{
		DMibTestSuite("Connection")
		{
			auto Factories = _fGetFactories();
			auto ServerFactory = fg_Get<0>(Factories); 
 			auto ClientFactory = fg_Get<1>(Factories); 
			TCActor<CServer> Server = fg_ConstructActor<CServer>(ServerFactory);
			
			auto &ServerInternal = *(Server(&CServer::f_Start).f_CallSync());
			
			CStr ConnectToURLString;
			if (ServerFactory)
				ConnectToURLString = "wss://localhost:10501/Test";
			else
				ConnectToURLString = "ws://localhost:10501/Test";
			
			TCActor<CDDPClient> Client = fg_ConstructActor<CDDPClient>(ConnectToURLString, "", fg_Default(), "", ClientFactory);
			
			CDDPClient::CConnectInfo ConnectionInfo = Client(&CDDPClient::f_Connect, "testuser", "testpass", "", "", 20.0, nullptr, nullptr).f_CallSync();
			
			DMibAssert(ConnectionInfo.m_UserID, ==, "testuserid");
			
			CEventAutoReset Event;
			TCAtomic<mint> nReady{false};
			TCAtomic<mint> nError{false};
			TCAtomic<mint> nAdded{false};
			TCAtomic<mint> nChanged{false};
			TCAtomic<mint> nRemoved{false};

			{
				auto &ConcurrentActor = fg_ConcurrentActor(); 
				CActorCallback Observation = Client
					(
						&CDDPClient::f_Observe
						, ConcurrentActor
						, "testCollection"
						, CDDPClient::EObserveNotification_Added
						| CDDPClient::EObserveNotification_Changed
						| CDDPClient::EObserveNotification_Removed
						, [&](CDDPClient::EObserveNotification _Notification, const NEncoding::CEJSON &_NotificationData)
						{
							if (_Notification & CDDPClient::EObserveNotification_Added)
								++nAdded;
							if (_Notification & CDDPClient::EObserveNotification_Changed)
								++nChanged;
							if (_Notification & CDDPClient::EObserveNotification_Removed)
								++nRemoved;
							
							Event.f_Signal();
						}
					).f_CallSync()
				;
				
				CActorCallback Subscription = Client
					(
						&CDDPClient::f_Subscribe
						, ConcurrentActor
						, "testSub"
						, ""
						, CEJSON(fg_CreateVector<CEJSON>())
						, CDDPClient::ESubscriptionNotification_Ready
						| CDDPClient::ESubscriptionNotification_Error
						, [&](CDDPClient::ESubscriptionNotification _Notification, const NEncoding::CEJSON &_NotificationData)
						{
							if (_Notification & CDDPClient::ESubscriptionNotification_Ready)
								++nReady;
							if (_Notification & CDDPClient::ESubscriptionNotification_Error)
								++nError;
							
							Event.f_Signal();
						}
						, true
					).f_CallSync(10.0)
				;
				
				CClock Timeout;
				Timeout.f_Start();
				
				while (!nReady.f_Load())
				{
					if (Timeout.f_GetTime() >= 10.0)
						break;
					Event.f_WaitTimeout(1.0);
				}
				
				DMibAssert(nReady.f_Load(), ==, 1);
				DMibAssert(nAdded.f_Load(), ==, 10);
				
				auto fGetDocuments = [&]() -> TCMap<CStr, NEncoding::CEJSON>
					{
						TCMap<CStr, NEncoding::CEJSON> Documents;
						NThread::CMutual Lock;
						
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
									catch (NException::CException const &_Exception)
									{
										(void)_Exception;
									}
								}
							)
							.f_CallSync()
						;
						return Documents;
					}
				;
				
				// Test document accessor
				{
					TCMap<CStr, NEncoding::CEJSON> Documents = fGetDocuments();
					
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
							CActorCallback Subscription = Client
								(
									&CDDPClient::f_Subscribe
									, ConcurrentActor
									, "testFalseSub"
									, ""
									, CEJSON(fg_CreateVector<CEJSON>())
									, CDDPClient::ESubscriptionNotification_None
									, [](CDDPClient::ESubscriptionNotification _Notification, const NEncoding::CEJSON &_NotificationData)
									{

									}
									, true
								).f_CallSync(10.0)
							;
						}
					;
					DMibExpectException(fSubscribe(), DMibErrorInstance("sub-not-found: Subscription not found"));
				}

				DMibExpectException((Client(&CDDPClient::f_Method, "testNoMethod", fg_CreateVector<CEJSON>()).f_CallSync()), DMibErrorInstance("method-not-found: Method not found"));

				DMibExpect(nChanged, ==, 0);
				Client(&CDDPClient::f_Method, "testChanged", fg_CreateVector<CEJSON>()).f_CallSync();
				Timeout.f_Start();
				while (!nChanged.f_Load())
				{
					if (Timeout.f_GetTime() >= 10.0)
						break;
					Event.f_WaitTimeout(1.0);
				}
				DMibExpect(nChanged, ==, 1);
				
				auto DocumentsAfterChanged = fGetDocuments();
				DMibExpect(DocumentsAfterChanged, ==, ServerInternal.m_Data["testCollection"]);
				
				DMibExpect(nRemoved, ==, 0);
				Client(&CDDPClient::f_Method, "testRemoved", fg_CreateVector<CEJSON>()).f_CallSync();
				Timeout.f_Start();
				while (!nRemoved.f_Load())
				{
					if (Timeout.f_GetTime() >= 10.0)
						break;
					Event.f_WaitTimeout(1.0);
				}
				DMibExpect(nRemoved, ==, 1);

				auto DocumentsAfterRemoved = fGetDocuments();
				DMibExpect(DocumentsAfterRemoved, ==, ServerInternal.m_Data["testCollection"]);
				
				DMibExpect(ServerInternal.m_nUnsubscribe.f_Load(), ==, 0);
			}
			{
				CClock Timeout;
				Timeout.f_Start();
				
				while (ServerInternal.m_nUnsubscribe.f_Load() == 0)
				{
					if (Timeout.f_GetTime() >= 10.0)
						break;
					ServerInternal.m_Event.f_WaitTimeout(1.0);
				}
				
				DMibExpect(ServerInternal.m_nUnsubscribe.f_Load(), ==, 1);
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

						CSSLContext::fs_GenerateSelfSignedCertAndKey("Malterlib test Self Signed", fg_CreateVector<CStr>("localhost"), ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData, 1024);

						NPtr::TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
						ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
						NPtr::TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						
						return {CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext)};
					}
				)
			;
		};
	}
};

DMibTestRegister(CDDP_Tests, Malterlib::Web);
