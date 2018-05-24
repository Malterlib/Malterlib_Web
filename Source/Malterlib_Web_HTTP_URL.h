// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once
#include <Mib/Core/Core>

namespace NMib
{
	namespace NHTTP
	{
		enum EURLFlag
		{
			EURLFlag_None = 0
			, EURLFlag_Valid = DMibBit(0)
			, EURLFlag_Scheme = DMibBit(1)
			, EURLFlag_Host = DMibBit(2)
			, EURLFlag_Port = DMibBit(3)
			, EURLFlag_Username = DMibBit(4)
			, EURLFlag_Password = DMibBit(5)
			, EURLFlag_Path = DMibBit(6)
			, EURLFlag_Query = DMibBit(7)
			, EURLFlag_Fragment = DMibBit(8)
			, EURLFlag_HostBrackets = DMibBit(9)
		};

		/*
		URL Utility Class

		URLs are of the form:
			<scheme>://[<username>[:<password>]@]<server>[:<port>]/<path>[?<query>][#<fragmentID>]

		Where:
			<path> is a string of possibly percent encoded path segments separated by forward slashes.
		*/
		class CURL
		{
		public:
			struct CQueryEntry
			{
				NStr::CStr m_Key;
				NStr::CStr m_Value;
				
				void f_Format(NStr::CStrAggregate &o_Str) const;
				bool operator < (CQueryEntry const &_Right) const;
				bool operator == (CQueryEntry const &_Right) const;
				
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
			};
			
		private:
			EURLFlag mp_Flags = EURLFlag_None;

			// Fields are only valid if the corresponding flag in mp_Flags is set:
			NStr::CStr mp_Scheme;
			NStr::CStr mp_Host;
			uint16 mp_Port = 0;

			NStr::CStr mp_Username;
			NStr::CStr mp_Password;

			NContainer::TCVector<NStr::CStr> mp_Paths;

			NContainer::TCVector<CQueryEntry> mp_Query;
			NStr::CStr mp_Fragment;

			void fp_Parse(NStr::CStr const &_Str);			

		public:
			enum 
			{
				EVersion = 0x102
			};
			
			CURL();
			CURL(NStr::CStr const &_URL);
			CURL(CURL const &_ToCopy);
			CURL(CURL &&_ToMove);
			~CURL();

			CURL &operator =(NStr::CStr const &_URL);
			CURL &operator =(CURL const &_ToCopy);
			CURL &operator =(CURL &&_ToMove);

			bool operator == (CURL const &_Right) const;
			bool operator < (CURL const &_Right) const;

			EURLFlag f_GetFlags() const;
			
			bint f_IsValid() const;

			// Does the URL have a scheme, host  &path?
			bint f_IsFullURL() const;

			void f_Clear();

			bint f_Decode(NStr::CStr const &_URL);
			NStr::CStr f_Encode() const;

			// Test if the URL has a field:

			bint f_HasAll(EURLFlag _Flags) const;
			bint f_HasAny(EURLFlag _Flags) const;

			bint f_HasScheme() const;
			bint f_HasHost() const;
			bint f_HasPort() const;
			bint f_HasUsername() const;
			bint f_HasPassword() const;
			bint f_HasPath() const;
			bint f_HasQuery() const;
			bint f_HasFragment() const;

			// Access an URL field:

			NStr::CStr const &f_GetScheme() const;
			NStr::CStr const &f_GetHost() const;
			uint16 f_GetPort() const;
			uint16 f_GetPortFromScheme() const;
			NStr::CStr const &f_GetUsername() const;
			NStr::CStr const &f_GetPassword() const;
			NContainer::TCVector<NStr::CStr> const &f_GetPath() const;
			NStr::CStr f_GetFullPath() const;
			NContainer::TCVector<CQueryEntry> const &f_GetQuery() const;
			NStr::CStr const &f_GetFragment() const;

			// Set an URL field:

			void f_SetScheme(NStr::CStr const &_Scheme);
			void f_SetHost(NStr::CStr const &_Host);
			void f_SetPort(uint16);
			void f_SetUsername(NStr::CStr const &_Username);
			void f_SetPassword(NStr::CStr const &_Password);
			void f_SetPath(NContainer::TCVector<NStr::CStr> const &_Path);
			void f_SetPath(NContainer::TCVector<NStr::CStr>  &&_Path);
			void f_SetQuery(NContainer::TCVector<CQueryEntry> const &_Query);
			void f_AddQueryEntry(CQueryEntry const &_QueryEntry);
			void f_SetFragment(NStr::CStr const &_Fragment);

			// Clear a URL field:

			void f_ClearScheme();
			void f_ClearHost();
			void f_ClearPort();
			void f_ClearUsername();
			void f_ClearPassword();
			void f_ClearPath();
			void f_ClearQuery();
			void f_ClearFragment();

			// Utility:

			void f_DebugOut() const;

			// _Start can range from 0 -> Len-1
			// _End can range from 0 -> Len or be -1 which == Len			
			static bint fs_PercentDecode(NStr::CStr &_oResult, NStr::CStr const &_Str, aint _Start = 0, aint _End = -1);

			static void fs_PercentEncode(NStr::CStr &o_Result, NStr::CStr const &_Str, ch8 const *_pReserved = nullptr, bool _bUpperCase = false);

			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream, uint32 _Version = EVersion) const;
			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream);
			
			void f_Format(NStr::CStrAggregate &o_Str) const;
		};

	} // Namespace NHTTP

} // Namespace NMib

#ifndef DMibPNoShortCuts
using namespace NMib::NHTTP;
#endif

#include "Malterlib_Web_HTTP_URL.hpp"
