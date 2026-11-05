// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Web_AWS_Lambda.h"
#include "Malterlib_Web_AWS_Internal.h"

#include <Mib/XML/XML>
#include <Mib/Cryptography/RandomID>
#include <Mib/Stream/Binary>
#include <Mib/Container/LinkedList>

#include "zlib.h"
#include "contrib/minizip/ioapi.h"
#include "contrib/minizip/zip.h"

namespace NMib::NWeb
{
	namespace
	{
		struct CMiniZipAdaptor
		{
			CMiniZipAdaptor(TCMap<CStr, CStr> const &_Files);

			void f_SetFunctions(zlib_filefunc_def *_pFileFunc);
			void f_SetFunctions(zlib_filefunc64_def *_pFileFunc);
			CByteVector f_GetOutFile(CStr const &_Path);

		private:
			static voidpf ZCALLBACK fsp_FOpenFunc(voidpf _pOpaque, char const *_pFileName, int _Mode);
			static voidpf ZCALLBACK fsp_FOpen64Func(voidpf _pOpaque, void const *_pFileName, int _Mode);
			static uLong ZCALLBACK fsp_FReadFunc(voidpf _pOpaque, voidpf _pStream, void *_pBuffer, uLong _Size);
			static uLong ZCALLBACK fsp_FWriteFunc(voidpf _pOpaque, voidpf _pStream, void const *_pBuffer, uLong _Size);
			static long ZCALLBACK fsp_FTellFunc(voidpf _pOpaque, voidpf _pStream);
			static ZPOS64_T ZCALLBACK fsp_FTell64Func(voidpf _pOpaque, voidpf _pStream);
			static long ZCALLBACK fsp_FSeekFunc(voidpf _pOpaque, voidpf _pStream, uLong _Offset, int _Origin);
			static long ZCALLBACK fsp_FSeek64Func(voidpf _pOpaque, voidpf _pStream, ZPOS64_T _Offset, int _Origin);
			static int ZCALLBACK fsp_FCloseFunc(voidpf _pOpaque, voidpf _pStream);
			static int ZCALLBACK fsp_FErrorFunc(voidpf _pOpaque, voidpf _pStream);

			TCMap<CStr, CStr> const &mp_Files;
			TCLinkedList<NStream::TCBinaryStreamMemoryPtr<>> mp_InFiles;
			TCMap<CStr, NStream::TCBinaryStreamMemory<>> mp_OutFiles;
		};

		CMiniZipAdaptor::CMiniZipAdaptor(TCMap<CStr, CStr> const &_Files)
			: mp_Files(_Files)
		{
		}

		void CMiniZipAdaptor::f_SetFunctions(zlib_filefunc_def *_pFileFunc)
		{
			_pFileFunc->zopen_file = fsp_FOpenFunc;
			_pFileFunc->zread_file = fsp_FReadFunc;
			_pFileFunc->zwrite_file = fsp_FWriteFunc;
			_pFileFunc->ztell_file = fsp_FTellFunc;
			_pFileFunc->zseek_file = fsp_FSeekFunc;
			_pFileFunc->zclose_file = fsp_FCloseFunc;
			_pFileFunc->zerror_file = fsp_FErrorFunc;
			_pFileFunc->opaque = this;
		}

		void CMiniZipAdaptor::f_SetFunctions(zlib_filefunc64_def *_pFileFunc)
		{
			_pFileFunc->zopen64_file = fsp_FOpen64Func;
			_pFileFunc->zread_file = fsp_FReadFunc;
			_pFileFunc->zwrite_file = fsp_FWriteFunc;
			_pFileFunc->ztell64_file = fsp_FTell64Func;
			_pFileFunc->zseek64_file = fsp_FSeek64Func;
			_pFileFunc->zclose_file = fsp_FCloseFunc;
			_pFileFunc->zerror_file = fsp_FErrorFunc;
			_pFileFunc->opaque = this;
		}

		CByteVector CMiniZipAdaptor::f_GetOutFile(CStr const &_Path)
		{
			auto pFile = mp_OutFiles.f_FindEqual(_Path);
			if (!pFile)
				return {};
			return pFile->f_MoveVector();
		}

