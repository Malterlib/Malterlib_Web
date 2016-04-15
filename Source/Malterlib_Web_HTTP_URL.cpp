// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Web_HTTP_URL.h"

namespace NMib
{
	namespace NHTTP
	{
		//
		// Static Utility Methods
		//

		// _Start can range from 0 -> Len-1
		// _End can range from 0 -> Len or be -1 which == Len			
		static bint fg_ParseU16Base10(uint16 &_oNum, NStr::CStr const &_Str, aint _Start, aint _End);

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
		
		bool CURL::operator == (CURL const &_Right) const
		{
			return
				NContainer::fg_TupleReferences
				(
					mp_Flags
					, mp_Scheme
					, mp_Host
					, mp_Port
					, mp_Username
					, mp_Password
					, mp_Paths
					, mp_Query
					, mp_Fragment
				)
				==
				NContainer::fg_TupleReferences
				(
					_Right.mp_Flags
					, _Right.mp_Scheme
					, _Right.mp_Host
					, _Right.mp_Port
					, _Right.mp_Username
					, _Right.mp_Password
					, _Right.mp_Paths
					, _Right.mp_Query
					, _Right.mp_Fragment
				)
			;
		}
		
		bool CURL::operator < (CURL const &_Right) const
		{
			return
				NContainer::fg_TupleReferences
				(
					mp_Flags
					, mp_Scheme
					, mp_Host
					, mp_Port
					, mp_Username
					, mp_Password
					, mp_Paths
					, mp_Query
					, mp_Fragment
				)
				<
				NContainer::fg_TupleReferences
				(
					_Right.mp_Flags
					, _Right.mp_Scheme
					, _Right.mp_Host
					, _Right.mp_Port
					, _Right.mp_Username
					, _Right.mp_Password
					, _Right.mp_Paths
					, _Right.mp_Query
					, _Right.mp_Fragment
				)
			;
		}	
		

		EURLFlag CURL::f_GetFlags() const
		{
			return mp_Flags;
		}

		bint CURL::f_IsValid() const
		{
			return mp_Flags  &EURLFlag_Valid;
		}

		// Does the URL have a scheme, host  &path?
		bint CURL::f_IsFullURL() const
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

		bint CURL::f_Decode(NStr::CStr const &_URL)
		{
			// The point of all of this is to touch the memory manager as little as possible.
			// When we have a substring class this can be reduced even more (i.e. only when we decode something)

			mp_Flags = EURLFlag_None;

			NStr::CStr URL = _URL;
			mint URLLength = URL.f_GetLen();

			// Find all the various split points for the main components:

			aint iColonSlashSlash = URL.f_Find("://");

			aint iPathStartSlash = URL.f_Find( (iColonSlashSlash >= 0) ? (iColonSlashSlash + 3) : 0, "/");
			if (iPathStartSlash == -1)
				return false;

			aint iQueryMark = URL.f_Find(iPathStartSlash + 1, "?");

			aint iFragmentMark = URL.f_Find( fg_Max(iQueryMark + 1, iPathStartSlash + 1), "#");

			aint iPathEnd = (iQueryMark >= 0) 
								? iQueryMark
								: ( (iFragmentMark >= 0) ? iFragmentMark : URLLength);


			if (iColonSlashSlash >= 0)
			{
				// Extract scheme:
				mp_Scheme = URL.f_Slice(0, iColonSlashSlash);
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
					{ // URL has username  &password
						mp_Username = URL.f_Slice(iUserPassHostStart, iUserColonMark);
						mp_Password = URL.f_Slice(iUserColonMark + 1, iUserPassAtMark);

						mp_Flags |= EURLFlag_Username | EURLFlag_Password;
					}
					else
					{ // URL has username
						mp_Username = URL.f_Slice(iUserPassHostStart, iUserPassAtMark);
						mp_Password.f_Clear();

						mp_Flags |= EURLFlag_Password;
					}

					iHostStart = iUserPassAtMark + 1;
				}

				// Extract Host  &port:
				aint iPortColonMark = URL.f_Find(iHostStart, ":", iPathStartSlash - iHostStart);
				if (iPortColonMark >= 0)
				{ // Has port
					mp_Host = URL.f_Slice( iHostStart, iPortColonMark);

					if (!fg_ParseU16Base10(mp_Port, URL, iPortColonMark + 1, iPathStartSlash))
						return false; // Invalid port

					mp_Flags |= EURLFlag_Host | EURLFlag_Port;
				}
				else
				{ // Does not have port
					mp_Host = URL.f_Slice( iHostStart, iPathStartSlash);

					mp_Flags |= EURLFlag_Host;
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

				if ( fs_PercentDecode(PathSegment, URL, iCurSegmentStart, iCurSegmentEnd) )
				{
					mp_Paths.f_Insert( fg_Move(PathSegment) );
				}
				else
				{
					// Invalid encoding
					return false;
				}
				
				iCurSegmentStart = iCurSegmentEnd + 1;
			}
			
			mp_Flags |= EURLFlag_Path;

			// Extract Query
			if (iQueryMark >= 0)
			{
				mp_Query = URL.f_Slice(iQueryMark + 1, (iFragmentMark >= 0) ? iFragmentMark : URLLength);
				mp_Flags |= EURLFlag_Query;
			}

			// Extract Fragment
			if (iFragmentMark >= 0)
			{
				mp_Fragment = URL.f_Slice(iFragmentMark + 1, URLLength );
				mp_Flags |= EURLFlag_Fragment;
			}

			mp_Flags |= EURLFlag_Valid;
			
			return true;
		}

