// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Test/Performance>
#include <Mib/Web/WebSocket>
#include <Mib/Network/Sockets/TCP>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Cryptography/Certificate>
#include <Mib/Concurrency/DistributedActorTestHelpers>
#include <Mib/File/File>

using namespace NMib;
using namespace NMib::NNetwork;
using namespace NMib::NTest;
using namespace NMib::NThread;
using namespace NMib::NContainer;
using namespace NMib::NStr;
using namespace NMib::NConcurrency;
using namespace NMib::NStorage;
using namespace NMib::NCryptography;
using namespace NMib::NWeb;

namespace
{
	fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;

	// Measures the websocket actor pair over loopback TCP: a ping over an echoing server for
	// round trip latency and a client to server upload for throughput, as plain ws and as wss
	// under a self signed certificate — the plain scheme is what exercises the send window
	// machinery with no TLS transport gating ahead of it
	struct CBenchState
	{
		CBenchState(TCSharedPointer<CDefaultRunLoop> const &_pRunLoop, bool _bEcho, uint64 _AckTargetBytes)
			: m_pRunLoop(_pRunLoop)
			, m_bEcho(_bEcho)
			, m_AckTargetBytes(_AckTargetBytes)
		{
		}

		~CBenchState()
		{
			TCFutureVector<void> Destroys;
			{
				DMibLock(m_Lock);
				m_ClientCallbacksReference.f_Clear();
				m_ServerCallbacksReference.f_Clear();
				if (m_ClientSocket)
					fg_Move(m_ClientSocket).f_Destroy() > Destroys;
				if (m_ServerConnection)
					fg_Move(m_ServerConnection).f_Destroy() > Destroys;
				if (m_ClientActor)
					fg_Move(m_ClientActor).f_Destroy() > Destroys;
				if (m_ListenSubscription)
				{
					m_ListenSubscription.f_Clear();
				}
			}
			fg_AllDoneWrapped(Destroys).f_CallSync(m_pRunLoop, g_Timeout);

			if (m_ServerActor)
			{
				m_ServerActor->f_BlockDestroy(m_pRunLoop->f_ActorDestroyLoop());
				m_ServerActor.f_Clear();
			}
		}

		// Resolving a promise can resume the waiting coroutine inline on this thread, and it comes
		// straight back here for the next round trip. Detaching the promise under the lock and
		// resolving it outside keeps the member consistent for whatever the resumption does
		void f_ClientReceived(umint _nBytes)
		{
			TCUniquePointer<TCPromise<void>> pPromise;
			{
				DMibLock(m_Lock);
				m_nClientReceivedBytes += _nBytes;
				if (m_pClientWaitPromise && m_nClientReceivedBytes >= m_ClientWaitTargetBytes)
					pPromise = fg_Move(m_pClientWaitPromise);
			}

			if (pPromise)
				pPromise->f_SetResult();
		}

		// Data is delivered from inside the accept, before the accept side has assigned
		// m_ServerConnection. The returned future resolves once it exists, so a receive that lands
		// in that window waits rather than finding a null connection and dropping its reply
		TCFuture<void> f_WaitServerConnection()
		{
			TCUniquePointer<TCPromise<void>> pPromise = fg_Construct();
			TCFuture<void> Future = pPromise->f_Future();

			{
				DMibLock(m_Lock);
				if (!m_ServerConnection)
				{
					m_ServerConnectionWaiters.f_InsertLast(fg_Move(pPromise));
					return Future;
				}
			}

			pPromise->f_SetResult();

			return Future;
		}

		// The returned future resolves once the client has received _TargetBytes in total
		TCFuture<void> f_WaitClientReceived(uint64 _TargetBytes)
		{
			TCUniquePointer<TCPromise<void>> pPromise = fg_Construct();
			TCFuture<void> Future = pPromise->f_Future();

			{
				DMibLock(m_Lock);
				DMibCheck(!m_pClientWaitPromise);

				m_ClientWaitTargetBytes = _TargetBytes;

				// The reply can land before the caller gets here. That case resolves the promise
				// rather than returning a default constructed future, which carries no promise
				// data and would dereference null when awaited
				if (m_nClientReceivedBytes < _TargetBytes)
					m_pClientWaitPromise = fg_Move(pPromise);
			}

			if (pPromise)
				pPromise->f_SetResult();

			return Future;
		}

