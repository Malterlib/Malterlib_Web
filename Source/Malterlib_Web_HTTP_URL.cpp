// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_URL.h"

namespace NMib::NWeb::NHTTP
{
	//
	// Static Utility Methods
	//

	// _Start can range from 0 -> Len-1
	// _End can range from 0 -> Len or be -1 which == Len
	static bool fg_ParseU16Base10(uint16 &_oNum, NStr::CStr const &_Str, aint _Start, aint _End);

	//
	// CURL Public Methods
	//

	CURL::CURL()
	{

	}

	CURL::CURL(NStr::CStr const &_URL)
	{
		f_Decode(_URL);
	}

	CURL::CURL(CURL const &_ToCopy)
	{
		*this = _ToCopy;
	}

	CURL::CURL(CURL &&_ToMove)
	{
		*this = fg_Move(_ToMove);
	}

	CURL::~CURL()
	{
		f_Clear();
	}


	CURL &CURL::operator =(NStr::CStr const &_URL)
	{
		f_Decode(_URL);
		return *this;
	}

	CURL &CURL::operator =(CURL const &_ToCopy)
	{
		mp_Flags = _ToCopy.mp_Flags;
		mp_Scheme = _ToCopy.mp_Scheme;
		mp_Host = _ToCopy.mp_Host;
		mp_Port = _ToCopy.mp_Port;
		mp_Username = _ToCopy.mp_Username;
		mp_Password = _ToCopy.mp_Password;
		mp_Paths = _ToCopy.mp_Paths;
		mp_Query = _ToCopy.mp_Query;
		mp_Fragment = _ToCopy.mp_Fragment;
		return *this;
	}

	CURL &CURL::operator =(CURL &&_ToMove)
	{
		mp_Flags = _ToMove.mp_Flags;
		mp_Scheme = fg_Move(_ToMove.mp_Scheme);
		mp_Host = fg_Move(_ToMove.mp_Host);
		mp_Port = _ToMove.mp_Port;
		mp_Username = fg_Move(_ToMove.mp_Username);
		mp_Password = fg_Move(_ToMove.mp_Password);
		mp_Paths = fg_Move(_ToMove.mp_Paths);
		mp_Query = fg_Move(_ToMove.mp_Query);
		mp_Fragment = fg_Move(_ToMove.mp_Fragment);
		_ToMove.f_Clear();
		return *this;
	}

	EURLFlag CURL::f_GetFlags() const
	{
		return mp_Flags;
	}

	bool CURL::f_IsValid() const
	{
		return mp_Flags & EURLFlag_Valid;
	}

	// Does the URL have a scheme, host & path?
	bool CURL::f_IsFullURL() const
	{
		return f_HasAll(EURLFlag_Scheme | EURLFlag_Host | EURLFlag_Path);
	}

	void CURL::f_Clear()
	{
		mp_Flags = EURLFlag_None;
		mp_Scheme.f_Clear();
		mp_Host.f_Clear();
		mp_Port = 0;
		mp_Username.f_Clear();
		mp_Password.f_Clear();
		mp_Paths.f_Clear();
		mp_Query.f_Clear();
		mp_Fragment.f_Clear();
	}

