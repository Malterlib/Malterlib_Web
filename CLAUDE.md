# CLAUDE.md - Web Module

This file provides guidance to Claude Code (claude.ai/code) when working with the Web module in Malterlib.

## Module Overview

The Web module provides comprehensive web communication functionality including HTTP servers, WebSocket support, HTTP clients, AWS integration, and SSL/TLS certificate management. The module is built on top of Malterlib's actor-based concurrency system and provides both synchronous and asynchronous APIs.

## Key Components

### HTTP Server & Client

#### HTTP Server (`CHTTPServer`)
Provides a high-performance HTTP server implementation with request handling.

```cpp
// Basic HTTP request handler
class CMyHTTPRequestHandler : public NHTTP::ICRequestHandler
{
public:
	NConcurrency::TCFuture<void> f_Handle
		(
			CHTTPRequest const &_Request
			, NStorage::TCIntrusivePointer<CHTTPConnection> const &_pConnection
		)
	{
		CHTTPResponseHeader Header;
		Header.m_MimeType = "text/html; charset=UTF-8";
		Header.m_Status = 200;

		_pConnection->f_WriteHeader(Header);
		_pConnection->f_WriteStr("<html><body>Hello World</body></html>");

		return NConcurrency::fs_MakeReady();
	}
};
```

#### HTTP Client (`CCurlActor`)
Actor-based HTTP client using libcurl for making HTTP requests.

```cpp
// Create curl actor for HTTP requests
TCActor<CCurlActor> CurlActor = fg_ConstructActor<CCurlActor>(CCurlActor::CCertificateConfig{});

// Make an HTTP GET request
CCurlActor::CRequest Request;
Request.m_URL = "https://api.example.com/data";
Request.m_Method = CCurlActor::EMethod_GET;
Request.m_Headers["Authorization"] = "Bearer token123";

CCurlActor::CResult Result = co_await CurlActor(&CCurlActor::f_ExecuteRequest, Request);

if (Result.m_StatusCode == 200)
{
	CStr Body = Result.m_Body;
	// Process response body
}
```

### WebSocket Support

#### WebSocket Server (`CWebSocketServerActor`)
Provides WebSocket server functionality with automatic frame handling.

```cpp
// Create WebSocket server
TCActor<CWebSocketServerActor> ServerActor = fg_ConstructActor<CWebSocketServerActor>();

// Create listen address
CNetAddressTCPv4 ListenAddr;
ListenAddr.f_SetAnyAddress();
ListenAddr.m_Port = 8080;

// Start listening with connection callbacks
CWebSocketServerActor::CListenResult ListenResult = co_await ServerActor
	(
		&CWebSocketServerActor::f_StartListenAddress
		, TCVector<CNetAddress>{ListenAddr}
		, ENetFlag_None
		, g_ActorFunctorWeak / [](CWebSocketNewServerConnection _Connection) -> TCFuture<void>
		{
			// Handle new connection - must accept or reject
			CWebSocketNewServerConnection Connection = fg_Move(_Connection);

			// Set up message handlers
			Connection.m_fOnReceiveTextMessage = g_ActorFunctorWeak / [Actor](CStr _Message) -> TCFuture<void>
				{
					// Handle received text message
					DMibLog(Info, "Received: {}", _Message);

					// Echo back
					co_await Actor(&CWebSocketActor::f_SendText, _Message + " (echo)");
				}
			;

			Connection.m_fOnClose = g_ActorFunctorWeak / [](EWebSocketStatus _Status, CStr _Message, EWebSocketCloseOrigin _Origin) -> TCFuture<void>
				{
					// Handle connection close
					DMibLog(Info, "Connection closed: {} - {}", _Status, _Message);
					co_return {};
				}
			;

			// Accept the connection (you can specify a protocol if needed)
			TCActor<CWebSocketActor> Actor = Connection.f_Accept
				(
					CStr() // Empty string means no specific protocol
					, fg_ThisActor(this) / [](TCAsyncResult<CActorSubscription> &&_Result)
					{
						// Store subscription if needed
					}
				)
			;

			// Send initial message
			co_await Actor(&CWebSocketActor::f_SendText, "Welcome!");

			co_return {};
		}
		, g_ActorFunctorWeak / [](CWebSocketActor::CConnectionInfo _ConnectionInfo) -> TCFuture<void>
		{
			// Handle failed connection attempt
			DMibLog(Info, "Connection failed: {}", _ConnectionInfo.m_Error);
			co_return {};
		}
		, NNetwork::FVirtualSocketFactory{} // Use default socket factory
	)
;

// ListenResult contains the subscription and actual listen ports
uint16 ListenPort = ListenResult.m_ListenPorts[0];
```

