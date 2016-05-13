// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib
{
	namespace NHTTP
	{
		template <typename tf_CStream>
		void CURL::f_Feed(tf_CStream &_Stream) const
		{
			_Stream << EVersion;
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
				_Stream << mp_Query;
			if (mp_Flags & EURLFlag_Fragment)
				_Stream << mp_Fragment;
		}
		
		template <typename tf_CStream>
		void CURL::f_Consume(tf_CStream &_Stream)
		{
			uint32 Version;
			_Stream >> Version;
			if (Version >= EVersion)
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
				_Stream >> mp_Query;
			else
				mp_Query.f_Clear();
			
			if (mp_Flags & EURLFlag_Fragment)
				_Stream >> mp_Fragment;
			else
				mp_Fragment.f_Clear();
		}
	}
}
