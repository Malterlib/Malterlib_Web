// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_HTTP.h"
#include <utility>

namespace NMib::NWeb::NHTTP
{
	// Lookup tables.

	// RFC 2616 Section 9 Method Definitions
	char const* gc_HTTPMethodNames[] =
			{
					""
				,	"OPTIONS"
				, 	"GET"
				,	"HEAD"
				,	"POST"
				,	"PUT"
				,	"DELETE"
				,	"TRACE"
				,	"CONNECT"
				,	nullptr
			};



	// RFC 2616 Section 3.1 HTTP Version
	char const* gc_HTTPVersions[] =
			{
					""
				,	"HTTP/1.0"
				,	"HTTP/1.1"
				,	"HTTP/2.0"
				,	nullptr
			};

	// RFC 2616 Section 3.6 & 14.41 Transfer Encodings
	char const* gc_HTTPTransferEncodings[] =
			{
					""
				,	"chunked"
				,	"identity"
				,	"gzip"
				,	"deflate"
				,	"compress"
				,	nullptr
			};


	// RFC 2616 Section 14.10 Connection
	char const* gc_HTTPConnectionTokens[] =
			{
					""
				,	"keep-alive"
				,	"close"
				,	"Upgrade"
				,	nullptr
			};

	// RFC 2616 Section 4.5 General Header Fields
	char const* gc_HTTPGeneralFields[] =
			{
					""
				,	"Cache-Control"
				,	"Connection"
				,	"Date"
				,	"Pragma"
				,	"Trailer"
				,	"Transfer-Encoding"
				,	"Upgrade"
				,	"Via"
				,	"Warning"
				,	nullptr
			};

	// RFC 2616 Section 5.4 RequestHeaderFields
	char const* gc_HTTPRequestFields[] =
		{
				""
			,	"Accept"
			,	"Accept-Charset"
			,	"Accept-Encoding"
			,	"Accept-Language"
			,	"Authorization"
			,	"Expect"
			,	"From"
			,	"Host"
			,	"If-Match"
			,	"If-Modified-Since"
			,	"If-None-Match"
			,	"If-Range"
			,	"If-Unmodified-Since"
			,	"Max-Forwards"
			,	"Proxy-Authorization"
			,	"Range"
			,	"Referer"
			,	"TE"
			,	"User-Agent"
			,	nullptr
		};

	// RFC 2616 Section 6.2 Response Header Fields
	char const* gc_HTTPResponseFields[] =
		{
			""
			, "Accept-Ranges"
			, "Age"
			, "ETag"
			, "Location"
			, "Proxy-Authenticate"
			, "Retry-After"
			, "Server"
			, "Vary"
			, "WWW-Authenticate"
			, nullptr
		};

	// RFC 2616 Section 7.1 Entity Header Fields
	char const* gc_HTTPEntityFields[] =
		{
				""
			,	"Content-Encoding"
			,	"Content-Language"
			,	"Content-Length"
			,	"Content-Location"
			,	"Content-MD5"
			,	"Content-Range"
			,	"Content-Type"
			,	"Expires"
			,	"Last-Modified"
			,	"extension-header"
			,	nullptr
		};

	// RFC 2616 Section 6.1.1 Status Code and Reason Phrase
	static std::pair<uint32, char const*> const gc_HTTPReasonPhrases[] =
		{
			  { 100, "Continue" }
			, { 101, "Switching Protocols" }
			, { 200, "OK" }
			, { 201, "Created" }
			, { 202, "Accepted" }
			, { 203, "Non-Authoritative Information" }
			, { 204, "No Content" }
			, { 205, "Reset Content" }
			, { 206, "Partial Content" }
			, { 300, "Multiple Choices" }
			, { 301, "Moved Permanently" }
			, { 302, "Found" }
			, { 303, "See Other" }
			, { 304, "Not Modified" }
			, { 305, "Use Proxy" }
			, { 307, "Temporary Redirect" }
			, { 400, "Bad Request" }
			, { 401, "Unauthorized" }
			, { 402, "Payment Required" }
			, { 403, "Forbidden" }
			, { 404, "Not Found" }
			, { 405, "Method Not Allowed" }
			, { 406, "Not Acceptable" }

			, { 407, "Proxy Authentication Required" }
			, { 408, "Request Time-out" }
			, { 409, "Conflict" }
			, { 410, "Gone" }
			, { 411, "Length Required" }
			, { 412, "Precondition Failed" }
			, { 413, "Request Entity Too Large" }
			, { 414, "Request-URI Too Large" }
			, { 415, "Unsupported Media Type" }
			, { 416, "Requested range not satisfiable" }
			, { 417, "Expectation Failed" }
			, { 500, "Internal Server Error" }
			, { 501, "Not Implemented" }
			, { 502, "Bad Gateway" }
			, { 503, "Service Unavailable" }
			, { 504, "Gateway Time-out" }
			, { 505, "HTTP Version not supported" }
			, { 0, nullptr }
		};