		CIntrusiveRefCountWithWeak m_RefCount;

		CMutual m_Lock;
		TCSharedPointer<CDefaultRunLoop> m_pRunLoop;

		bool m_bEcho = false;
		uint64 m_AckTargetBytes = 0;
		uint64 m_nServerReceivedBytes = 0;

		TCActor<CWebSocketServerActor> m_ServerActor;
		CActorSubscription m_ListenSubscription;
		TCActor<CWebSocketActor> m_ServerConnection;
		CActorSubscription m_ServerCallbacksReference;

		TCActor<CWebSocketClientActor> m_ClientActor;
		TCActor<CWebSocketActor> m_ClientSocket;
		CActorSubscription m_ClientCallbacksReference;

		// Waiters for m_ServerConnection, resolved when the accept side assigns it
		TCVector<TCUniquePointer<TCPromise<void>>> m_ServerConnectionWaiters;

		uint64 m_nClientReceivedBytes = 0;
		uint64 m_ClientWaitTargetBytes = 0;
		TCUniquePointer<TCPromise<void>> m_pClientWaitPromise;
	};

	void fg_SetupConnection
		(
			CActorRunLoopTestHelper &_RunLoopHelper
			, TCSharedPointerSupportWeak<CBenchState> const &_pState
			, FVirtualSocketFactory const &_ServerFactory
			, FVirtualSocketFactory const &_ClientFactory
		)
	{
		TCWeakPointer<CBenchState> pStateWeak = _pState;

		CNetAddress ListenAddress = CSocket::fs_ResolveAddress("localhost", ENetAddressType_TCPv4);

		_pState->m_ServerActor = fg_ConstructActor<CWebSocketServerActor>();
		_pState->m_ServerActor(&CWebSocketServerActor::f_SetDefaultFragmentationSize, umint(1024 * 1024), umint(4 * 1024 * 1024)).f_DiscardResult();

		auto ListenResult = _pState->m_ServerActor
			(
				&CWebSocketServerActor::f_StartListenAddress
				, fg_CreateVector(ListenAddress)
				, ENetFlag_None
				, g_ActorFunctorWeak / [pStateWeak](CWebSocketNewServerConnection _ConnectionInfo) -> TCFuture<void>
				{
					CWebSocketNewServerConnection ConnectionInfo = fg_Move(_ConnectionInfo);

					ConnectionInfo.m_fOnReceiveBinaryMessage = g_ActorFunctorWeak / [pStateWeak](TCSharedPointer<NStream::CBinaryStorage const> _pMessage) -> TCFuture<void>
						{
							auto pState = pStateWeak.f_Lock();
							if (!pState)
								co_return {};

							co_await pState->f_WaitServerConnection();

							DMibLock(pState->m_Lock);

							if (pState->m_bEcho)
							{
								// The received storage is forwarded by reference; the send queue
								// keeps it alive until it is back on the wire
								pState->m_ServerConnection(&CWebSocketActor::f_SendBinaryStorage, fg_Move(_pMessage), 0).f_DiscardResult();
								co_return {};
							}

							pState->m_nServerReceivedBytes += _pMessage->f_GetTotalLength();
							if (pState->m_nServerReceivedBytes >= pState->m_AckTargetBytes && pState->m_ServerConnection)
							{
								pState->m_nServerReceivedBytes -= pState->m_AckTargetBytes;

								CIOByteVector Ack;
								Ack.f_SetLen(8);
								NMemory::fg_ObjectSet(Ack.f_GetArray(), (uint8)0xA5, 8);

								TCSharedPointer<CIOByteVector const> pAck = fg_Construct(fg_Move(Ack));
								pState->m_ServerConnection(&CWebSocketActor::f_SendBinary, fg_Move(pAck), 0).f_DiscardResult();
							}

							co_return {};
						}
					;

					CStr Protocol;
					if (!ConnectionInfo.m_Protocols.f_IsEmpty())
						Protocol = ConnectionInfo.m_Protocols.f_GetFirst();

					auto Socket = ConnectionInfo.f_Accept
						(
							Protocol
							, [pStateWeak](TCAsyncResult<CActorSubscription> &&_Callback)
							{
								auto pState = pStateWeak.f_Lock();
								if (!pState)
									return;

								DMibLock(pState->m_Lock);
								if (_Callback)
									pState->m_ServerCallbacksReference = fg_Move(*_Callback);
							}
						)
					;

					if (auto pState = pStateWeak.f_Lock())
					{
						TCVector<TCUniquePointer<TCPromise<void>>> Waiters;
						{
							DMibLock(pState->m_Lock);
							pState->m_ServerConnection = fg_Move(Socket);
							Waiters = fg_Move(pState->m_ServerConnectionWaiters);
						}

						// Resolved outside the lock: resuming a waiter runs receive handling that
						// takes the same lock again
						for (auto &pWaiter : Waiters)
							pWaiter->f_SetResult();
					}

					co_return {};
				}
				, g_ActorFunctorWeak / [](CWebSocketActor::CConnectionInfo _ConnectionInfo) -> TCFuture<void>
				{
					DMibLog(Error, "Benchmark accept failed: {}", _ConnectionInfo.m_Error);
					co_return {};
				}
				// The benchmark measures the transport, not the masking XOR: both peers are ours on a
				// confidential loopback link, so the handshake negotiates unmasked frames
				, CWebSocketListenSocketFactory::fs_PerAddress
					(
						[ServerFactory = _ServerFactory](umint, CNetAddress const &) -> CWebSocketListenAddressConfig
						{
							return {.m_Factory = ServerFactory, .m_bNegotiateUnmaskedFrames = true};
						}
					)
			)
			.f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout)
		;
		_pState->m_ListenSubscription = fg_Move(ListenResult.m_Subscription);

