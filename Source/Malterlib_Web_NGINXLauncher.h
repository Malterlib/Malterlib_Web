// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Process/ProcessLaunch>

namespace NMib
{
	namespace NWeb
	{
		class CNGINXLauncher
		{
		public:
			CNGINXLauncher(NStr::CStr const& _LaunchPath, NStr::CStr const& _RootDir);
			~CNGINXLauncher();
			
			void f_SetFastCGIListen(uint16 _StartPort, uint16 _nListen);
			void f_SetListen(uint16 _Poth);
			void f_SetStaticRoot(NStr::CStr const& _Root);
			void f_Launch();
			
		private:
			NStr::CStr mp_LaunchPath;
			NStr::CStr mp_RootDir;
			NStr::CStr mp_StaticRoot;
			uint16 mp_FastCGIStartListen;
			uint16 mp_nFastCGIListen;
			uint16 mp_Listen;
			NPtr::TCUniquePointer<NProcess::CProcessLaunch> mp_pProcessLaunch;
		};
	}
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