		voidpf ZCALLBACK CMiniZipAdaptor::fsp_FOpenFunc(voidpf _pOpaque, char const *_pFileName, int _Mode)
		{
			CMiniZipAdaptor *pThis = (CMiniZipAdaptor *)_pOpaque;

			NStream::CBinaryStreamDefault *pFile;

			CStr FileName(_pFileName);

			if (_Mode & ZLIB_FILEFUNC_MODE_EXISTING)
			{
				if (!(_Mode & ZLIB_FILEFUNC_MODE_READ))
					return nullptr;

				auto *pSourceFile = pThis->mp_Files.f_FindEqual(FileName);
				if (!pSourceFile)
					return nullptr;

				NStream::TCBinaryStreamMemoryPtr<> *pNewFile = &pThis->mp_InFiles.f_Insert();

				pNewFile->f_OpenRead(pSourceFile->f_GetStr(), pSourceFile->f_GetLen());
				pFile = pNewFile;
			}
			else if (_Mode & ZLIB_FILEFUNC_MODE_CREATE)
			{
				if (!(_Mode & ZLIB_FILEFUNC_MODE_WRITE))
					return nullptr;
				pFile = &pThis->mp_OutFiles[FileName];
			}
			else
				return nullptr;

			return pFile;
		}

		voidpf ZCALLBACK CMiniZipAdaptor::fsp_FOpen64Func(voidpf _pOpaque, void const *_pFileName, int _Mode)
		{
			return fsp_FOpenFunc(_pOpaque, (ch8 const *)_pFileName, _Mode);
		}

		uLong ZCALLBACK CMiniZipAdaptor::fsp_FReadFunc(voidpf _pOpaque, voidpf _pStream, void *_pBuffer, uLong _Size)
		{
			NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)_pStream;

			pFile->f_ConsumeBytes(_pBuffer, _Size);
			return _Size;
		}

		uLong ZCALLBACK CMiniZipAdaptor::fsp_FWriteFunc(voidpf _pOpaque, voidpf _pStream, void const *_pBuffer, uLong _Size)
		{
			NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)_pStream;

