// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NWeb::NHTTP
{
	template <typename tf_CStream>
	void CURL::CQueryEntry::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Key;
		_Stream % m_Value;
	}

	template <typename tf_CStream>
	void CURL::f_Feed(tf_CStream &_Stream, uint32 _Version) const
	{
		_Stream << _Version;
		_Stream << mp_Flags;

		if (mp_Flags & EURLFlag_Scheme)
			_Stream << mp_Scheme;
		if (mp_Flags & EURLFlag_Host)
			_Stream << mp_Host;
		if (mp_Flags & EURLFlag_Port)
			_Stream << mp_Port;
		if (mp_Flags & EURLFlag_Username)
			_Stream << mp_Username;
		if (mp_Flags & EURLFlag_Password)
			_Stream << mp_Password;
		if (mp_Flags & EURLFlag_Path)
			_Stream << mp_Paths;
		if (mp_Flags & EURLFlag_Query)
		{
			if (_Version == 0x101)
			{
				NStr::CStr Query;
				if (!mp_Query.f_IsEmpty())
					Query = fg_Format("{}={}", mp_Query.f_GetFirst().m_Key, mp_Query.f_GetFirst().m_Value);
				_Stream << Query;
			}
			else
				_Stream << mp_Query;
		}
		if (mp_Flags & EURLFlag_Fragment)
			_Stream << mp_Fragment;
	}

	template <typename tf_CStream>
	void CURL::f_Consume(tf_CStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EVersion)
			DMibError("Invalid URL version");
		_Stream >> mp_Flags;

		if (mp_Flags & EURLFlag_Scheme)
			_Stream >> mp_Scheme;
		else
			mp_Scheme.f_Clear();

		if (mp_Flags & EURLFlag_Host)
			_Stream >> mp_Host;
		else
			mp_Host.f_Clear();

		if (mp_Flags & EURLFlag_Port)
			_Stream >> mp_Port;
		else
			mp_Port = 0;

		if (mp_Flags & EURLFlag_Username)
			_Stream >> mp_Username;
		else
			mp_Username.f_Clear();

		if (mp_Flags & EURLFlag_Password)
			_Stream >> mp_Password;
		else
			mp_Password.f_Clear();

		if (mp_Flags & EURLFlag_Path)
			_Stream >> mp_Paths;
		else
			mp_Paths.f_Clear();

		if (mp_Flags & EURLFlag_Query)
		{
			if (Version == 0x101)
			{
				NStr::CStr Query;
				_Stream >> Query;
				mp_Query.f_Clear();
				NStr::CStr Key = fg_GetStrSep(Query, "=");
				mp_Query.f_Insert({Key, Query});
			}
			else
				_Stream >> mp_Query;
		}
		else
			mp_Query.f_Clear();

		if (mp_Flags & EURLFlag_Fragment)
			_Stream >> mp_Fragment;
		else
			mp_Fragment.f_Clear();
	}
}