	bool CURL::f_Decode(NStr::CStr const &_URL, EURLFlag _Flags)
	{
		f_Clear();
		// The point of all of this is to touch the memory manager as little as possible.
		// When we have a substring class this can be reduced even more (i.e. only when we decode something)

		mp_Flags = _Flags;

		NStr::CStr const &URL = _URL;
		mint URLLength = URL.f_GetLen();

		// Find all the various split points for the main components:

		aint iColonSlashSlash = URL.f_Find("://");

		auto fParseHost = [&](mint _iStart, ch8 _EndChar, mint _MaxLen) -> aint
			{
				ch8 const *pParse = URL.f_GetStr() + _iStart;
				ch8 const *pMaxLen = pParse + _MaxLen;
				while (*pParse && pParse < pMaxLen)
				{
					if (*pParse == _EndChar)
						break;
					else if (*pParse == '[')
					{
						while (*pParse && pParse < pMaxLen)
						{
							if (*pParse == ']')
								break;
							++pParse;
						}
						continue;
					}
					++pParse;
				}
				if (!*pParse || pParse == pMaxLen)
					return -1;
				return pParse - URL.f_GetStr();
			}
		;

		aint iPathStartSlash = fParseHost((iColonSlashSlash >= 0) ? (iColonSlashSlash + 3) : 0, '/', URL.f_GetLen());

		if (iPathStartSlash < 0)
			return false;

		aint iQueryMark = URL.f_Find(iPathStartSlash + 1, "?");

		aint iFragmentMark = URL.f_Find( fg_Max(iQueryMark + 1, iPathStartSlash + 1), "#");

		aint iPathEnd = (iQueryMark >= 0)
							? iQueryMark
							: ( (iFragmentMark >= 0) ? iFragmentMark : URLLength);


		if (iColonSlashSlash >= 0)
		{
			// Extract scheme:
			if (!fs_PercentDecode(mp_Scheme, URL, 0, iColonSlashSlash))
				return false;

			mp_Flags |= EURLFlag_Scheme;

			// Extract User, Pass:
			aint iUserPassHostStart = iColonSlashSlash + 3;
			NStr::CStr UserPassHost = URL.f_Slice(iColonSlashSlash + 3, iPathStartSlash);

			aint iHostStart = iUserPassHostStart;

			aint iUserPassAtMark = URL.f_Find(iUserPassHostStart, "@", iPathStartSlash - iUserPassHostStart);
			if (iUserPassAtMark >= 0)
			{
				aint iUserColonMark = URL.f_Find(iUserPassHostStart, ":", iUserPassAtMark - iUserPassHostStart);
				if (iUserColonMark >= 0)
				{ // URL has username & password
					if (!fs_PercentDecode(mp_Username, URL, iUserPassHostStart, iUserColonMark))
						return false;
					if (!fs_PercentDecode(mp_Password, URL, iUserColonMark + 1, iUserPassAtMark))
						return false;

					mp_Flags |= EURLFlag_Username | EURLFlag_Password;
				}
				else
				{ // URL has username
					if (!fs_PercentDecode(mp_Username, URL, iUserPassHostStart, iUserPassAtMark))
						return false;

					mp_Flags |= EURLFlag_Password;
				}

				iHostStart = iUserPassAtMark + 1;
			}

			// Extract Host & port:
			if (mp_Flags & EURLFlag_HostRaw)
				mp_Host = URL.f_Extract(iHostStart, iPathStartSlash - iHostStart);
			else
			{
				mint iHostStartTrimmed = iHostStart;
				mint iHostEndTrimmed = iPathStartSlash;
				aint iPortColonMark = fParseHost(iHostStart, ':', iPathStartSlash - iHostStart);
				if (iPortColonMark >= 0)
				{ // Has port
					iHostStartTrimmed = iHostStart;
					iHostEndTrimmed = iPortColonMark;

					if (!fg_ParseU16Base10(mp_Port, URL, iPortColonMark + 1, iPathStartSlash))
						return false; // Invalid port

					mp_Flags |= EURLFlag_Host | EURLFlag_Port;
				}
				else
					mp_Flags |= EURLFlag_Host;

				if (URL[iHostStartTrimmed] == '[')
				{
					mp_Flags |= EURLFlag_HostBrackets;
					++iHostStartTrimmed;
					if (URL[iHostEndTrimmed-1] == ']')
						--iHostEndTrimmed;
				}
				if (!fs_PercentDecode(mp_Host, URL, iHostStartTrimmed, iHostEndTrimmed))
					return false;
			}
		}

		// Extract path

		aint iCurSegmentStart = iPathStartSlash + 1;
		aint iCurSegmentEnd;
		NStr::CStr PathSegment;

		while ( iCurSegmentStart < iPathEnd )
		{
			iCurSegmentEnd = URL.f_Find(iCurSegmentStart, "/", iPathEnd - iCurSegmentStart);

			if (iCurSegmentEnd == -1)
				iCurSegmentEnd = iPathEnd;

			if (!fs_PercentDecode(PathSegment, URL, iCurSegmentStart, iCurSegmentEnd))
				return false;
			mp_Paths.f_Insert( fg_Move(PathSegment) );

			iCurSegmentStart = iCurSegmentEnd + 1;
		}

		mp_Flags |= EURLFlag_Path;

		// Extract Query
		if (iQueryMark >= 0)
		{
			ch8 const *pParse = URL.f_GetStr() + iQueryMark + 1;
			ch8 const *pEnd = URL.f_GetStr() + ((iFragmentMark >= 0) ? iFragmentMark : URLLength);
			while (pParse < pEnd)
			{
				auto pStart = pParse;
				while (pParse < pEnd && *pParse != '&')
					++pParse;

				auto pStartValue = pStart;
				while (pStartValue < pParse && *pStartValue != '=')
					++pStartValue;

				NStr::CStr Key;
				NStr::CStr Value;
				if (pStartValue == pParse)
				{
					if (!fs_PercentDecode(Key, URL, pStart - URL.f_GetStr(), pParse - URL.f_GetStr()))
						return false;
				}
				else
				{
					if (!fs_PercentDecode(Key, URL, pStart - URL.f_GetStr(), pStartValue - URL.f_GetStr()))
						return false;
					if (!fs_PercentDecode(Value, URL, (pStartValue + 1) - URL.f_GetStr(), pParse - URL.f_GetStr()))
						return false;
				}

				mp_Query.f_Insert({fg_Move(Key), fg_Move(Value)});

				if (*pParse == '&')
					++pParse;
			}

			mp_Flags |= EURLFlag_Query;
		}

		// Extract Fragment
		if (iFragmentMark >= 0)
		{
			if (!fs_PercentDecode(mp_Fragment, URL, iFragmentMark + 1, URLLength))
				return false;
			mp_Flags |= EURLFlag_Fragment;
		}

		mp_Flags |= EURLFlag_Valid;

		return true;
	}