		NStr::CStr CURL::f_Encode() const
		{
			NStr::CStr Output;
			
			// <scheme>://[<username>[:<password>]@]<server>[:<port>]/<path>[?<query>][#<fragmentID>]
			
			if (mp_Flags  &EURLFlag_Scheme)
			{
				fs_PercentEncode(Output, mp_Scheme);
				Output += "://";
			}
			
			if (mp_Flags  &EURLFlag_Username)
			{
				fs_PercentEncode(Output, mp_Username);
				if (mp_Flags  &EURLFlag_Password)
				{
					Output += ":";
					fs_PercentEncode(Output, mp_Password);
				}				
				Output += "@";
			}
			if (mp_Flags  &EURLFlag_Host)
			{
				fs_PercentEncode(Output, mp_Host);
				if (mp_Flags  &EURLFlag_Port)
					Output += NStr::CStr::CFormat(":{}") << mp_Port;
			}
			
			if ((mp_Flags  &EURLFlag_Path)  &&!mp_Paths.f_IsEmpty())
			{
				for (auto &Path : mp_Paths)
				{
					Output += "/";
					fs_PercentEncode(Output, Path);
				}
			}
			else
				Output += "/";
			
			if (mp_Flags  &EURLFlag_Query)
			{
				Output += "?";
				fs_PercentEncode(Output, mp_Query);
			}
			if (mp_Flags  &EURLFlag_Fragment)
			{
				Output += "#";
				fs_PercentEncode(Output, mp_Fragment);
			}

			return Output;
		}

		// Test if the URL has a field:

		bint CURL::f_HasAll(EURLFlag _Flags) const
		{
			return (mp_Flags  &_Flags) == _Flags;
		}

		bint CURL::f_HasAny(EURLFlag _Flags) const
		{
			return (mp_Flags  &_Flags);
		}

		bint CURL::f_HasScheme() const
		{
			return mp_Flags  &EURLFlag_Scheme;
		}

		bint CURL::f_HasHost() const
		{
			return mp_Flags  &EURLFlag_Host;
		}

		bint CURL::f_HasPort() const
		{
			return mp_Flags  &EURLFlag_Port;
		}

		bint CURL::f_HasUsername() const
		{
			return mp_Flags  &EURLFlag_Username;
		}

		bint CURL::f_HasPassword() const
		{
			return mp_Flags  &EURLFlag_Password;
		}

		bint CURL::f_HasPath() const
		{
			return mp_Flags  &EURLFlag_Path;
		}

		bint CURL::f_HasQuery() const
		{
			return mp_Flags  &EURLFlag_Query;
		}