	//
	// Utility Methods
	//


	template<typename t_EnumType = mint>
	static t_EnumType fg_FindInStringList(NStr::CStr const& _FindThis, char const** _pInHere, t_EnumType _ReturnIfNotFound)
	{
		for (	char const** pCur = _pInHere
			;	*pCur
			;	++pCur)
		{
			if (_FindThis.f_CmpNoCase(*pCur) == 0)
				return t_EnumType(pCur - _pInHere);
		}

		return _ReturnIfNotFound;
	}

	template<typename t_EnumType = mint>
	static t_EnumType fg_FindInStringListNoCase(NStr::CStr const& _FindThis, char const** _pInHere, t_EnumType _ReturnIfNotFound)
	{
		for (	char const** pCur = _pInHere
			;	*pCur
			;	++pCur)
		{
			if (_FindThis.f_CmpNoCase(*pCur) == 0)
				return t_EnumType(pCur - _pInHere);
		}

		return _ReturnIfNotFound;
	}



	char const* fg_HTTP_GetMethodName(EMethod _Method)
	{
		return gc_HTTPMethodNames[(int)_Method];
	}

	EMethod fg_HTTP_LookupMethod(NStr::CStr const& _Method)
	{
		return fg_FindInStringListNoCase(_Method, gc_HTTPMethodNames, EMethod_Unknown);
	}


	char const* fg_HTTP_GetVersionName(EVersion _Version)
	{
		return gc_HTTPVersions[(int)_Version];
	}

	EVersion fg_HTTP_LookupVersion(NStr::CStr const& _Version)
	{
		return fg_FindInStringListNoCase(_Version, gc_HTTPVersions, EVersion_Unknown);
	}


	char const* fg_HTTP_GetTransferEncodingName(ETransferEncoding _Encoding)
	{
		return gc_HTTPTransferEncodings[(int)_Encoding];
	}

	ETransferEncoding fg_HTTP_LookupTransferEncoding(NStr::CStr const& _Encoding)
	{
		return fg_FindInStringListNoCase(_Encoding, gc_HTTPTransferEncodings, ETransferEncoding_Unknown);
	}


	char const* fg_HTTP_GetConnectionTokenName(EConnectionToken _Token)
	{
		return gc_HTTPConnectionTokens[(int)_Token];
	}

	EConnectionToken fg_HTTP_LookupConnectionToken(NStr::CStr const& _Token)
	{
		return fg_FindInStringListNoCase(_Token, gc_HTTPConnectionTokens, EConnectionToken_Unknown);
	}


	char const* fg_HTTP_GetGeneralFieldName(EGeneralField _Field)
	{
		return gc_HTTPGeneralFields[(int)_Field];
	}

	EGeneralField fg_HTTP_LookupGeneralField(NStr::CStr const& _Field)
	{
		return fg_FindInStringListNoCase(_Field, gc_HTTPGeneralFields, EGeneralField_Unknown);
	}

	char const* fg_HTTP_GetRequestFieldName(ERequestField _Field)
	{
		return gc_HTTPRequestFields[(int)_Field];
	}

	ERequestField fg_HTTP_LookupRequestField(NStr::CStr const& _Field)
	{
		return fg_FindInStringListNoCase(_Field, gc_HTTPRequestFields, ERequestField_Unknown);
	}

	char const* fg_HTTP_GetResponseFieldName(EResponseField _Field)
	{
		return gc_HTTPResponseFields[(int)_Field];
	}

	EResponseField fg_HTTP_LookupResponseField(NStr::CStr const& _Field)
	{
		return fg_FindInStringListNoCase(_Field, gc_HTTPResponseFields, EResponseField_Unknown);
	}


	char const* fg_HTTP_GetEntityFieldName(EEntityField _Field)
	{
		return gc_HTTPEntityFields[(int)_Field];
	}

	EEntityField fg_HTTP_LookupEntityField(NStr::CStr const& _Field)
	{
		return fg_FindInStringListNoCase(_Field, gc_HTTPEntityFields, EEntityField_Unknown);
	}


	char const* fg_HTTP_GetReasonPhrase(EStatus _Status)
	{
		std::pair<uint32, char const*> const* pCurPair = gc_HTTPReasonPhrases;

		while (pCurPair->first > 0)
		{
			if (pCurPair->first == (uint32)_Status)
				return pCurPair->second;

			++pCurPair;
		}

		return "Unknown";
	}
}
