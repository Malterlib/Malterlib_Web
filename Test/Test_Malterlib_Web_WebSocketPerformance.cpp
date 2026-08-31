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
#include <Mib/Time/Stopwatch>

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

	// The callback host: replies and receive counting run on a pool actor of the chosen
	// priority — never on the ambient test actor, which is in no pool. The normal variant
	// measures the cross pool hop, the high CPU one stays in the socket actors’ pool
	template <EPriority t_Priority>
	struct TCBenchHandlerActor : public CActor
	{
		static constexpr EPriority mc_Priority = t_Priority;
	};

	using CBenchHandlerActor = TCBenchHandlerActor<EPriority_Normal>;
	using CBenchHandlerActorHighCpu = TCBenchHandlerActor<EPriority_NormalHighCPU>;

	// The upload ack target travels in band as a tiny control message, so the loopback and the
	// cross machine suites share one driver and one server callback: eight magic bytes and the
	// payload byte count the server answers with an eight byte ack
	constexpr uint64 gc_BenchControlMagic = 0x68636E654262694Dull; // "MibBench"
	constexpr umint gc_BenchControlBytes = 16;

	TCSharedPointer<CIOByteVector const> fg_BenchControlMessage(uint64 _AckTargetBytes)
	{
		CIOByteVector Message;
		Message.f_SetLen(gc_BenchControlBytes);

		uint64 Magic = gc_BenchControlMagic;
		NMemory::fg_ObjectCopy(Message.f_GetArray(), (uint8 const *)&Magic, sizeof(Magic));
		NMemory::fg_ObjectCopy(Message.f_GetArray() + sizeof(Magic), (uint8 const *)&_AckTargetBytes, sizeof(_AckTargetBytes));

		TCSharedPointer<CIOByteVector const> pMessage = fg_Construct(fg_Move(Message));

		return pMessage;
	}

	bool fg_BenchParseControl(uint8 const *_pData, umint _nBytes, uint64 &o_AckTargetBytes)
	{
		if (_nBytes != gc_BenchControlBytes)
			return false;

		uint64 Magic = 0;
		NMemory::fg_ObjectCopy((uint8 *)&Magic, _pData, sizeof(Magic));
		if (Magic != gc_BenchControlMagic)
			return false;

		NMemory::fg_ObjectCopy((uint8 *)&o_AckTargetBytes, _pData + sizeof(Magic), sizeof(o_AckTargetBytes));

		return true;
	}

	// The server half on its own, so the cross machine serve side can stand one up without a
	// client in the same process; returns the bound port for listens on an ephemeral one
	template <typename t_CHandler>
	uint16 fg_SetupServer
		(
			CActorRunLoopTestHelper &_RunLoopHelper
			, TCSharedPointerSupportWeak<CBenchState> const &_pState
			, CNetAddress const &_ListenAddress
			, FVirtualSocketFactory const &_ServerFactory
			, bool _bMasked
			, TCActor<t_CHandler> const &_HandlerActor
		)
	{
		TCWeakPointer<CBenchState> pStateWeak = _pState;

		_pState->m_ServerActor = fg_ConstructActor<CWebSocketServerActor>();
		_pState->m_ServerActor(&CWebSocketServerActor::f_SetDefaultFragmentationSize, umint(1024 * 1024), umint(4 * 1024 * 1024)).f_DiscardResult();

		auto ListenResult = _pState->m_ServerActor
			(
				&CWebSocketServerActor::f_StartListenAddress
				, fg_CreateVector(_ListenAddress)
				, ENetFlag_None
				// Bound to the handler actor: an unbound functor runs on the ambient test actor,
				// whose thread the cross machine serve parks in a sleep loop instead of pumping
				// the run loop
				, g_ActorFunctorWeak(_HandlerActor) / [pStateWeak, _HandlerActor](CWebSocketNewServerConnection _ConnectionInfo) -> TCFuture<void>
				{
					CWebSocketNewServerConnection ConnectionInfo = fg_Move(_ConnectionInfo);

					auto fOnReceiveBinaryMessage = [pStateWeak](TCSharedPointer<NStream::CBinaryStorage const> _pMessage) -> TCFuture<void>
						{
							auto pState = pStateWeak.f_Lock();
							if (!pState)
								co_return {};

							co_await pState->f_WaitServerConnection();

							DMibLock(pState->m_Lock);

							if (_pMessage->f_GetTotalLength() == gc_BenchControlBytes)
							{
								uint8 Control[gc_BenchControlBytes];
								_pMessage->f_CopyTo(Control, 0, gc_BenchControlBytes);

								uint64 ControlTarget = 0;
								if (fg_BenchParseControl(Control, gc_BenchControlBytes, ControlTarget))
								{
									pState->m_AckTargetBytes = ControlTarget;
									pState->m_nServerReceivedBytes = 0;
									co_return {};
								}
							}

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

					ConnectionInfo.m_fOnReceiveBinaryMessage = g_ActorFunctorWeak(_HandlerActor) / fOnReceiveBinaryMessage;

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
				, g_ActorFunctorWeak(_HandlerActor) / [](CWebSocketActor::CConnectionInfo _ConnectionInfo) -> TCFuture<void>
				{
					DMibLog(Error, "Benchmark accept failed: {}", _ConnectionInfo.m_Error);
					co_return {};
				}
				// Both peers are ours on a confidential loopback link, so the handshake negotiates
				// unmasked frames unless this run measures the masking XOR itself
				, CWebSocketListenSocketFactory::fs_PerAddress
					(
						[ServerFactory = _ServerFactory, _bMasked](umint, CNetAddress const &) -> CWebSocketListenAddressConfig
						{
							return {.m_Factory = ServerFactory, .m_bNegotiateUnmaskedFrames = !_bMasked};
						}
					)
			)
			.f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout)
		;
		_pState->m_ListenSubscription = fg_Move(ListenResult.m_Subscription);

		return ListenResult.m_ListenPorts.f_IsEmpty() ? uint16(0) : ListenResult.m_ListenPorts[0];
	}

	// The client half: connect, receive counting, and the send window pass through
	template <typename t_CHandler>
	void fg_SetupClient
		(
			CActorRunLoopTestHelper &_RunLoopHelper
			, TCSharedPointerSupportWeak<CBenchState> const &_pState
			, CStr const &_ConnectToAddress
			, uint16 _Port
			, FVirtualSocketFactory const &_ClientFactory
			, bool _bMasked
			, TCActor<t_CHandler> const &_HandlerActor
		)
	{
		TCWeakPointer<CBenchState> pStateWeak = _pState;

		_pState->m_ClientActor = fg_ConstructActor<CWebSocketClientActor>();

		umint nSendWindow = umint(fg_GetSys()->f_GetEnvironmentVariable("SendWindow").f_ToInt(int64(0)));

		auto NewClientConnection = _pState->m_ClientActor
			(
				&CWebSocketClientActor::f_Connect
				, CWebSocketClientActor::CConnectSettings
				{
					.m_ConnectToAddress = _ConnectToAddress
					, .m_Port = _Port
					, .m_URI = "/Bench"
					, .m_Origin = "http://localhost"
					, .m_Protocols = fg_CreateVector<CStr>("Bench")
					, .m_SocketFactory = _ClientFactory
					, .m_bNegotiateUnmaskedFrames = !_bMasked
					, .m_FragmentationSize = 1024 * 1024
					, .m_MaxFragmentSize = 4 * 1024 * 1024
					, .m_SendWindowBytes = nSendWindow
				}
			)
			.f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout)
		;

		// The lambda's strong capture of the handler actor keeps it alive for the life of the
		// callback: with a client half alone in the process, the weak binding is otherwise the
		// only reference and the actor dies when the setup call returns
		auto fOnClientReceiveBinaryMessage = [pStateWeak, _HandlerActor](TCSharedPointer<NStream::CBinaryStorage const> _pMessage) -> TCFuture<void>
			{
				if (auto pState = pStateWeak.f_Lock())
					pState->f_ClientReceived(_pMessage->f_GetTotalLength());
				co_return {};
			}
		;

		NewClientConnection.m_fOnReceiveBinaryMessage = g_ActorFunctorWeak(_HandlerActor) / fOnClientReceiveBinaryMessage;

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

	template <typename t_CHandler>
	void fg_SetupConnection
		(
			CActorRunLoopTestHelper &_RunLoopHelper
			, TCSharedPointerSupportWeak<CBenchState> const &_pState
			, CStr const &_Address
			, FVirtualSocketFactory const &_ServerFactory
			, FVirtualSocketFactory const &_ClientFactory
			, bool _bMasked
			, TCActor<t_CHandler> const &_HandlerActor
		)
	{
		uint16 ListenPort = fg_SetupServer(_RunLoopHelper, _pState, CSocket::fs_ResolveAddress(_Address, ENetAddressType_TCPv4), _ServerFactory, _bMasked, _HandlerActor);

		fg_SetupClient(_RunLoopHelper, _pState, _Address, ListenPort, _ClientFactory, _bMasked, _HandlerActor);
	}

	// Drives the benchmark from a coroutine so each iteration costs actor scheduling, not test
	// thread synchronization. The priority picks the pool the driver runs in: the normal pool
	// hops to the socket actors’ high CPU pool every round trip, the high CPU variant stays
	// inside it
	template <EPriority t_Priority>
	struct TCBenchDriverActor : public CActor
	{
		static constexpr EPriority mc_Priority = t_Priority;

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

			// Tells the server how many payload bytes this run covers before any of them are
			// queued; sends on one connection stay ordered
			_pState->m_ClientSocket(&CWebSocketActor::f_SendBinary, fg_BenchControlMessage(_nBytes), 0).f_DiscardResult();

			uint64 AckTargetBytes = 0;
			{
				DMibLock(_pState->m_Lock);
				AckTargetBytes = _pState->m_nClientReceivedBytes + 8;
			}

			// Keep a pipeline of sends in flight so the socket never idles between chunks,
			// mirroring the transport benchmark's pipeline depth
			umint PipelineLength = fg_GetSys()->f_GetEnvironmentVariable("PipelineLength").f_ToInt(umint(16));

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

	using CBenchDriverActor = TCBenchDriverActor<EPriority_Normal>;
	using CBenchDriverActorHighCpu = TCBenchDriverActor<EPriority_NormalHighCPU>;

	template <typename t_CDriver, typename t_CHandler>
	void fg_MeasurePing
		(
			CActorRunLoopTestHelper &_RunLoopHelper
			, CTestPerformance &_PerfTest
			, CStr const &_Name
			, CStr const &_Address
			, FVirtualSocketFactory const &_ServerFactory
			, FVirtualSocketFactory const &_ClientFactory
			, bool _bMasked
			, umint _nPingRoundTrips
			, umint _nRepetitions
		)
	{
		TCSharedPointerSupportWeak<CBenchState> pState = fg_Construct(_RunLoopHelper.m_pRunLoop, true, uint64(0));
		fg_SetupConnection(_RunLoopHelper, pState, _Address, _ServerFactory, _ClientFactory, _bMasked, fg_ConstructActor<t_CHandler>());

		TCActor<t_CDriver> Driver = fg_ConstructActor<t_CDriver>();

		Driver(&t_CDriver::f_RunPing, pState, _nPingRoundTrips / 8)
			.f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout)
		;

		CTestPerformanceMeasure Time(_Name);
		for (umint iRepetition = 0; iRepetition < _nRepetitions; ++iRepetition)
		{
			Time.f_Start();
			Driver(&t_CDriver::f_RunPing, pState, _nPingRoundTrips)
				.f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout)
			;
			Time.f_Stop(_nPingRoundTrips);
		}
		_PerfTest.f_Add(Time);

		Driver->f_BlockDestroy(_RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
	}

	// The measured upload loop on an already connected state, shared by the loopback suites and
	// the cross machine client
	void fg_MeasureUpload
		(
			CActorRunLoopTestHelper &_RunLoopHelper
			, CTestPerformance &_PerfTest
			, TCSharedPointerSupportWeak<CBenchState> const &_pState
			, CStr const &_MeasureName
			, uint64 _nTransferBytes
			, uint32 _ChunkSize
			, umint _nRepetitions
			, fp64 _CallTimeout
		)
	{
		TCActor<CBenchDriverActor> Driver = fg_ConstructActor<CBenchDriverActor>();

		// The chunk is created once outside the measured path and sent by reference every time,
		// so the benchmark measures the transport rather than buffer creation
		CIOByteVector ChunkData;
		ChunkData.f_SetLen(_ChunkSize);
		NMemory::fg_ObjectSet(ChunkData.f_GetArray(), (uint8)0x3C, _ChunkSize);
		TCSharedPointer<CIOByteVector const> pChunk = fg_Construct(fg_Move(ChunkData));

		Driver(&CBenchDriverActor::f_RunUpload, _pState, pChunk, uint64(_ChunkSize))
			.f_CallSync(_RunLoopHelper.m_pRunLoop, _CallTimeout)
		;

		CTestPerformanceMeasure Time(_MeasureName);
		for (umint iRepetition = 0; iRepetition < _nRepetitions; ++iRepetition)
		{
			Time.f_Start();
			Driver(&CBenchDriverActor::f_RunUpload, _pState, pChunk, _nTransferBytes)
				.f_CallSync(_RunLoopHelper.m_pRunLoop, _CallTimeout)
			;
			Time.f_Stop(_nTransferBytes);
		}
		_PerfTest.f_Add(Time);

		Driver->f_BlockDestroy(_RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
	}

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
			CStr RootDirectory = NFile::CFile::fs_GetProgramDirectory() / "WebSocketPerf";
			fg_TestAddCleanupPath(RootDirectory);

			auto fUnixAddress = [&](CStr const &_Tag)
				{
					return "UNIX:" + fg_GetSafeUnixSocketPath("{}/WebSocketPerf_{}.socket"_f << RootDirectory << _Tag);
				}
			;

			_fMeasure("ws", "localhost", FVirtualSocketFactory(), FVirtualSocketFactory(), false);
			_fMeasure("ws_unix", fUnixAddress("ws"), FVirtualSocketFactory(), FVirtualSocketFactory(), false);

			// The RFC’s client side masking, measured on its own so its cost stays visible
			_fMeasure("ws_masked", "localhost", FVirtualSocketFactory(), FVirtualSocketFactory(), true);

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

			_fMeasure("wss", "localhost", CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext), false);
			_fMeasure("wss_unix", fUnixAddress("wss"), CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext), false);
			_fMeasure("wss_masked", "localhost", CSocket_SSL::fs_GetFactory(pServerContext), CSocket_SSL::fs_GetFactory(pClientContext), true);

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
						, [&](CStr const &_Tag, CStr const &_Address, FVirtualSocketFactory const &_ServerFactory, FVirtualSocketFactory const &_ClientFactory, bool _bMasked)
						{
							DMibTestPath(_Tag);

							fg_MeasurePing<CBenchDriverActor, CBenchHandlerActor>(RunLoopHelper, PerfTest, "Ping_{}"_f << _Tag, _Address, _ServerFactory, _ClientFactory, _bMasked, mc_nPingRoundTrips, mc_nRepetitions);

							// The high CPU variant keeps the driver and the receive handling in the
							// socket actors’ pool, so a round trip never hops pools
							fg_MeasurePing<CBenchDriverActorHighCpu, CBenchHandlerActorHighCpu>(RunLoopHelper, PerfTest, "PingHigh_{}"_f << _Tag, _Address, _ServerFactory, _ClientFactory, _bMasked, mc_nPingRoundTrips, mc_nRepetitions);
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
						, [&](CStr const &_Tag, CStr const &_Address, FVirtualSocketFactory const &_ServerFactory, FVirtualSocketFactory const &_ClientFactory, bool _bMasked)
						{
							DMibTestPath(_Tag);

							TCSharedPointerSupportWeak<CBenchState> pState = fg_Construct(RunLoopHelper.m_pRunLoop, false, uint64(0));
							fg_SetupConnection(RunLoopHelper, pState, _Address, _ServerFactory, _ClientFactory, _bMasked, fg_ConstructActor<CBenchHandlerActor>());

							fg_MeasureUpload(RunLoopHelper, PerfTest, pState, "Thr_{}"_f << _Tag, mc_nTransferBytes, mc_ChunkSize, mc_nRepetitions, g_Timeout);
						}
					)
				;
			};

			// The cross machine pair, mirroring the transport benchmark's remote suites: the
			// same upload benchmark split over two processes on two hosts. The serve side
			// listens on a routable address and writes its connect URL as the ticket; the
			// ticket is carried to the client host, and the client connects from it and runs
			// the same measured loop as the loopback suites — the ticket reading side is the
			// SENDER, so run the client on the machine whose send path is measured. Frames are
			// negotiated unmasked as on the loopback link — both peers are ours. One client
			// at a time. Both sides are quiet no-ops without their required environment.
			//
			//   serve:  BenchHost=<routable address> [BenchPort=39301] [BenchSchemes=wss|ws]
			//           [BenchTicketFile=<program dir>/WebSocketBench.ticket]
			//           [BenchServeSeconds=600]
			//   client: BenchTicketFile=<carried ticket> (or BenchTicket=<url>)
			//           plus TransferBytes/ChunkSize/PipelineLength/SendWindow exactly as the
			//           loopback suites, and [BenchCallTimeout=600] to cover one upload on a
			//           slow link
			DMibTestSuite(CTestCategory("WebSocketRemoteServe") << CTestGroup("Performance"))
			{
				CStr Host = fg_GetSys()->f_GetEnvironmentVariable("BenchHost");
				if (!Host.f_IsEmpty())
				{
					CStr Scheme = fg_GetSys()->f_GetEnvironmentVariable("BenchSchemes");
					if (Scheme.f_IsEmpty())
						Scheme = "wss";
					uint32 Port = fg_GetSys()->f_GetEnvironmentVariable("BenchPort").f_ToInt(uint32(39301));
					CStr TicketFile = fg_GetSys()->f_GetEnvironmentVariable("BenchTicketFile");
					if (TicketFile.f_IsEmpty())
						TicketFile = NFile::CFile::fs_GetProgramDirectory() / "WebSocketBench.ticket";
					fp64 ServeSeconds = fg_GetSys()->f_GetEnvironmentVariable("BenchServeSeconds").f_ToFloat(fp64(600.0));

					CActorRunLoopTestHelper RunLoopHelper;

					FVirtualSocketFactory ServerFactory;
					if (Scheme == "wss")
					{
						CSSLSettings ServerSettings;
						CCertificateOptions Options;
						Options.m_CommonName = "Malterlib websocket benchmark";
						Options.m_Hostnames = fg_CreateVector<CStr>(Host);
						Options.m_KeySetting = CPublicKeySettings_EC_secp256r1{};
						CCertificate::fs_GenerateSelfSignedCertAndKey(Options, ServerSettings.m_PublicCertificateData, ServerSettings.m_PrivateKeyData);
						TCSharedPointer<CSSLContext> pServerContext = fg_Construct(CSSLContext::EType_Server, ServerSettings);
						ServerFactory = CSocket_SSL::fs_GetFactory(pServerContext);
					}

					TCSharedPointerSupportWeak<CBenchState> pState = fg_Construct(RunLoopHelper.m_pRunLoop, false, uint64(0));

					// Chunk counting is the serve side's hot path, so its receive handling stays
					// in the socket actors' pool
					fg_SetupServer(RunLoopHelper, pState, CSocket::fs_ResolveAddress("{}:{}"_f << Host << Port, ENetAddressType_TCPv4), ServerFactory, false, fg_ConstructActor<CBenchHandlerActorHighCpu>());

					CStr TicketString = "{}://{}:{}"_f << Scheme << Host << Port;
					NFile::CFile::fs_WriteStringToFile(TicketFile, TicketString, false);

					NSys::fg_ConsoleOutput(CStr("WebSocketRemoteServe: {} for {} seconds, ticket at {}\n"_f << TicketString << ServeSeconds << TicketFile));

					// Removing the ticket file ends the serve early, which is how a driver
					// script releases the server the moment its client is done
					NTime::CStopwatch Serving(true);
					while (Serving.f_GetTime() < ServeSeconds && NFile::CFile::fs_FileExists(TicketFile))
						NSys::fg_Thread_Sleep(0.25);
				}
			};

			DMibTestSuite(CTestCategory("WebSocketRemoteThroughput") << CTestGroup("Performance"))
			{
				CStr TicketString = fg_GetSys()->f_GetEnvironmentVariable("BenchTicket");
				CStr TicketFile = fg_GetSys()->f_GetEnvironmentVariable("BenchTicketFile");
				if (TicketString.f_IsEmpty() && !TicketFile.f_IsEmpty())
					TicketString = NFile::CFile::fs_ReadStringFromFile(TicketFile);

				if (!TicketString.f_IsEmpty())
				{
					CStr Scheme = "ws";
					CStr Address = TicketString;
					if (TicketString.f_StartsWith("wss://"))
					{
						Scheme = "wss";
						Address = TicketString.f_Right(TicketString.f_GetLen() - 6);
					}
					else if (TicketString.f_StartsWith("ws://"))
						Address = TicketString.f_Right(TicketString.f_GetLen() - 5);

					CStr ConnectHost = Address;
					uint32 ConnectPort = 0;
					aint nParsed = 0;
					(CStr::CParse("{}:{}") >> ConnectHost >> ConnectPort).f_Parse(Address, nParsed);

					uint32 ChunkSize = fg_GetSys()->f_GetEnvironmentVariable("ChunkSize").f_ToInt(mc_ChunkSize);
					uint64 TransferBytes = fg_GetSys()->f_GetEnvironmentVariable("TransferBytes").f_ToInt(mc_nTransferBytes);
					fp64 CallTimeout = fg_GetSys()->f_GetEnvironmentVariable("BenchCallTimeout").f_ToFloat(fp64(600.0));

					CActorRunLoopTestHelper RunLoopHelper;

					CTestPerformance PerfTest(0.015);

					FVirtualSocketFactory ClientFactory;
					if (Scheme == "wss")
					{
						// The serve side's certificate is self signed and per run; the client
						// accepts it outright — a benchmark link, with the handshake and record
						// crypto still fully measured
						CSSLSettings ClientSettings;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_IgnoreVerificationFailures;
						ClientSettings.m_VerificationFlags |= CSSLSettings::EVerificationFlag_IgnoreTrustFailures;
						TCSharedPointer<CSSLContext> pClientContext = fg_Construct(CSSLContext::EType_Client, ClientSettings);
						ClientFactory = CSocket_SSL::fs_GetFactory(pClientContext);
					}

					TCSharedPointerSupportWeak<CBenchState> pState = fg_Construct(RunLoopHelper.m_pRunLoop, false, uint64(0));
					fg_SetupClient(RunLoopHelper, pState, ConnectHost, uint16(ConnectPort), ClientFactory, false, fg_ConstructActor<CBenchHandlerActor>());

					DMibTestPath("Remote");
					fg_MeasureUpload(RunLoopHelper, PerfTest, pState, "ThrRemote_{}"_f << Scheme, TransferBytes, ChunkSize, mc_nRepetitions, CallTimeout);

					DMibExpectTrue(PerfTest);
				}
			};
		}
	};

	DMibTestRegister(CWebSocketPerformance_Tests, Malterlib::Web);
}