	[[maybe_unused]] static bool fg_IsValidScheme(NStr::CStr const &_Scheme)
	{
		auto *pParse = _Scheme.f_GetStr();
		while (*pParse)
		{
			if (NStr::fg_CharIsAnsiAlphabetical(*pParse) || NStr::fg_CharIsNumber(*pParse) || *pParse == '+' || *pParse == '-' || *pParse == '.')
				++pParse;
			else
				return false;
		}

		return true;
	}

	NStr::CStr CURL::f_Encode(EEncodeFlag _Flags) const
	{
		NStr::CStr Output;

		// <scheme>://[<username>[:<password>]@]<server>[:<port>]/<path>[?<query>][#<fragmentID>]

		if (mp_Flags & EURLFlag_Scheme)
		{
			//ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
			DMibCheck(fg_IsValidScheme(mp_Scheme));

			Output += mp_Scheme;
			Output += "://";
		}

		if (mp_Flags & EURLFlag_Username)
		{
			fs_PercentEncode(Output, mp_Username, nullptr, _Flags);
			if (mp_Flags & EURLFlag_Password)
			{
				Output += ":";
				fs_PercentEncode(Output, mp_Password, nullptr, _Flags);
			}
			Output += "@";
		}
		if (mp_Flags & EURLFlag_Host)
		{
			if (mp_Flags & EURLFlag_HostBrackets)
			{
				Output += "[";
				fs_PercentEncode(Output, mp_Host, "[]%", _Flags);
				Output += "]";
			}
			else if (mp_Flags & EURLFlag_HostRaw)
				Output += mp_Host;
			else
				fs_PercentEncode(Output, mp_Host, nullptr, _Flags);
			if (mp_Flags & EURLFlag_Port)
				Output += NStr::CStr::CFormat(":{}") << mp_Port;
		}

		if ((mp_Flags & EURLFlag_Path) && !mp_Paths.f_IsEmpty())
		{
			for (auto &Path : mp_Paths)
			{
				Output += "/";
				fs_PercentEncode(Output, Path, nullptr, _Flags);
			}
		}
		else
			Output += "/";

		if (mp_Flags & EURLFlag_Query)
		{
			Output += "?";
			bool bFirst = true;
			for (auto &Entry : mp_Query)
			{
				if (!bFirst)
				{
					Output += "&";
				}
				else
					bFirst = false;
				fs_PercentEncode(Output, Entry.m_Key, nullptr, _Flags);
				Output += "=";
				fs_PercentEncode(Output, Entry.m_Value, nullptr, _Flags);
			}
		}
		if (mp_Flags & EURLFlag_Fragment)
		{
			Output += "#";
			fs_PercentEncode(Output, mp_Fragment, nullptr, _Flags);
		}

		return Output;
	}

	// Test if the URL has a field:

	bool CURL::f_HasAll(EURLFlag _Flags) const
	{
		return (mp_Flags & _Flags) == _Flags;
	}

	bool CURL::f_HasAny(EURLFlag _Flags) const
	{
		return (mp_Flags & _Flags);
	}

	bool CURL::f_HasScheme() const
	{
		return mp_Flags & EURLFlag_Scheme;
	}

	bool CURL::f_HasHost() const
	{
		return mp_Flags & EURLFlag_Host;
	}

	bool CURL::f_HasPort() const
	{
		return mp_Flags & EURLFlag_Port;
	}