			pFile->f_FeedBytes(_pBuffer, _Size);
			return _Size;
		}

		long ZCALLBACK CMiniZipAdaptor::fsp_FTellFunc(voidpf _pOpaque, voidpf _pStream)
		{
			NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)_pStream;

			return pFile->f_GetPosition();
		}

		ZPOS64_T ZCALLBACK CMiniZipAdaptor::fsp_FTell64Func(voidpf _pOpaque, voidpf _pStream)
		{
			NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)_pStream;

			return pFile->f_GetPosition();
		}

		long ZCALLBACK CMiniZipAdaptor::fsp_FSeekFunc(voidpf _pOpaque, voidpf _pStream, uLong _Offset, int _Origin)
		{
			NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)_pStream;

			switch (_Origin)
			{
			case ZLIB_FILEFUNC_SEEK_CUR: pFile->f_AddPosition(_Offset); break;
			case ZLIB_FILEFUNC_SEEK_END: pFile->f_SetPositionFromEnd(_Offset); break;
			case ZLIB_FILEFUNC_SEEK_SET: pFile->f_SetPosition(_Offset); break;
			default: return -1;
			}

			return 0;
		}

		long ZCALLBACK CMiniZipAdaptor::fsp_FSeek64Func(voidpf _pOpaque, voidpf _pStream, ZPOS64_T _Offset, int _Origin)
		{
			NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)_pStream;

			switch (_Origin)
			{
			case ZLIB_FILEFUNC_SEEK_CUR: pFile->f_AddPosition(_Offset); break;
			case ZLIB_FILEFUNC_SEEK_END: pFile->f_SetPositionFromEnd(_Offset); break;
			case ZLIB_FILEFUNC_SEEK_SET: pFile->f_SetPosition(_Offset); break;
			default: return -1;
			}

			return 0;
		}

		int ZCALLBACK CMiniZipAdaptor::fsp_FCloseFunc(voidpf _pOpaque, voidpf _pStream)
		{
			return 0;
		}

		int ZCALLBACK CMiniZipAdaptor::fsp_FErrorFunc(voidpf _pOpaque, voidpf _pStream)
		{
			/* We never return errors */
			return 0;
		}
	}

	struct CAwsLambdaActor::CInternal : public NConcurrency::CActorInternal
	{
		struct CCodeBlob
		{
			CStr m_Base64;
			NCryptography::CHashDigest_SHA256 m_SHA256;
		};

		CInternal(TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials);

		static void fs_CheckZipError(int _Error, CStr const &_File);

		TCFuture<CCodeBlob> f_CreateCodeBlob(TCMap<CStr, CStr> const &_Files);
		TCFuture<CJsonSorted> f_GetFunction(CStr const &_FunctionName);

		static CFunctionConfiguration fs_FunctionConfigFromJson(CJsonSorted const &_Json);
		TCFuture<CFunctionInfo> f_CreateFunction(CStr _FunctionName, CCodeBlob _CodeBlob, CFunctionConfiguration _Config);
		TCFuture<void> f_UpdateFunctionConfiguration(CStr _FunctionName, CFunctionConfiguration _Config);
		TCFuture<CFunctionInfo> f_UpdateFunctionCode(CStr _FunctionName, CCodeBlob _CodeBlob, CFunctionConfiguration _Config);
		TCFuture<CJsonSorted> f_GetFunctionVersions(CStr const &_FunctionName);
		TCFuture<CFunctionInfo> f_PublishVersion(CStr _FunctionName);
		TCFuture<void> f_WaitForFunctionActive(CStr _FunctionName);

		CAwsCredentials m_Credentials;
		TCActor<CHttpClientActor> m_HttpClientActor;

	private:
		static CStr fsp_TracingConfigToString(CFunctionConfiguration::ETracingMode _Mode);
		static CFunctionConfiguration::ETracingMode fsp_TracingConfigFromString(CStr const &_String);
		void fp_PopulateFunctionConfigurationRequest(CJsonSorted &o_Request, CFunctionConfiguration const &_Config);
	};

	CAwsLambdaActor::CInternal::CInternal(TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials)
		: m_HttpClientActor{_HttpClientActor}
		, m_Credentials{_Credentials}
	{
	}

	void CAwsLambdaActor::CInternal::fs_CheckZipError(int _Error, CStr const &_File)
	{
		switch (_Error)
		{
		case ZIP_OK: return;
		case ZIP_ERRNO: DMibError(fg_Format("Zip: Errno, {}", _File));
		case ZIP_PARAMERROR: DMibError(fg_Format("Zip: Param error: {}", _File));
		case ZIP_BADZIPFILE: DMibError(fg_Format("Zip: bad zip file: {}", _File));
		case ZIP_INTERNALERROR: DMibError(fg_Format("Zip: internal error: {}", _File));
		}

		DMibError(fg_Format("Unzip: {}: {}", _Error, _File));
	}

	TCFuture<CAwsLambdaActor::CInternal::CCodeBlob> CAwsLambdaActor::CInternal::f_CreateCodeBlob(TCMap<CStr, CStr> const &_Files)
	{
		return g_ConcurrentDispatch / [=]() -> CCodeBlob
			{
				CMiniZipAdaptor Adaptor({});

				// We keep the files at the same year to keep the zip SHA256 the same
				auto Time = NTime::CTimeConvert::fs_CreateTime(NTime::CTimeConvert(NTime::CTime::fs_NowUTC()).f_GetYear());

				NTime::CTimeConvert::CDateTime DateTime;
				NTime::CTimeConvert(Time).f_ExtractDateTime(DateTime);

				{
					zlib_filefunc64_def Functions;
					Adaptor.f_SetFunctions(&Functions);

					zipFile pZipFile = zipOpen2_64("OutFile.zip", false, nullptr, &Functions);
					if (!pZipFile)
						DMibError("Failed to create zip file");
					auto Cleanup = g_OnScopeExit / [&]()
						{
							zipClose(pZipFile, "AWS code blob");
						}
					;

					for (auto &File : _Files)
					{
						auto &FileName = _Files.fs_GetKey(File);

						zip_fileinfo Info;
						Info.dosDate = 0;
						Info.external_fa = 0;
						Info.internal_fa = 0;
						Info.tmz_date.tm_year = DateTime.m_Year;
						Info.tmz_date.tm_mon = DateTime.m_Month;
						Info.tmz_date.tm_mday = DateTime.m_DayOfMonth;
						Info.tmz_date.tm_hour = DateTime.m_Hour;
						Info.tmz_date.tm_min = DateTime.m_Minute;
						Info.tmz_date.tm_sec = DateTime.m_Second;
						if (zipOpenNewFileInZip64(pZipFile, FileName.f_GetStr(), &Info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_BEST_SPEED, 0) != ZIP_OK)
							DMibError("zipOpenNewFileInZip64 failed for: {}"_f << FileName);

						zipWriteInFileInZip(pZipFile, File.f_GetStr(), File.f_GetLen());
						zipCloseFileInZip(pZipFile);
					}
				}

				auto OutFileData = Adaptor.f_GetOutFile("OutFile.zip");

				return {NEncoding::fg_Base64Encode(OutFileData), NCryptography::CHash_SHA256::fs_DigestFromData(OutFileData)};
			}
		;
	}

	TCFuture<CJsonSorted> CAwsLambdaActor::CInternal::f_GetFunction(CStr const &_FunctionName)
	{
		NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions"_f << m_Credentials.m_Region};
		AWSUrl.f_AppendPath({_FunctionName});
		return fg_DoAWSRequestJson("Get function", m_HttpClientActor, 200, AWSUrl, {}, CHttpClientActor::EMethod_GET, m_Credentials, {}, "lambda");
	}

	CStr CAwsLambdaActor::CInternal::fsp_TracingConfigToString(CFunctionConfiguration::ETracingMode _Mode)
	{
		switch (_Mode)
		{
		case CFunctionConfiguration::ETracingMode_Active: return "Active";
		case CFunctionConfiguration::ETracingMode_PassThrough: return "PassThrough";
		case CFunctionConfiguration::ETracingMode_Unknown: return "";
		}
		return "";
	}

	CAwsLambdaActor::CFunctionConfiguration::ETracingMode CAwsLambdaActor::CInternal::fsp_TracingConfigFromString(CStr const &_String)
	{
		if (_String == "Active")
			return CFunctionConfiguration::ETracingMode_Active;
		else if (_String == "PassThrough")
			return CFunctionConfiguration::ETracingMode_PassThrough;
		return CFunctionConfiguration::ETracingMode_Unknown;
	}

	void CAwsLambdaActor::CInternal::fp_PopulateFunctionConfigurationRequest(CJsonSorted &o_Request, CFunctionConfiguration const &_Config)
	{
		if (_Config.m_DeadLetterConfig.m_TargetArn)
			o_Request["DeadLetterConfig"]["TargetArn"] = *_Config.m_DeadLetterConfig.m_TargetArn;

		if (_Config.m_TracingConfig.m_Mode)
			o_Request["TracingConfig"]["Mode"] = fsp_TracingConfigToString(*_Config.m_TracingConfig.m_Mode);

		if (_Config.m_VpcConfig.m_SecurityGroupIds)
			o_Request["VpcConfig"]["SecurityGroupIds"] = *_Config.m_VpcConfig.m_SecurityGroupIds;

		if (_Config.m_VpcConfig.m_SubnetIDs)
			o_Request["VpcConfig"]["SubnetIDs"] = *_Config.m_VpcConfig.m_SubnetIDs;

		if (_Config.m_Description)
			o_Request["Description"] = *_Config.m_Description;

		if (_Config.m_EnvironmentVariables)
		{
			auto &Vars = o_Request["Environment"]["Variables"];
			for (auto &Value : *_Config.m_EnvironmentVariables)
				Vars[TCMap<CStr, CStr>::fs_GetKey(Value)] = Value;
		}

		if (_Config.m_Handler)
			o_Request["Handler"] = *_Config.m_Handler;

		if (_Config.m_KMSKeyArn)
			o_Request["KMSKeyArn"] = *_Config.m_KMSKeyArn;

		if (_Config.m_MemorySizeMB)
			o_Request["MemorySize"] = *_Config.m_MemorySizeMB;

		if (_Config.m_Role)
			o_Request["Role"] = *_Config.m_Role;

		if (_Config.m_Runtime)
			o_Request["Runtime"] = *_Config.m_Runtime;

		if (_Config.m_TimeoutSeconds)
			o_Request["Timeout"] = *_Config.m_TimeoutSeconds;
	}

	CAwsLambdaActor::CFunctionConfiguration CAwsLambdaActor::CInternal::fs_FunctionConfigFromJson(CJsonSorted const &_Json)
	{
		auto &JsonConfig = _Json["Configuration"];

		CFunctionConfiguration Config;

		if (auto pValue = JsonConfig.f_GetMember("DeadLetterConfig", EJsonType_Object))
		{
			if (auto pTargetArn = pValue->f_GetMember("TargetArn", EJsonType_String))
				Config.m_DeadLetterConfig.m_TargetArn = pTargetArn->f_String();
		}

		if (auto pValue = JsonConfig.f_GetMember("TracingConfig", EJsonType_Object))
		{
			if (auto pTargetArn = pValue->f_GetMember("Mode", EJsonType_String))
				Config.m_TracingConfig.m_Mode = fsp_TracingConfigFromString(pTargetArn->f_String());
		}

		if (auto pValue = JsonConfig.f_GetMember("VpcConfig", EJsonType_Object))
		{
			if (auto pSecurityGroupIds = pValue->f_GetMember("SecurityGroupIds", EJsonType_Array))
			{
				auto &OutSecurityGroupIds = *(Config.m_VpcConfig.m_SecurityGroupIds = NContainer::TCVector<NStr::CStr>{});
				for (auto &ID : pSecurityGroupIds->f_Array())
					OutSecurityGroupIds.f_Insert(ID.f_String());
			}

			if (auto pSubnetIDs = pValue->f_GetMember("SubnetIDs", EJsonType_Array))
			{
				auto &OutSubnetIDs = *(Config.m_VpcConfig.m_SubnetIDs = NContainer::TCVector<NStr::CStr>{});
				for (auto &ID : pSubnetIDs->f_Array())
					OutSubnetIDs.f_Insert(ID.f_String());
			}
		}

		if (auto pValue = JsonConfig.f_GetMember("Description", EJsonType_String))
			Config.m_Description = pValue->f_String();

		if (auto pEnvironment = JsonConfig.f_GetMember("Environment", EJsonType_Object))
		{
			if (auto pVariables = pEnvironment->f_GetMember("Variables", EJsonType_Object))
			{
				auto &OutEnv = *(Config.m_EnvironmentVariables = NContainer::TCMap<NStr::CStr, NStr::CStr>{});
				for (auto &EnvVar : pVariables->f_Object())
					OutEnv[EnvVar.f_Name()] = EnvVar.f_Value().f_String();
			}
		}

		if (auto pValue = JsonConfig.f_GetMember("Handler", EJsonType_String))
			Config.m_Handler = pValue->f_String();

		if (auto pValue = JsonConfig.f_GetMember("KMSKeyArn", EJsonType_String))
			Config.m_KMSKeyArn = pValue->f_String();

		if (auto pValue = JsonConfig.f_GetMember("MemorySize", EJsonType_Integer))
			Config.m_MemorySizeMB = pValue->f_Integer();

		if (auto pValue = JsonConfig.f_GetMember("Role", EJsonType_String))
			Config.m_Role = pValue->f_String();

		if (auto pValue = JsonConfig.f_GetMember("Runtime", EJsonType_String))
			Config.m_Runtime = pValue->f_String();

		if (auto pValue = JsonConfig.f_GetMember("Timeout", EJsonType_Integer))
			Config.m_TimeoutSeconds = pValue->f_Integer();

		return Config;
	}

	TCFuture<CAwsLambdaActor::CFunctionInfo> CAwsLambdaActor::CInternal::f_CreateFunction(CStr _FunctionName, CCodeBlob _CodeBlob, CFunctionConfiguration _Config)
	{
		NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions"_f << m_Credentials.m_Region};
		CJsonSorted Request;
		Request["Code"]["ZipFile"] = _CodeBlob.m_Base64;
		Request["FunctionName"] = _FunctionName;

		if (_Config.m_Publish)
			Request["Publish"] = *_Config.m_Publish;

		if (_Config.m_Tags)
		{
			auto &Tags = Request["Tags"];
			for (auto &Value : *_Config.m_Tags)
				Tags[TCMap<CStr, CStr>::fs_GetKey(Value)] = Value;
		}

		fp_PopulateFunctionConfigurationRequest(Request, _Config);

		auto Results = co_await fg_DoAWSRequestJson("Create function", m_HttpClientActor, 201, AWSUrl, Request, CHttpClientActor::EMethod_POST, m_Credentials, {}, "lambda");
		CFunctionInfo FunctionInfo;
		FunctionInfo.m_Version = Results.f_GetMemberValue("Version", "").f_String();
		FunctionInfo.m_Arn = "{}:{}"_f << Results.f_GetMemberValue("FunctionArn", "").f_String() << FunctionInfo.m_Version;

		if (FunctionInfo.m_Version.f_IsEmpty())
			co_return DMibErrorInstance("No Version entry found for created function");
		if (FunctionInfo.m_Arn.f_IsEmpty())
			co_return DMibErrorInstance("No FunctionArn entry found for created function");

		co_await f_WaitForFunctionActive(FunctionInfo.m_Arn);

		co_return fg_Move(FunctionInfo);
	}

	TCFuture<void> CAwsLambdaActor::CInternal::f_UpdateFunctionConfiguration(CStr _FunctionName, CFunctionConfiguration _Config)
	{
		NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/configuration"_f << m_Credentials.m_Region << _FunctionName};
		CJsonSorted Request;

		fp_PopulateFunctionConfigurationRequest(Request, _Config);

		CJsonSorted Results = co_await fg_DoAWSRequestJson
			(
				"Update function configuration"
				, m_HttpClientActor
				, 200
				, AWSUrl
				, Request
				, CHttpClientActor::EMethod_PUT
				, m_Credentials
				, {}
				, "lambda"
			)
		;

		CFunctionInfo FunctionInfo;
		FunctionInfo.m_Arn = Results.f_GetMemberValue("FunctionArn", "").f_String();

		co_await f_WaitForFunctionActive(FunctionInfo.m_Arn);

		co_return {};
	}

	TCFuture<CAwsLambdaActor::CFunctionInfo> CAwsLambdaActor::CInternal::f_UpdateFunctionCode(CStr _FunctionName, CCodeBlob _CodeBlob, CFunctionConfiguration _Config)
	{
		NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/code"_f << m_Credentials.m_Region << _FunctionName};
		CJsonSorted Request;
		Request["ZipFile"] = _CodeBlob.m_Base64;

		if (_Config.m_Publish)
			Request["Publish"] = *_Config.m_Publish;

		CJsonSorted Results = co_await fg_DoAWSRequestJson("Update function code", m_HttpClientActor, 200, AWSUrl, Request, CHttpClientActor::EMethod_PUT, m_Credentials, {}, "lambda");

		CFunctionInfo FunctionInfo;
		FunctionInfo.m_Version = Results.f_GetMemberValue("Version", "").f_String();
		FunctionInfo.m_Arn = Results.f_GetMemberValue("FunctionArn", "").f_String();

		if (FunctionInfo.m_Version.f_IsEmpty())
			co_return DMibErrorInstance("No Version entry found for updated function");
		if (FunctionInfo.m_Arn.f_IsEmpty())
			co_return DMibErrorInstance("No FunctionArn entry found for updated function");

		co_await f_WaitForFunctionActive(FunctionInfo.m_Arn);

		co_return fg_Move(FunctionInfo);
	}

	TCFuture<CJsonSorted> CAwsLambdaActor::CInternal::f_GetFunctionVersions(CStr const &_FunctionName)
	{
		NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/versions?MaxItems=10000"_f << m_Credentials.m_Region << _FunctionName};
		return fg_DoAWSRequestJson("Get function versions", m_HttpClientActor, 200, AWSUrl, {}, CHttpClientActor::EMethod_GET, m_Credentials, {}, "lambda");
	}

	TCFuture<CAwsLambdaActor::CFunctionInfo> CAwsLambdaActor::CInternal::f_PublishVersion(CStr _FunctionName)
	{
		NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/versions"_f << m_Credentials.m_Region << _FunctionName};
		CJsonSorted Results = co_await fg_DoAWSRequestJson("Publish function", m_HttpClientActor, 201, AWSUrl, {}, CHttpClientActor::EMethod_POST, m_Credentials, {}, "lambda");

		CFunctionInfo FunctionInfo;
		FunctionInfo.m_Version = Results.f_GetMemberValue("Version", "").f_String();
		FunctionInfo.m_Arn = Results.f_GetMemberValue("FunctionArn", "").f_String();

		if (FunctionInfo.m_Version.f_IsEmpty())
			co_return DMibErrorInstance("No Version entry found for updated function");
		if (FunctionInfo.m_Arn.f_IsEmpty())
			co_return DMibErrorInstance("No FunctionArn entry found for updated function");

		co_await f_WaitForFunctionActive(FunctionInfo.m_Arn);

		co_return fg_Move(FunctionInfo);
	}

	TCFuture<void> CAwsLambdaActor::CInternal::f_WaitForFunctionActive(CStr _FunctionName)
	{
		NTime::CStopwatch Stopwatch(true);
		while (true)
		{
			CJsonSorted FunctionInfo = co_await f_GetFunction(_FunctionName);

			auto *pConfig = FunctionInfo.f_GetMember("Configuration", EJsonType_Object);
			if (!pConfig)
				co_return DMibErrorInstance("No function configuration when waiting for function to become active");

			auto *pState = pConfig->f_GetMember("State", EJsonType_String);
			if (!pState)
				co_return DMibErrorInstance("No function state when waiting for function to become active");

			auto &State = pState->f_String();

			auto fGetFailedErrorString = [&](CStr const &_ReasonName) -> CStr
				{
					if (auto pReason = pConfig->f_GetMember(_ReasonName, EJsonType_String))
						return "Function update failed ({}): {}"_f << _ReasonName << pReason->f_String();
					else
						return "Function update failed for unknown reason";
				}
			;

			if (State == "Failed")
				co_return DMibErrorInstance(fGetFailedErrorString("StateReason"));
			else if (State == "Active")
			{
				auto *pLastUpdateStatus = pConfig->f_GetMember("LastUpdateStatus", EJsonType_String);
				if (!pLastUpdateStatus)
					co_return {};

				auto &LastUpdateStatus = pLastUpdateStatus->f_String();
				if (LastUpdateStatus == "Failed")
					co_return DMibErrorInstance(fGetFailedErrorString("LastUpdateStatusReason"));
				else if (LastUpdateStatus == "Successful")
					co_return {};
			}

			if (Stopwatch.f_GetTime() > 60.0)
				co_return DMibErrorInstance("Timed out waiting for function to become active. State: {}"_f << State);

			co_await fg_Timeout(1.0);
		}

		co_return {};
	}

	CAwsLambdaActor::CAwsLambdaActor(TCActor<CHttpClientActor> const &_HttpClientActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_HttpClientActor, _Credentials)}
	{
	}

	CAwsLambdaActor::~CAwsLambdaActor() = default;

	TCFuture<CAwsLambdaActor::CFunctionInfo> CAwsLambdaActor::f_CreateOrUpdateFunction(CStr _FunctionName, TCMap<CStr, CStr> _Files, CFunctionConfiguration _Config)
	{
		auto &Internal = *mp_pInternal;

		CInternal::CCodeBlob CodeBlob = co_await Internal.f_CreateCodeBlob(_Files);
		TCAsyncResult<CJsonSorted> ExistingFunctionWrapped = co_await Internal.f_GetFunction(_FunctionName).f_Wrap();

		if (!ExistingFunctionWrapped)
		{
			bool bShouldCreate = false;

			NException::fg_VisitException<CExceptionAws>
				(
					ExistingFunctionWrapped.f_GetException()
					, [&](CExceptionAws const &_Exception)
					{
						if (_Exception.f_GetSpecific().m_StatusCode == 404)
							bShouldCreate = true;
					}
				)
			;

			if (bShouldCreate)
				co_return co_await Internal.f_CreateFunction(_FunctionName, fg_Move(CodeBlob), _Config);
			co_return ExistingFunctionWrapped.f_GetException();
		}

		auto &ExistingFunction = fg_Const(*ExistingFunctionWrapped);

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Unexpected return from get function");

			CFunctionConfiguration ExistingConfig = CInternal::fs_FunctionConfigFromJson(ExistingFunction);

			bool bChangedConfig = false;
			if (_Config.m_DeadLetterConfig.m_TargetArn && _Config.m_DeadLetterConfig.m_TargetArn != ExistingConfig.m_DeadLetterConfig.m_TargetArn)
				bChangedConfig = true;
			if (_Config.m_TracingConfig.m_Mode && _Config.m_TracingConfig.m_Mode != ExistingConfig.m_TracingConfig.m_Mode)
				bChangedConfig = true;
			if (_Config.m_VpcConfig.m_SecurityGroupIds && _Config.m_VpcConfig.m_SecurityGroupIds != ExistingConfig.m_VpcConfig.m_SecurityGroupIds)
				bChangedConfig = true;
			if (_Config.m_VpcConfig.m_SubnetIDs && _Config.m_VpcConfig.m_SubnetIDs != ExistingConfig.m_VpcConfig.m_SubnetIDs)
				bChangedConfig = true;
			if (_Config.m_Handler && _Config.m_Handler != ExistingConfig.m_Handler)
				bChangedConfig = true;
			if (_Config.m_Role && _Config.m_Role != ExistingConfig.m_Role)
				bChangedConfig = true;
			if (_Config.m_Runtime && _Config.m_Runtime != ExistingConfig.m_Runtime)
				bChangedConfig = true;
			if (_Config.m_EnvironmentVariables && _Config.m_EnvironmentVariables != ExistingConfig.m_EnvironmentVariables)
				bChangedConfig = true;
			if (_Config.m_Description && _Config.m_Description != ExistingConfig.m_Description)
				bChangedConfig = true;
			if (_Config.m_KMSKeyArn && _Config.m_KMSKeyArn != ExistingConfig.m_KMSKeyArn)
				bChangedConfig = true;
			if (_Config.m_MemorySizeMB && _Config.m_MemorySizeMB != ExistingConfig.m_MemorySizeMB)
				bChangedConfig = true;
			if (_Config.m_TimeoutSeconds && _Config.m_TimeoutSeconds != ExistingConfig.m_TimeoutSeconds)
				bChangedConfig = true;

			if (bChangedConfig)
				co_await Internal.f_UpdateFunctionConfiguration(_FunctionName, _Config);

			auto &ExistingConfigJson = ExistingFunction["Configuration"];

			NContainer::CByteVector HashData;
			NEncoding::fg_Base64Decode(ExistingConfigJson["CodeSha256"].f_String(), HashData);

			auto Hash = NCryptography::CHashDigest_SHA256::fs_FromBytes(HashData.f_GetArray(), HashData.f_GetLen());

			CFunctionInfo FunctionInfo;

			if (Hash != CodeBlob.m_SHA256)
				FunctionInfo = co_await Internal.f_UpdateFunctionCode(_FunctionName, CodeBlob, _Config);
			else
			{
				CJsonSorted VersionsJson = co_await Internal.f_GetFunctionVersions(_FunctionName);
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Unexpected return from get function versions");

					bool bAlreadyLatest = false;
					for (auto &VersionJson : VersionsJson["Versions"].f_Array())
					{
						NContainer::CByteVector HashData;
						NEncoding::fg_Base64Decode(VersionJson["CodeSha256"].f_String(), HashData);
						auto CodeHash = NCryptography::CHashDigest_SHA256::fs_FromBytes(HashData.f_GetArray(), HashData.f_GetLen());
						if (CodeHash == CodeBlob.m_SHA256)
						{
							CStr Version = VersionJson.f_GetMemberValue("Version", "").f_String();

							if (Version == "$LATEST")
								continue;

							bAlreadyLatest = true;
							FunctionInfo = CFunctionInfo{VersionJson.f_GetMemberValue("FunctionArn", "").f_String(), Version};
							break;
						}
					}
					if (!bAlreadyLatest)
					{
						if (_Config.m_Publish && *_Config.m_Publish)
							FunctionInfo = co_await Internal.f_PublishVersion(_FunctionName);
						else
							FunctionInfo = CFunctionInfo{ExistingConfigJson.f_GetMemberValue("FunctionArn", "").f_String(), ExistingConfigJson.f_GetMemberValue("Version", "").f_String()};
					}
				}
			}

			co_return fg_Move(FunctionInfo);
		}
	}
}