		bint CURL::f_HasFragment() const
		{
			return mp_Flags  &EURLFlag_Fragment;
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
		
		NStr::CStr CURL::f_GetFullPath() const
		{
			NStr::CStr FullPath;
			for (auto &Path : mp_Paths)
				fg_AddStrSep(FullPath, Path, "/");
			
			return "/" + FullPath;
		}


		NStr::CStr const &CURL::f_GetQuery() const
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

		void CURL::f_SetHost(NStr::CStr const &_Host)
		{
			mp_Host = _Host;
			mp_Flags |= EURLFlag_Host;
		}

		void CURL::f_SetPort(uint16 _Port)
		{
			mp_Port = _Port;
			mp_Flags |= EURLFlag_Port;
		}

		void CURL::f_SetUsername(NStr::CStr const &_Username)
		{
			mp_Username = _Username;
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

		void CURL::f_SetPath(NContainer::TCVector<NStr::CStr>  &&_Path)
		{
			mp_Paths = fg_Move(_Path);
			mp_Flags |= EURLFlag_Path;
		}

		void CURL::f_SetQuery(NStr::CStr const &_Query)
		{
			mp_Query = _Query;
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

		static bint fg_DecodeHexChar(char &_oNum, char _Ch)
		{
			if (_Ch >= '0'  &&_Ch <= '9')
				_oNum = _Ch - '0';
			else if (_Ch >= 'a'  &&_Ch <= 'f')
				_oNum = (_Ch - 'a') + 10;
			else if (_Ch >= 'A'  &&_Ch <= 'F')
				_oNum = (_Ch - 'A') + 10;
			else
				return false;

			return true;
		}

		// _Start can range from 0 -> Len-1
		// _End can range from 0 -> Len or be -1 which == Len			
		bint fg_ParseU16Base10(uint16 &o_Num, NStr::CStr const &_Str, aint _Start, aint _End)
		{
			if (_End == -1)
				_End = _Str.f_GetLen();

			if (_Start < 0 || _End < 0 || _Start >= _End)
				return false;

			char const* pStart = _Str.f_GetStr() + _Start;
			char const* pEnd = _Str.f_GetStr() + _End;

			char const* pPtr = pEnd - 1;

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
					, f_HasScheme() ? mp_Scheme : "<None>"
					, f_HasUsername() ? mp_Username : "<None>"
					, f_HasPassword() ? mp_Password : "<None>"
					, f_HasHost() ? mp_Host : "<None>"
					, f_HasPort() ? NStr::fg_Format("{}", mp_Port) : "<None>"
					, f_HasPath() ? Path : "<None>"
					, f_HasQuery() ? mp_Query : "<None>"
					, f_HasFragment() ? mp_Fragment : "<None>"
				)
			;
			NSys::fg_DebugOutput(Text.f_GetStr());
		}

		//
		// CURL Public Static Methods
		//

		// _Start can range from 0 -> Len-1
		// _End can range from 0 -> Len or be -1 which == Len			
		bint CURL::fs_PercentDecode(NStr::CStr &_oResult, NStr::CStr const &_Str, aint _Start, aint _End)
		{
			if (_End == -1)
				_End = _Str.f_GetLen();

			if (_Start < 0 || _End < 0 || _Start >= _End)
				return false;

			char const* pStart = _Str.f_GetStr() + _Start;
			char const* pPtr = pStart;
			char const* pEnd = _Str.f_GetStr() + _End;

			char const* pRunStart = pPtr;

			NStr::CStr Result;
			char Ch0, Ch1;

			while(pPtr != pEnd)
			{
				if (*pPtr == '%')
				{
					// End current run
					if (pRunStart > pPtr)
						Result += NStr::CStr(pRunStart, pPtr - pRunStart);

					if ( (pPtr + 2) >= pEnd)
					{
						// Error: No room for encoded char
						return false;
					}

					if (	!fg_DecodeHexChar(Ch0, pPtr[1])
						|| 	!fg_DecodeHexChar(Ch1, pPtr[2]) )
						return false; // Error: Invalid encoding chars.

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
				if (	pRunStart == _Str.f_GetStr()
					 &&	pPtr == (_Str.f_GetStr() + _Str.f_GetLen()) )
					Result = _Str;
				else
				{
					Result += NStr::CStr(pRunStart, pPtr - pRunStart);					
				}
			}

			_oResult = fg_Move(Result);
			return true;
		}
		
		void CURL::fs_PercentEncode(NStr::CStr &o_Result, NStr::CStr const &_Str)
		{
			NStr::CStr Str = _Str;
			
			auto pParse = Str.f_GetStr();
			
			ch8 const *pReserved = ":/?#[]@!$&'()*+,;=";
			
			while (*pParse)
			{
				uch8 Character = (uch8)*pParse;
				if (Character <= 32 || Character >= 128 || NStr::fg_StrFindChar(pReserved, Character) >= 0)
				{
					o_Result += NStr::CStr::CFormat("%{nfh,sf0,sj2}") << Character;
				}
				else
					o_Result.f_AddChar(*pParse);
				++pParse;
			}
		}
		

	} // Namespace NHTTP

} // Namespace NMib