	bool CURL::f_HasUsername() const
	{
		return mp_Flags & EURLFlag_Username;
	}

	bool CURL::f_HasPassword() const
	{
		return mp_Flags & EURLFlag_Password;
	}

	bool CURL::f_HasPath() const
	{
		return mp_Flags & EURLFlag_Path;
	}

	bool CURL::f_HasQuery() const
	{
		return mp_Flags & EURLFlag_Query;
	}

	bool CURL::f_HasFragment() const
	{
		return mp_Flags & EURLFlag_Fragment;
	}

	// Access an URL field:

	NStr::CStr const &CURL::f_GetScheme() const
	{
		return mp_Scheme;
	}

	NStr::CStr const &CURL::f_GetHost() const
	{
		return mp_Host;
	}

	uint16 CURL::f_GetPort() const
	{
		return mp_Port;
	}

	namespace
	{
		struct CSchemeToPort
		{
			ch8 const *m_pScheme;
			uint16 m_Port;
		};

		const CSchemeToPort g_SchemeToPort[] =
			{
				{"ws", 80}
				, {"wss", 443}
				, {"http", 80}
				, {"https", 443}
			}
		;
	}

	uint16 CURL::f_GetPortFromScheme() const
	{
		if (mp_Port)
			return mp_Port;

		for (auto &Scheme : g_SchemeToPort)
		{
			if (NStr::fg_StrCmpNoCase(mp_Scheme, Scheme.m_pScheme) == 0)
				return Scheme.m_Port;
		}

		return 0;
	}

	NStr::CStr const &CURL::f_GetUsername() const
	{
		return mp_Username;
	}

	NStr::CStr const &CURL::f_GetPassword() const
	{
		return mp_Password;
	}

	NContainer::TCVector<NStr::CStr> const &CURL::f_GetPath() const
	{
		return mp_Paths;
	}

	NStr::CStr CURL::f_GetFullPathPercentEncoded(EEncodeFlag _Flags) const
	{
		NStr::CStr FullPath;
		for (auto &Path : mp_Paths)
		{
			NStr::CStr Encoded;
			fs_PercentEncode(Encoded, Path, nullptr, _Flags);
			fg_AddStrSep(FullPath, Encoded, "/");
		}
		return "/" + FullPath;
	}

	NStr::CStr CURL::f_GetFullPath() const
	{
		NStr::CStr FullPath;
		for (auto &Path : mp_Paths)
			fg_AddStrSep(FullPath, Path, "/");

		return "/" + FullPath;
	}

	NContainer::TCVector<CURL::CQueryEntry> const &CURL::f_GetQuery() const
	{
		return mp_Query;
	}

	NStr::CStr const &CURL::f_GetFragment() const
	{
		return mp_Fragment;
	}

	// Set an URL field:

	void CURL::f_SetScheme(NStr::CStr const &_Scheme)
	{
		mp_Scheme = _Scheme;
		mp_Flags |= EURLFlag_Scheme;
	}

	void CURL::f_SetHost(NStr::CStr const &_Host, bool _bRaw)
	{
		mp_Host = _Host;
		mp_Flags |= EURLFlag_Host;
		mp_Flags &= ~EURLFlag_HostBrackets;
		if (_bRaw)
			mp_Flags |= EURLFlag_HostRaw;
		else
			mp_Flags &= ~EURLFlag_HostRaw;
	}

	void CURL::f_SetPort(uint16 _Port)
	{
		mp_Port = _Port;
		mp_Flags |= EURLFlag_Port;
	}

	void CURL::f_SetUsername(NStr::CStr const &_Username)
	{
		mp_Username = _Username;
		if (mp_Username.f_IsEmpty())
			mp_Flags &= ~EURLFlag_Username;
		else
			mp_Flags |= EURLFlag_Username;
	}

	void CURL::f_SetPassword(NStr::CStr const &_Password)
	{
		mp_Password = _Password;
		mp_Flags |= EURLFlag_Password;
	}

	void CURL::f_SetPath(NContainer::TCVector<NStr::CStr> const &_Path)
	{
		mp_Paths = _Path;
		mp_Flags |= EURLFlag_Path;
	}

	void CURL::f_SetPath(NContainer::TCVector<NStr::CStr> && _Path)
	{
		mp_Paths = fg_Move(_Path);
		mp_Flags |= EURLFlag_Path;
	}

