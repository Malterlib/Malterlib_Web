// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Web/HTTP/URL>

using namespace NMib;
using namespace NMib::NStr;
using namespace NMib::NWeb;

// RFC 3986 coverage for NHTTP::CURL. Assertions reflect the parser's documented model:
//   <scheme>://[<user>[:<pass>]@]<host>[:<port>]/<path>[?<query>][#<fragment>]
// where the path is a list of percent-decoded segments and an empty path (path-abempty) is valid.
class CURL_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("Scheme")
		{
			{
				DMibTestPath("https");
				NHTTP::CURL Url("https://host/");
				DMibExpectTrue(Url.f_HasScheme());
				DMibExpect(Url.f_GetScheme(), ==, "https");
			}

			{
				DMibTestPath("case preserved");
				NHTTP::CURL Url("HtTpS://host/");
				DMibExpect(Url.f_GetScheme(), ==, "HtTpS");
			}

			{
				DMibTestPath("custom scheme with + - .");
				NHTTP::CURL Url("git+ssh://host/repo");
				DMibExpect(Url.f_GetScheme(), ==, "git+ssh");
			}
		};

		DMibTestSuite("Host and port")
		{
			{
				DMibTestPath("host only");
				NHTTP::CURL Url("https://example.com/");
				DMibExpectTrue(Url.f_HasHost());
				DMibExpect(Url.f_GetHost(), ==, "example.com");
				DMibExpectFalse(Url.f_HasPort());
			}

			{
				DMibTestPath("host and port");
				NHTTP::CURL Url("https://example.com:8443/");
				DMibExpect(Url.f_GetHost(), ==, "example.com");
				DMibExpectTrue(Url.f_HasPort());
				DMibExpect(Url.f_GetPort(), ==, uint16(8443));
			}

			{
				DMibTestPath("percent decoded host");
				NHTTP::CURL Url("https://ex%41mple.com/");
				DMibExpect(Url.f_GetHost(), ==, "exAmple.com");
			}
		};

		DMibTestSuite("IPv6 host")
		{
			{
				DMibTestPath("brackets with port");
				NHTTP::CURL Url("http://[2001:db8::1]:8080/path");
				DMibExpectTrue(Url.f_HasHost());
				DMibExpect(Url.f_GetHost(), ==, "2001:db8::1");
				DMibExpect(Url.f_GetPort(), ==, uint16(8080));
			}

			{
				DMibTestPath("brackets without port");
				NHTTP::CURL Url("http://[::1]/");
				DMibExpect(Url.f_GetHost(), ==, "::1");
				DMibExpectFalse(Url.f_HasPort());
			}
		};

		DMibTestSuite("Userinfo")
		{
			{
				DMibTestPath("user and password");
				NHTTP::CURL Url("https://user:secret@host/");
				DMibExpectTrue(Url.f_HasUsername());
				DMibExpect(Url.f_GetUsername(), ==, "user");
				DMibExpectTrue(Url.f_HasPassword());
				DMibExpect(Url.f_GetPassword(), ==, "secret");
				DMibExpect(Url.f_GetHost(), ==, "host");
			}

			{
				DMibTestPath("user only");
				NHTTP::CURL Url("https://user@host/");
				DMibExpectTrue(Url.f_HasUsername());
				DMibExpect(Url.f_GetUsername(), ==, "user");
				DMibExpectFalse(Url.f_HasPassword());
				DMibExpect(Url.f_GetHost(), ==, "host");
			}

			{
				DMibTestPath("percent decoded userinfo");
				NHTTP::CURL Url("https://us%20er:pa%40ss@host/");
				DMibExpect(Url.f_GetUsername(), ==, "us er");
				DMibExpect(Url.f_GetPassword(), ==, "pa@ss");
			}
		};

		DMibTestSuite("Path")
		{
			{
				DMibTestPath("multiple segments");
				NHTTP::CURL Url("https://host/a/b/c");
				DMibExpect(Url.f_GetPath().f_GetLen(), ==, umint(3));
				DMibExpect(Url.f_GetFullPath(), ==, "/a/b/c");
			}

			{
				DMibTestPath("root path");
				NHTTP::CURL Url("https://host/");
				DMibExpect(Url.f_GetPath().f_GetLen(), ==, umint(0));
				DMibExpect(Url.f_GetFullPath(), ==, "/");
			}

			{
				DMibTestPath("percent decoded segment with encoded slash");
				NHTTP::CURL Url("https://host/a%2Fb/c");
				DMibExpect(Url.f_GetPath().f_GetLen(), ==, umint(2));
				DMibExpect(Url.f_GetPath()[0], ==, "a/b");
				DMibExpect(Url.f_GetPath()[1], ==, "c");
			}
		};

		DMibTestSuite("Query")
		{
			{
				DMibTestPath("key value pairs");
				NHTTP::CURL Url("https://host/?a=1&b=2");
				DMibExpectTrue(Url.f_HasQuery());
				DMibExpect(Url.f_GetQuery().f_GetLen(), ==, umint(2));
				DMibExpect(Url.f_GetQuery()[0].m_Key, ==, "a");
				DMibExpect(Url.f_GetQuery()[0].m_Value, ==, "1");
				DMibExpect(Url.f_GetQuery()[1].m_Key, ==, "b");
				DMibExpect(Url.f_GetQuery()[1].m_Value, ==, "2");
			}

			{
				DMibTestPath("empty value and key only");
				NHTTP::CURL Url("https://host/?a=&b");
				DMibExpect(Url.f_GetQuery().f_GetLen(), ==, umint(2));
				DMibExpect(Url.f_GetQuery()[0].m_Value, ==, "");
				DMibExpect(Url.f_GetQuery()[1].m_Key, ==, "b");
				DMibExpect(Url.f_GetQuery()[1].m_Value, ==, "");
			}

			{
				DMibTestPath("percent decoded value");
				NHTTP::CURL Url("https://host/?q=hello%20world%26more");
				DMibExpect(Url.f_GetQuery()[0].m_Value, ==, "hello world&more");
			}
		};

		DMibTestSuite("Fragment")
		{
			{
				DMibTestPath("simple fragment");
				NHTTP::CURL Url("https://host/path#section");
				DMibExpectTrue(Url.f_HasFragment());
				DMibExpect(Url.f_GetFragment(), ==, "section");
			}

			{
				DMibTestPath("percent decoded fragment");
				NHTTP::CURL Url("https://host/#a%20b");
				DMibExpect(Url.f_GetFragment(), ==, "a b");
			}

			{
				DMibTestPath("empty fragment");
				NHTTP::CURL Url("https://host/path#");
				DMibExpectTrue(Url.f_IsValid());
				DMibExpectTrue(Url.f_HasFragment());
				DMibExpect(Url.f_GetFragment(), ==, "");
			}
		};

		// RFC 3986 path-abempty: an authority-form URI may have an empty path (no trailing '/').
		DMibTestSuite("Empty path")
		{
			{
				DMibTestPath("host only");
				NHTTP::CURL Url("https://host");
				DMibExpectTrue(Url.f_IsValid());
				DMibExpectTrue(Url.f_HasHost());
				DMibExpect(Url.f_GetHost(), ==, "host");
				DMibExpect(Url.f_GetFullPath(), ==, "/");
			}

			{
				DMibTestPath("host and port");
				NHTTP::CURL Url("https://host:18443");
				DMibExpect(Url.f_GetHost(), ==, "host");
				DMibExpect(Url.f_GetPort(), ==, uint16(18443));
			}

			{
				DMibTestPath("empty path with query");
				NHTTP::CURL Url("https://host?a=1");
				DMibExpect(Url.f_GetHost(), ==, "host");
				DMibExpectFalse(Url.f_HasPort());
				DMibExpect(Url.f_GetQuery()[0].m_Key, ==, "a");
			}

			{
				DMibTestPath("empty path with fragment");
				NHTTP::CURL Url("https://host:8443#frag");
				DMibExpect(Url.f_GetHost(), ==, "host");
				DMibExpect(Url.f_GetPort(), ==, uint16(8443));
				DMibExpect(Url.f_GetFragment(), ==, "frag");
			}
		};

		DMibTestSuite("Default port from scheme")
		{
			{
				DMibTestPath("http default 80");
				NHTTP::CURL Url("http://host/");
				DMibExpect(Url.f_GetPortFromScheme(), ==, uint16(80));
			}

			{
				DMibTestPath("https default 443");
				NHTTP::CURL Url("https://host/");
				DMibExpect(Url.f_GetPortFromScheme(), ==, uint16(443));
			}

			{
				DMibTestPath("wss default 443");
				NHTTP::CURL Url("wss://host/");
				DMibExpect(Url.f_GetPortFromScheme(), ==, uint16(443));
			}

			{
				DMibTestPath("explicit port overrides scheme default");
				NHTTP::CURL Url("https://host:9000/");
				DMibExpect(Url.f_GetPortFromScheme(), ==, uint16(9000));
			}
		};

		DMibTestSuite("Encode round-trip")
		{
			{
				DMibTestPath("full url is stable");
				NStr::CStr Input = "https://example.com:8443/api/path?x=1&y=2#frag";
				NHTTP::CURL Url(Input);
				DMibExpect(Url.f_Encode(), ==, Input);
			}

			{
				DMibTestPath("empty path encodes a trailing slash");
				NHTTP::CURL Url("https://host:8443");
				DMibExpect(Url.f_Encode(), ==, "https://host:8443/");
			}
		};

		DMibTestSuite("Invalid input")
		{
			{
				DMibTestPath("empty string");
				NHTTP::CURL Url;
				DMibExpectFalse(Url.f_Decode(""));
				DMibExpectFalse(Url.f_IsValid());
			}

			{
				DMibTestPath("scheme-less reference without separator");
				NHTTP::CURL Url;
				DMibExpectFalse(Url.f_Decode("notaurl"));
			}
		};
	}
};

DMibTestRegister(CURL_Tests, Malterlib::Web);