		uint16 ListenPort = ListenResult.m_ListenPorts[0];

		_pState->m_ClientActor = fg_ConstructActor<CWebSocketClientActor>();

		umint nSendWindow = umint(fg_GetSys()->f_GetEnvironmentVariable("SendWindow").f_ToInt(int64(0)));

		auto NewClientConnection = _pState->m_ClientActor
			(
				&CWebSocketClientActor::f_Connect
				, CWebSocketClientActor::CConnectSettings
				{
					.m_ConnectToAddress = "localhost"
					, .m_Port = ListenPort
					, .m_URI = "/Bench"
					, .m_Origin = "http://localhost"
					, .m_Protocols = fg_CreateVector<CStr>("Bench")
					, .m_SocketFactory = _ClientFactory
					, .m_bNegotiateUnmaskedFrames = true
					, .m_FragmentationSize = 1024 * 1024
					, .m_MaxFragmentSize = 4 * 1024 * 1024
					, .m_SendWindowBytes = nSendWindow
				}
			)
			.f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout)
		;

		NewClientConnection.m_fOnReceiveBinaryMessage = g_ActorFunctorWeak / [pStateWeak](TCSharedPointer<NStream::CBinaryStorage const> _pMessage) -> TCFuture<void>
			{
				if (auto pState = pStateWeak.f_Lock())
					pState->f_ClientReceived(_pMessage->f_GetTotalLength());
				co_return {};
			}
		;

		_pState->m_ClientSocket = NewClientConnection.f_Accept
			(
				[pStateWeak](TCAsyncResult<CActorSubscription> &&_Callback)
				{
					auto pState = pStateWeak.f_Lock();
					if (!pState)
						return;

					DMibLock(pState->m_Lock);
					if (_Callback)
						pState->m_ClientCallbacksReference = fg_Move(*_Callback);
				}
			)
		;
	}

	// Drives the benchmark from a coroutine so each iteration costs actor scheduling, not test
	// thread synchronization
	struct CBenchDriverActor : public CActor
	{
		TCFuture<void> f_RunPing(TCSharedPointerSupportWeak<CBenchState> _pState, umint _nRoundTrips)
		{
			CIOByteVector Ping;
			Ping.f_SetLen(8);
			NMemory::fg_ObjectSet(Ping.f_GetArray(), (uint8)0x5A, 8);
			TCSharedPointer<CIOByteVector const> pChunk = fg_Construct(fg_Move(Ping));

			uint64 TargetBytes = 0;
			{
				DMibLock(_pState->m_Lock);
				TargetBytes = _pState->m_nClientReceivedBytes;
			}

			for (umint i = 0; i < _nRoundTrips; ++i)
			{
				TargetBytes += 8;
				_pState->m_ClientSocket(&CWebSocketActor::f_SendBinary, fg_TempCopy(pChunk), 0).f_DiscardResult();
				co_await _pState->f_WaitClientReceived(TargetBytes);
			}

			co_return {};
		}

		TCFuture<void> f_RunUpload(TCSharedPointerSupportWeak<CBenchState> _pState, TCSharedPointer<CIOByteVector const> _pChunk, uint64 _nBytes)
		{
			umint ChunkSize = _pChunk->f_GetLen();

			uint64 AckTargetBytes = 0;
			{
				DMibLock(_pState->m_Lock);
				AckTargetBytes = _pState->m_nClientReceivedBytes + 8;
			}

			// Keep a pipeline of sends in flight so the socket never idles between chunks,
			// mirroring the transport benchmark's pipeline depth
			umint PipelineLength = fg_GetSys()->f_GetEnvironmentVariable("PipelineLength").f_ToInt(umint(2));

			uint64 nChunks = (_nBytes + ChunkSize - 1) / ChunkSize;
			uint64 nQueued = 0;

			TCVector<TCFuture<void>> InFlight;
			while (nQueued < nChunks && InFlight.f_GetLen() < PipelineLength)
			{
				InFlight.f_InsertLast(_pState->m_ClientSocket(&CWebSocketActor::f_SendBinary, fg_TempCopy(_pChunk), 0));
				++nQueued;
			}

			for (uint64 iChunk = 0; iChunk < nChunks; ++iChunk)
			{
				umint iSlot = umint(iChunk % InFlight.f_GetLen());
				co_await fg_Move(InFlight[iSlot]);

				if (nQueued < nChunks)
				{
					InFlight[iSlot] = _pState->m_ClientSocket(&CWebSocketActor::f_SendBinary, fg_TempCopy(_pChunk), 0);
					++nQueued;
				}
			}

			co_await _pState->f_WaitClientReceived(AckTargetBytes);

			co_return {};
		}
	};

	class CWebSocketPerformance_Tests : public CTest
	{
#if defined(DMibDebug) || defined(DMibSanitizerEnabled)
		constexpr static uint64 mc_nTransferBytes = 8ull << 20;
		constexpr static umint mc_nPingRoundTrips = 128;
		constexpr static umint mc_nRepetitions = 3;
#else
		constexpr static uint64 mc_nTransferBytes = 8 * (1024ull << 20);
		constexpr static umint mc_nPingRoundTrips = 4 * 16384;
		constexpr static umint mc_nRepetitions = 5;
#endif
		constexpr static uint32 mc_ChunkSize = NFile::gc_IdealIoSize;

		template <typename tf_FMeasure>
		static void fs_MeasureTransports(CTestPerformance &_PerfTest, tf_FMeasure const &_fMeasure)
		{
			_fMeasure("ws", FVirtualSocketFactory(), FVirtualSocketFactory());

			CSSLSettings ServerSettings;
			CCertificateOptions Options;
			Options.m_CommonName = "Malterlib websocket benchmark";
			Options.m_Hostnames = fg_CreateVector<CStr>("localhost");
			Options.m_KeySetting = CPublicKeySettings_EC_secp256r1{};
			CCertificate::fs_GenerateSelfSignedCertAndKey(Options, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
			TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);

			CSSLSettings ClientSettings;
			ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_UseSpecificPeerCertificate;
			ClientSettings.m_CACertificateData = ServerSettings.m_PublicCertificateData;
			TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);

			_fMeasure("wss", CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext));

			DMibExpectTrue(_PerfTest);
		}

	public:
		void f_DoTests()
		{
			DMibTestSuite(CTestCategory("WebSocketPing") << CTestGroup("Performance"))
			{
				CActorRunLoopTestHelper RunLoopHelper;

				CTestPerformance PerfTest(0.015);

				fs_MeasureTransports
					(
						PerfTest
						, [&](CStr const &_Tag, FVirtualSocketFactory const &_ServerFactory, FVirtualSocketFactory const &_ClientFactory)
						{
							DMibTestPath(_Tag);

							TCSharedPointerSupportWeak<CBenchState> pState = fg_Construct(RunLoopHelper.m_pRunLoop, true, uint64(0));
							fg_SetupConnection(RunLoopHelper, pState, _ServerFactory, _ClientFactory);

							TCActor<CBenchDriverActor> Driver = fg_ConstructActor<CBenchDriverActor>();

							Driver(&CBenchDriverActor::f_RunPing, pState, mc_nPingRoundTrips / 8)
								.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
							;

							CTestPerformanceMeasure Time("Ping_{}"_f << _Tag);
							for (umint iRepetition = 0; iRepetition < mc_nRepetitions; ++iRepetition)
							{
								Time.f_Start();
								Driver(&CBenchDriverActor::f_RunPing, pState, mc_nPingRoundTrips)
									.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
								;
								Time.f_Stop(mc_nPingRoundTrips);
							}
							PerfTest.f_Add(Time);

							Driver->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
						}
					)
				;
			};

			DMibTestSuite(CTestCategory("WebSocketThroughput") << CTestGroup("Performance"))
			{
				CActorRunLoopTestHelper RunLoopHelper;

				CTestPerformance PerfTest(0.015);

				fs_MeasureTransports
					(
						PerfTest
						, [&](CStr const &_Tag, FVirtualSocketFactory const &_ServerFactory, FVirtualSocketFactory const &_ClientFactory)
						{
							DMibTestPath(_Tag);

							TCSharedPointerSupportWeak<CBenchState> pState = fg_Construct(RunLoopHelper.m_pRunLoop, false, mc_nTransferBytes);
							fg_SetupConnection(RunLoopHelper, pState, _ServerFactory, _ClientFactory);

							TCActor<CBenchDriverActor> Driver = fg_ConstructActor<CBenchDriverActor>();

							// The chunk is created once outside the measured path and sent by
							// reference every time, so the benchmark measures the transport
							// rather than buffer creation
							CIOByteVector ChunkData;
							ChunkData.f_SetLen(mc_ChunkSize);
							NMemory::fg_ObjectSet(ChunkData.f_GetArray(), (uint8)0x3C, mc_ChunkSize);
							TCSharedPointer<CIOByteVector const> pChunk = fg_Construct(fg_Move(ChunkData));

							{
								DMibLock(pState->m_Lock);
								pState->m_AckTargetBytes = uint64(mc_ChunkSize);
							}
							Driver(&CBenchDriverActor::f_RunUpload, pState, pChunk, uint64(mc_ChunkSize))
								.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
							;

							{
								DMibLock(pState->m_Lock);
								pState->m_AckTargetBytes = mc_nTransferBytes;
							}

							CTestPerformanceMeasure Time("Thr_{}"_f << _Tag);
							for (umint iRepetition = 0; iRepetition < mc_nRepetitions; ++iRepetition)
							{
								Time.f_Start();
								Driver(&CBenchDriverActor::f_RunUpload, pState, pChunk, mc_nTransferBytes)
									.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
								;
								Time.f_Stop(mc_nTransferBytes);
							}
							PerfTest.f_Add(Time);

							Driver->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
						}
					)
				;
			};
		}
	};

	DMibTestRegister(CWebSocketPerformance_Tests, Malterlib::Web);
}