	void CURL::f_AppendPath(NContainer::TCVector<NStr::CStr> const &_Path)
	{
		mp_Paths.f_Insert(_Path);
		mp_Flags |= EURLFlag_Path;
	}

	void CURL::f_SetQuery(NContainer::TCVector<CQueryEntry> const &_Query)
	{
		mp_Query = _Query;
		mp_Flags |= EURLFlag_Query;
	}

	void CURL::f_AddQueryEntry(CQueryEntry const &_QueryEntry)
	{
		for (auto &Entry : mp_Query)
		{
			if (Entry.m_Key == _QueryEntry.m_Key)
			{
				Entry.m_Value = _QueryEntry.m_Value;
				return;
			}
		}

		mp_Query.f_Insert(_QueryEntry);
		mp_Flags |= EURLFlag_Query;
	}

	void CURL::f_SetFragment(NStr::CStr const &_Fragment)
	{
		mp_Fragment = _Fragment;
		mp_Flags |= EURLFlag_Fragment;
	}

	// Clear an URL field:

	void CURL::f_ClearScheme()
	{
		mp_Scheme.f_Clear();
		mp_Flags &= ~EURLFlag_Scheme;
	}

	void CURL::f_ClearHost()
	{
		mp_Host.f_Clear();
		mp_Flags &= ~EURLFlag_Host;
	}

	void CURL::f_ClearPort()
	{
		mp_Port = 0;
		mp_Flags &= ~EURLFlag_Port;
	}

	void CURL::f_ClearUsername()
	{
		mp_Username.f_Clear();
		mp_Flags &= ~EURLFlag_Username;
	}

	void CURL::f_ClearPassword()
	{
		mp_Password.f_Clear();
		mp_Flags &= ~EURLFlag_Password;
	}

	void CURL::f_ClearPath()
	{
		mp_Paths.f_Clear();
		mp_Flags &= ~EURLFlag_Path;
	}

	void CURL::f_ClearQuery()
	{
		mp_Query.f_Clear();
		mp_Flags &= ~EURLFlag_Query;
	}

	void CURL::f_ClearFragment()
	{
		mp_Fragment.f_Clear();
		mp_Flags &= ~EURLFlag_Fragment;
	}

	//
	// Utility Methods
	//

	static bool fg_DecodeHexChar(char &_oNum, char _Ch)
	{
		if (_Ch >= '0' && _Ch <= '9')
			_oNum = _Ch - '0';
		else if (_Ch >= 'a' && _Ch <= 'f')
			_oNum = (_Ch - 'a') + 10;
		else if (_Ch >= 'A' && _Ch <= 'F')
			_oNum = (_Ch - 'A') + 10;
		else
			return false;

		return true;
	}

	// _Start can range from 0 -> Len-1
	// _End can range from 0 -> Len or be -1 which == Len
	bool fg_ParseU16Base10(uint16 &o_Num, NStr::CStr const &_Str, aint _Start, aint _End)
	{
		mint StrLen = _Str.f_GetLen();

		if (_End < 0)
			_End = StrLen;

		if (_Start < 0 || _End < 0)
			return false;

		if (mint(_Start) > StrLen)
			_Start = StrLen;

		if (mint(_End) > StrLen)
			_End = StrLen;

		if (_Start >= _End)
			return false;

		char const *pStart = _Str.f_GetStr() + _Start;
		char const *pEnd = _Str.f_GetStr() + _End;

		char const *pPtr = pEnd - 1;

		uint16 Result = 0;
		uint16 Multiplier = 1;
		char Ch;

		while (pPtr >= pStart)
		{
			Ch = *pPtr;
			if (Ch < '0' || Ch > '9')
				return false; // Error: Invalid number

			Result += uint16(Ch - '0') * Multiplier;

			Multiplier *= 10;
			--pPtr;
		}

		o_Num = Result;

		return true;
	}

	void CURL::f_DebugOut() const
	{
		NStr::CStr Path;

		for (auto Segment : mp_Paths)
		{
			Path += "/";
			Path += Segment;
		}

		NStr::CStr Text = NStr::fg_Format
			(
				"URL:\n"
				"	Scheme: {}\n"
				"	User:	{}\n"
				"	Pass:	{}\n"
				"	Host:	{}\n"
				"	Port:	{}\n"
				"	Path:	{}\n"
				"	Query:	{}\n"
				"	Frag:	{}\n"
				, f_HasScheme() ? mp_Scheme.f_GetStr() : "<None>"
				, f_HasUsername() ? mp_Username.f_GetStr() : "<None>"
				, f_HasPassword() ? mp_Password.f_GetStr() : "<None>"
				, f_HasHost() ? mp_Host.f_GetStr() : "<None>"
				, f_HasPort() ? NStr::fg_Format("{}", mp_Port) : NStr::CStr("<None>")
				, f_HasPath() ? Path.f_GetStr() : "<None>"
				, f_HasQuery() ? mp_Query : fg_Default()
				, f_HasFragment() ? mp_Fragment.f_GetStr() : "<None>"
			)
		;
		NSys::fg_DebugOutput(Text.f_GetStr());
	}