#### WebSocket Client (`CWebSocketClientActor`)
Provides WebSocket client functionality for connecting to WebSocket servers.

```cpp
// Create WebSocket client
TCActor<CWebSocketClientActor> ClientActor = fg_ConstructActor<CWebSocketClientActor>();

// Connect to server
CWebSocketNewClientConnection Connection = co_await ClientActor
	(
		&CWebSocketClientActor::f_Connect
		, "localhost"  // Server address
		, CStr()  // Bind address (empty = don't bind)
		, ENetAddressType_None  // Preferred address type
		, 8080  // Port
		, "/websocket"  // URI path
		, "http://localhost"  // Origin
		, TCVector<CStr>{}  // Protocols
		, NHTTP::CRequest()  // Additional headers
		, NNetwork::FVirtualSocketFactory{}  // Socket factory
	)
;

// Set up message handlers before accepting
Connection.m_fOnReceiveTextMessage = g_ActorFunctorWeak / [](CStr _Message) -> TCFuture<void>
{
	// Handle received message
	DMibLog(Info, "Received: {}", _Message);
	co_return {};
};

Connection.m_fOnClose = g_ActorFunctorWeak / [](EWebSocketStatus _Status, CStr _Message, EWebSocketCloseOrigin _Origin) -> TCFuture<void>
{
	// Handle connection close
	DMibLog(Info, "Connection closed: {} - {}", _Status, _Message);
	co_return {};
};

// Accept the connection after setting handlers
TCActor<CWebSocketActor> SocketActor = Connection.f_Accept
	(
		fg_ThisActor(this) / [](TCAsyncResult<CActorSubscription> &&_Result)
		{
			// Store subscription if needed
		}
	)
;

// Send messages
co_await SocketActor(&CWebSocketActor::f_SendText, "Hello Server");
```

### AWS Services Integration

#### S3 (`CAwsS3Actor`)
Provides AWS S3 bucket operations including upload, download, and listing.

```cpp
// Create S3 actor with credentials
CAwsCredentials Credentials;
Credentials.m_AccessKeyID = "AKIAIOSFODNN7EXAMPLE";
Credentials.m_SecretAccessKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
Credentials.m_Region = "us-west-2";

TCActor<CAwsS3Actor> S3Actor = fg_ConstructActor<CAwsS3Actor>(CurlActor, Credentials);

// Upload object to S3
CAwsS3Actor::CPutObjectInfo Info;
Info.m_ContentType = "application/json";
Info.m_CacheControl = "max-age=3600";

CByteVector Data = CByteVector::fs_FromString("{\"key\": \"value\"}");
co_await S3Actor
	(
		&CAwsS3Actor::f_PutObject
		, "my-bucket"
		, "path/to/object.json"
		, Info
		, Data
	)
;

// List bucket contents
CAwsS3Actor::CListBucket Bucket = co_await S3Actor(&CAwsS3Actor::f_ListBucket, "my-bucket");

for (auto const &Object : Bucket.m_Objects)
	DMibLog(Info, "Object: {} ({}bytes)", Object.m_Key, Object.m_Size);
```

#### CloudFront (`CAwsCloudFrontActor`)
Provides AWS CloudFront CDN invalidation and management.

```cpp
// Create CloudFront actor
TCActor<CAwsCloudFrontActor> CloudFrontActor = fg_ConstructActor<CAwsCloudFrontActor>(CurlActor, Credentials);

// Create invalidation
TCVector<CStr> Paths = {"/images/*", "/api/v1/*"};
CStr InvalidationId = co_await CloudFrontActor
	(
		&CAwsCloudFrontActor::f_CreateInvalidation
		, "E1EXAMPLE123"  // Distribution ID
		, Paths
	)
;
```

#### Lambda (`CAwsLambdaActor`)
Provides AWS Lambda function invocation.

