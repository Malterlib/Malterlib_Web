// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <Mib/Core/Core>

namespace NMib::NWeb::NHTTP
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
		, EURLFlag_HostRaw = DMibBit(10)
	};

	enum EEncodeFlag
	{
		EEncodeFlag_None = 0
		, EEncodeFlag_UpperCasePercentEncode = DMibBit(0)
		, EEncodeFlag_DoublePercentEncode = DMibBit(1)
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

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;
			auto operator <=> (CQueryEntry const &_Right) const noexcept = default;

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

		auto operator <=> (CURL const &_Right) const noexcept = default;

		EURLFlag f_GetFlags() const;

		bool f_IsValid() const;

		// Does the URL have a scheme, host  &path?
		bool f_IsFullURL() const;

		void f_Clear();

		bool f_Decode(NStr::CStr const &_URL, EURLFlag _Flags = EURLFlag_None);
		NStr::CStr f_Encode(EEncodeFlag _Flags = EEncodeFlag_None) const;

		// Test if the URL has a field:

		bool f_HasAll(EURLFlag _Flags) const;
		bool f_HasAny(EURLFlag _Flags) const;

		bool f_HasScheme() const;
		bool f_HasHost() const;
		bool f_HasPort() const;
		bool f_HasUsername() const;
		bool f_HasPassword() const;
		bool f_HasPath() const;
		bool f_HasQuery() const;
		bool f_HasFragment() const;

		// Access an URL field:

		NStr::CStr const &f_GetScheme() const;
		NStr::CStr const &f_GetHost() const;
		uint16 f_GetPort() const;
		uint16 f_GetPortFromScheme() const;
		NStr::CStr const &f_GetUsername() const;
		NStr::CStr const &f_GetPassword() const;
		NContainer::TCVector<NStr::CStr> const &f_GetPath() const;
		NStr::CStr f_GetFullPath() const;
		NStr::CStr f_GetFullPathPercentEncoded(EEncodeFlag _Flags = EEncodeFlag_None) const;
		NContainer::TCVector<CQueryEntry> const &f_GetQuery() const;
		NStr::CStr const &f_GetFragment() const;

		// Set an URL field:

		void f_SetScheme(NStr::CStr const &_Scheme);
		void f_SetHost(NStr::CStr const &_Host, bool _bRaw = false);
		void f_SetPort(uint16);
		void f_SetUsername(NStr::CStr const &_Username);
		void f_SetPassword(NStr::CStr const &_Password);
		void f_SetPath(NContainer::TCVector<NStr::CStr> const &_Path);
		void f_SetPath(NContainer::TCVector<NStr::CStr>  &&_Path);
		void f_AppendPath(NContainer::TCVector<NStr::CStr> const &_Path);
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
		static bool fs_PercentDecode(NStr::CStr &_oResult, NStr::CStr const &_Str, aint _Start = 0, aint _End = -1);

		static void fs_PercentEncode(NStr::CStr &o_Result, NStr::CStr const &_Str, ch8 const *_pReserved = nullptr, EEncodeFlag _Flags = EEncodeFlag_None);
		static NStr::CStr fs_PercentEncode(NStr::CStr const &_Str, ch8 const *_pReserved = nullptr, EEncodeFlag _Flags = EEncodeFlag_None);
		static NStr::CStr fs_GetQueryPercentEncoded(NContainer::TCVector<CQueryEntry> const &_QueryEntries, EEncodeFlag _Flags = EEncodeFlag_None);

		template <typename tf_CStream>
		void f_Feed(tf_CStream &_Stream, uint32 _Version = EVersion) const;
		template <typename tf_CStream>
		void f_Consume(tf_CStream &_Stream);

		template <typename tf_CStr>
		void f_Format(tf_CStr &o_Str) const;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb::NHTTP;
#endif

#include "Malterlib_Web_HTTP_URL.hpp"