	//
	// CURL Public Static Methods
	//

	// _Start can range from 0 -> Len-1
	// _End can range from 0 -> Len or be -1 which == Len
	bool CURL::fs_PercentDecode(NStr::CStr &_oResult, NStr::CStr const &_Str, aint _Start, aint _End)
	{
		mint StrLen = _Str.f_GetLen();

		if (_End < 0)
			_End = StrLen;

		if (_Start < 0 || _End < 0)
			return false;

		if (mint(_Start) > StrLen)
			_Start = StrLen;

		if (mint(_End) > StrLen)
			_End = StrLen;

		if (_Start >= _End)
			return false;

		char const* pStart = _Str.f_GetStr() + _Start;
		char const* pPtr = pStart;
		char const* pEnd = _Str.f_GetStr() + _End;

		char const* pRunStart = pPtr;

		NStr::CStr Result;
		char Ch0, Ch1;

		while (pPtr != pEnd)
		{
			if (*pPtr == '%')
			{
				// End current run
				if (pPtr > pRunStart)
					Result += NStr::CStr(pRunStart, pPtr - pRunStart);

				if ( (pPtr + 2) >= pEnd)
				{
					// Error: No room for encoded char
					return false;
				}

				if
					(
						!fg_DecodeHexChar(Ch0, pPtr[1])
						|| !fg_DecodeHexChar(Ch1, pPtr[2])
					)
				{
					return false; // Error: Invalid encoding chars.
				}

				char Code = ( Ch0 << 4 ) + Ch1;

				Result.f_AddChar(Code);

				pPtr += 3;
				pRunStart = pPtr;
			}
			else
			{
				++pPtr;
			}
		}

		if (pRunStart < pPtr)
		{
			// Check for the case of returning the whole string to get an implicit copy.
			if
				(
					pRunStart == _Str.f_GetStr()
					&& pPtr == (_Str.f_GetStr() + _Str.f_GetLen())
				)
			{
				Result = _Str;
			}
			else
			{
				Result += NStr::CStr(pRunStart, pPtr - pRunStart);
			}
		}

		_oResult = fg_Move(Result);
		return true;
	}

	void CURL::fs_PercentEncode(NStr::CStr &o_Result, NStr::CStr const &_Str, ch8 const *_pReserved, EEncodeFlag _Flags)
	{
		NStr::CStr Str = _Str;

		auto pParse = Str.f_GetStr();

		ch8 const *pReserved = ":/?#[]@!$&'()*+,;=%";

		if (_pReserved)
			pReserved = _pReserved;

		while (*pParse)
		{
			uch8 Character = (uch8)*pParse;
			if (Character <= 32 || Character >= 128 || NStr::fg_StrFindChar(pReserved, Character) >= 0)
				o_Result += NStr::CStr::CFormat((_Flags & EEncodeFlag_UpperCasePercentEncode) ? "%{nfh,sf0,sj2,nc}" : "%{nfh,sf0,sj2}") << Character;
			else
				o_Result.f_AddChar(*pParse);
			++pParse;
		}

		if (_Flags & EEncodeFlag_DoublePercentEncode)
		{
			NStr::CStr Temp;
			fs_PercentEncode(Temp, o_Result, _pReserved, _Flags & ~EEncodeFlag_DoublePercentEncode);
			o_Result = fg_Move(Temp);
		}
	}

	NStr::CStr CURL::fs_GetQueryPercentEncoded(NContainer::TCVector<CQueryEntry> const &_QueryEntries, EEncodeFlag _Flags)
	{
		NStr::CStr Output;

		bool bFirst = true;
		for (auto &Entry : _QueryEntries)
		{
			if (!bFirst)
				Output += "&";
			else
				bFirst = false;

			fs_PercentEncode(Output, Entry.m_Key, nullptr, _Flags);
			Output += "=";
			fs_PercentEncode(Output, Entry.m_Value, nullptr, _Flags);
		}

		return Output;
	}
}