```cpp
// Create Lambda actor
TCActor<CAwsLambdaActor> LambdaActor = fg_ConstructActor<CAwsLambdaActor>(CurlActor, Credentials);

// Invoke Lambda function
CEJsonSorted Payload;
Payload["key"] = "value";

CAwsLambdaActor::CInvokeResult InvokeResult = co_await LambdaActor
	(
		&CAwsLambdaActor::f_Invoke
		, "my-function"
		, Payload
		, CAwsLambdaActor::EInvocationType_RequestResponse
	)
;
```

### Utility Components

#### URL Parsing (`CHTTP_URL`)
Provides URL parsing and manipulation utilities.

```cpp
CHTTP_URL URL;
URL.f_Parse("https://user:pass@example.com:8080/path/to/resource?query=value#fragment");

CStr Scheme = URL.f_GetScheme();       // "https"
CStr Host = URL.f_GetHost();           // "example.com"
uint16 Port = URL.f_GetPort();         // 8080
CStr Path = URL.f_GetPath();           // "/path/to/resource"
CStr Query = URL.f_GetQuery();         // "query=value"
```

#### HTTP Fields (`CHTTP_Fields`)
Provides HTTP header field parsing and generation.

```cpp
CHTTP_Fields Fields;
Fields.f_SetField("Content-Type", "application/json");
Fields.f_SetField("Authorization", "Bearer token123");

CStr ContentType = Fields.f_GetField("Content-Type");
```

## Architecture Patterns

### Actor-Based Design
All network operations use the actor model for concurrency:
- Each network component (server, client, connection) is an actor
- Communication happens through futures and callbacks
- Thread-safe by design

### Request Handlers
HTTP and FastCGI servers use handler interfaces:
- Implement `ICRequestHandler` for HTTP
- Handlers return futures for async processing
- Connection objects provide response writing methods

### WebSocket Callbacks
WebSocket actors use callback-based event handling:
- Connection callbacks for new connections
- Message callbacks for data reception
- Close callbacks for connection termination

## Build Configuration

The Web module depends on:
- **libcurl** - HTTP client functionality
- **nginx** - Optional nginx launcher support
- **pcre2** - Regular expression support
- **BoringSSL** - SSL/TLS support (via Cryptography module)
- **zlib** - Compression support

External dependencies are managed through the build system:
```mib
%Import "../../External/curl/CMakeLists.txt"
{
	Import.CMake_Variables =+ ["BUILD_SHARED_LIBS=OFF", "CURL_USE_OPENSSL=ON"]
}
```

## Testing

The module includes comprehensive tests in `Test/`:
- `Test_Malterlib_Web_Curl.cpp` - HTTP client tests
- `Test_Malterlib_Web_WebSocket.cpp` - WebSocket tests
- `Test_Malterlib_Web_DDP.cpp` - DDP protocol tests

Tests use virtual socket factories for isolated testing:
```cpp
void fp_Test
	(
		TCFunction<TCTuple<FVirtualSocketFactory, FVirtualSocketFactory> ()> const &_fGetFactories
		, CStr const &_AcceptError
		, CStr const &_ConnectError
		, mint _FragmentationSize
	)
{
	// Test implementation with virtual sockets
}
```

## Common Usage Patterns

### Error Handling
Use exception types for error handling:
```cpp
CCurlActor::CResult Result = co_await CurlActor(&CCurlActor::f_ExecuteRequest, Request);
if (Result.m_StatusCode != 200)
{
	DMibErrorWebRequest("Request failed", CWebRequestExceptionData::fs_FromResult(Result));
}
```

### Async Operations
Use coroutines for async operations:
```cpp
// Direct co_await for simple cases
CCurlActor::CResult Result = co_await CurlActor(&CCurlActor::f_ExecuteRequest, Request);

// Or store future if you need to do other work first
TCFuture<CCurlActor::CResult> Future = CurlActor(&CCurlActor::f_ExecuteRequest, Request);
// Do other work here...
CCurlActor::CResult Result = co_await fg_Move(Future);
```

## Important Notes

- All network operations should use actors for thread safety
- WebSocket frame size limits are configurable via `CWebsocketSettings`
- HTTP server supports both IPv4 and Unix domain sockets
- AWS actors require proper IAM credentials with appropriate permissions
- FastCGI server is designed for nginx integration
- ACME implementation supports HTTP-01 and DNS-01 challenges
- Virtual socket factories enable comprehensive unit testing
- The module uses Malterlib's memory management with secure allocators for sensitive data
