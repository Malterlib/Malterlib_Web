// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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
			CMiniZipAdaptor(TCMap<CStr, CStr> const &_Files)
				: mp_Files(_Files)
			{
			}

			void f_SetFunctions(zlib_filefunc_def *_pFileFunc)
			{
				_pFileFunc->zopen_file = fp_fopen_func;
				_pFileFunc->zread_file = fp_fread_func;
				_pFileFunc->zwrite_file = fp_fwrite_func;
				_pFileFunc->ztell_file = fp_ftell_func;
				_pFileFunc->zseek_file = fp_fseek_func;
				_pFileFunc->zclose_file = fp_fclose_func;
				_pFileFunc->zerror_file = fp_ferror_func;
				_pFileFunc->opaque = this;
			}

			void f_SetFunctions(zlib_filefunc64_def *_pFileFunc)
			{
				_pFileFunc->zopen64_file = fp_fopen64_func;
				_pFileFunc->zread_file = fp_fread_func;
				_pFileFunc->zwrite_file = fp_fwrite_func;
				_pFileFunc->ztell64_file = fp_ftell64_func;
				_pFileFunc->zseek64_file = fp_fseek64_func;
				_pFileFunc->zclose_file = fp_fclose_func;
				_pFileFunc->zerror_file = fp_ferror_func;
				_pFileFunc->opaque = this;
			}

			CByteVector f_GetOutFile(CStr const &_Path)
			{
				auto pFile = mp_OutFiles.f_FindEqual(_Path);
				if (!pFile)
					return {};
				return pFile->f_MoveVector();
			}

		private:
			static voidpf ZCALLBACK fp_fopen_func (voidpf opaque, const char* filename, int mode)
			{
				CMiniZipAdaptor *pThis = (CMiniZipAdaptor *)opaque;

				NStream::CBinaryStreamDefault *pFile;

				CStr FileName(filename);

				if (mode & ZLIB_FILEFUNC_MODE_EXISTING)
				{
					if (!(mode & ZLIB_FILEFUNC_MODE_READ))
						return nullptr;

					auto *pSourceFile = pThis->mp_Files.f_FindEqual(FileName);
					if (!pSourceFile)
						return nullptr;

					NStream::CBinaryStreamMemoryPtr<> *pNewFile = &pThis->mp_InFiles.f_Insert();

					pNewFile->f_OpenRead(pSourceFile->f_GetStr(), pSourceFile->f_GetLen());
					pFile = pNewFile;

				}
				else if (mode & ZLIB_FILEFUNC_MODE_CREATE)
				{
					if (!(mode & ZLIB_FILEFUNC_MODE_WRITE))
						return nullptr;
					pFile = &pThis->mp_OutFiles[FileName];
				}
				else
					return nullptr;

				return pFile;
			}

			static voidpf ZCALLBACK fp_fopen64_func (voidpf opaque, const void* filename, int mode)
			{
				return fp_fopen_func(opaque, (const ch8 *)filename, mode);
			}


			static uLong ZCALLBACK fp_fread_func (voidpf opaque, voidpf stream, void* buf, uLong size)
			{
				NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)stream;

				pFile->f_ConsumeBytes(buf, size);
				return size;
			}


			static uLong ZCALLBACK fp_fwrite_func (voidpf opaque, voidpf stream, const void* buf, uLong size)
			{
				NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)stream;

				pFile->f_FeedBytes(buf, size);
				return size;
			}

			static long ZCALLBACK fp_ftell_func (voidpf opaque, voidpf stream)
			{
				NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)stream;

				return pFile->f_GetPosition();
			}

			static ZPOS64_T ZCALLBACK fp_ftell64_func (voidpf opaque, voidpf stream)
			{
				NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)stream;

				return pFile->f_GetPosition();
			}

			static long ZCALLBACK fp_fseek_func (voidpf opaque, voidpf stream, uLong offset, int origin)
			{
				NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)stream;

				switch (origin)
				{
				case ZLIB_FILEFUNC_SEEK_CUR:
					pFile->f_AddPosition(offset);
					break;
				case ZLIB_FILEFUNC_SEEK_END:
					pFile->f_SetPositionFromEnd(offset);
					break;
				case ZLIB_FILEFUNC_SEEK_SET:
					pFile->f_SetPosition(offset);
					break;
				default: return -1;
				}

				return 0;
			}

			static long ZCALLBACK fp_fseek64_func (voidpf opaque, voidpf stream, ZPOS64_T offset, int origin)
			{
				NStream::CBinaryStreamDefault *pFile = (NStream::CBinaryStreamDefault *)stream;

				switch (origin)
				{
				case ZLIB_FILEFUNC_SEEK_CUR:
					pFile->f_AddPosition(offset);
					break;
				case ZLIB_FILEFUNC_SEEK_END:
					pFile->f_SetPositionFromEnd(offset);
					break;
				case ZLIB_FILEFUNC_SEEK_SET:
					pFile->f_SetPosition(offset);
					break;
				default: return -1;
				}

				return 0;
			}

			static int ZCALLBACK fp_fclose_func (voidpf opaque, voidpf stream)
			{
				return 0;
			}

			static int ZCALLBACK fp_ferror_func (voidpf opaque, voidpf stream)
			{
				/* We never return errors */
				return 0;
			}

			TCMap<CStr, CStr> const &mp_Files;
			TCLinkedList<NStream::CBinaryStreamMemoryPtr<>> mp_InFiles;
			TCMap<CStr, NStream::CBinaryStreamMemory<>> mp_OutFiles;
		};
	}

	struct CAwsLambdaActor::CInternal
	{
		
		CInternal(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
			: m_CurlActor{_CurlActor}
			, m_Credentials{_Credentials}
		{
			m_FileActor = fg_Construct(fg_Construct(), "AWS Lambda code compress actor");
		}

		CAwsCredentials m_Credentials;
		TCActor<CCurlActor> m_CurlActor;
		TCActor<CSeparateThreadActor> m_FileActor;

		static void fs_CheckZipError(int _Error, CStr const &_File)
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

		struct CCodeBlob
		{
			CStr m_Base64;
			NCryptography::CHashDigest_SHA256 m_SHA256;
		};

		TCContinuation<CCodeBlob> f_CreateCodeBlob(TCMap<CStr, CStr> const &_Files)
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
						auto Cleanup = g_OnScopeExit > [&]()
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

		TCContinuation<CJSON> f_GetFunction(CStr const &_FunctionName)
		{
			NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}"_f << m_Credentials.m_Region << _FunctionName};
			return fg_DoAWSRequestJSON("Get function", m_CurlActor, 200, AWSUrl, {}, CCurlActor::EMethod_GET, m_Credentials, {}, "lambda");
		}

		static CStr fsp_TracingConfigToString(CFunctionConfiguration::ETracingMode _Mode)
		{
			switch (_Mode)
			{
			case CFunctionConfiguration::ETracingMode_Active: return "Active";
			case CFunctionConfiguration::ETracingMode_PassThrough: return "PassThrough";
			}
			return "";
		}

		static CFunctionConfiguration::ETracingMode fsp_TracingConfigFromString(CStr const &_String)
		{
			if (_String == "Active")
				return CFunctionConfiguration::ETracingMode_Active;
			else if (_String == "PassThrough")
				return CFunctionConfiguration::ETracingMode_PassThrough;
			return CFunctionConfiguration::ETracingMode_Unknown;
		}

		void fp_PopulateFunctionConfigurationRequest(CJSON &o_Request, CFunctionConfiguration const &_Config)
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

		static CFunctionConfiguration fsp_FunctionConfigFromJSON(CJSON const &_JSON)
		{
			auto &JSONConfig = _JSON["Configuration"];

			CFunctionConfiguration Config;

			if (auto pValue = JSONConfig.f_GetMember("DeadLetterConfig", EJSONType_Object))
			{
				if (auto pTargetArn = pValue->f_GetMember("TargetArn", EJSONType_String))
					Config.m_DeadLetterConfig.m_TargetArn = pTargetArn->f_String();
			}

			if (auto pValue = JSONConfig.f_GetMember("TracingConfig", EJSONType_Object))
			{
				if (auto pTargetArn = pValue->f_GetMember("Mode", EJSONType_String))
					Config.m_TracingConfig.m_Mode = fsp_TracingConfigFromString(pTargetArn->f_String());
			}

			if (auto pValue = JSONConfig.f_GetMember("VpcConfig", EJSONType_Object))
			{
				if (auto pSecurityGroupIds = pValue->f_GetMember("SecurityGroupIds", EJSONType_Array))
				{
					auto &OutSecurityGroupIds = *(Config.m_VpcConfig.m_SecurityGroupIds = NContainer::TCVector<NStr::CStr>{});
					for (auto &ID : pSecurityGroupIds->f_Array())
						OutSecurityGroupIds.f_Insert(ID.f_String());
				}

				if (auto pSubnetIDs = pValue->f_GetMember("SubnetIDs", EJSONType_Array))
				{
					auto &OutSubnetIDs = *(Config.m_VpcConfig.m_SubnetIDs = NContainer::TCVector<NStr::CStr>{});
					for (auto &ID : pSubnetIDs->f_Array())
						OutSubnetIDs.f_Insert(ID.f_String());
				}
			}

			if (auto pValue = JSONConfig.f_GetMember("Description", EJSONType_String))
				Config.m_Description = pValue->f_String();

			if (auto pEnvironment = JSONConfig.f_GetMember("Environment", EJSONType_Object))
			{
				if (auto pVariables = JSONConfig.f_GetMember("Variables", EJSONType_Object))
				{
					auto &OutEnv = *(Config.m_EnvironmentVariables = NContainer::TCMap<NStr::CStr, NStr::CStr>{});
					for (auto &EnvVar : pVariables->f_Object())
					{
						OutEnv[EnvVar.f_Name()] = EnvVar.f_Value().f_String();
					}
				}
			}

			if (auto pValue = JSONConfig.f_GetMember("Handler", EJSONType_String))
				Config.m_Handler = pValue->f_String();

			if (auto pValue = JSONConfig.f_GetMember("KMSKeyArn", EJSONType_String))
				Config.m_KMSKeyArn = pValue->f_String();

			if (auto pValue = JSONConfig.f_GetMember("MemorySize", EJSONType_Integer))
				Config.m_MemorySizeMB = pValue->f_Integer();

			if (auto pValue = JSONConfig.f_GetMember("Role", EJSONType_String))
				Config.m_Role = pValue->f_String();

			if (auto pValue = JSONConfig.f_GetMember("Runtime", EJSONType_String))
				Config.m_Runtime = pValue->f_String();

			if (auto pValue = JSONConfig.f_GetMember("Timeout", EJSONType_Integer))
				Config.m_TimeoutSeconds = pValue->f_Integer();

			return Config;
		}

		TCContinuation<CFunctionInfo> f_CreateFunction
			(
				CStr const &_FunctionName
				, CCodeBlob const &_CodeBlob
				, CFunctionConfiguration const &_Config
			)
		{
			NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions"_f << m_Credentials.m_Region};
			CJSON Request;
			Request["Code"]["ZipFile"] = _CodeBlob.m_Base64;
			Request["FunctionName"] = _FunctionName;

			if (_Config.m_bPublish)
				Request["Publish"] = *_Config.m_bPublish;

			if (_Config.m_Tags)
			{
				auto &Tags = Request["Tags"];
				for (auto &Value : *_Config.m_Tags)
					Tags[TCMap<CStr, CStr>::fs_GetKey(Value)] = Value;
			}

			fp_PopulateFunctionConfigurationRequest(Request, _Config);

			TCContinuation<CFunctionInfo> Continuation;

			fg_DoAWSRequestJSON("Create function", m_CurlActor, 201, AWSUrl, Request, CCurlActor::EMethod_POST, m_Credentials, {}, "lambda")
				> Continuation / [=](CJSON &&_Results)
				{
					CFunctionInfo FunctionInfo;
					FunctionInfo.m_Version = _Results.f_GetMemberValue("Version", "").f_String();
					FunctionInfo.m_Arn = "{}:{}"_f << _Results.f_GetMemberValue("FunctionArn", "").f_String() << FunctionInfo.m_Version;

					if (FunctionInfo.m_Version.f_IsEmpty())
						return Continuation.f_SetException(DMibErrorInstance("No Version entry found for created function"));
					if (FunctionInfo.m_Arn.f_IsEmpty())
						return Continuation.f_SetException(DMibErrorInstance("No FunctionArn entry found for created function"));

					Continuation.f_SetResult(fg_Move(FunctionInfo));
				}
			;

			return Continuation;
		}

		TCContinuation<void> f_UpdateFunctionConfiguration
			(
				CStr const &_FunctionName
				, CFunctionConfiguration const &_Config
			)
		{
			NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/configuration"_f << m_Credentials.m_Region << _FunctionName};
			CJSON Request;

			fp_PopulateFunctionConfigurationRequest(Request, _Config);

			TCContinuation<void> Continuation;

			fg_DoAWSRequestJSON("Update function configuration", m_CurlActor, 200, AWSUrl, Request, CCurlActor::EMethod_PUT, m_Credentials, {}, "lambda")
				> Continuation / [=](CJSON &&_Results)
				{
					Continuation.f_SetResult();
				}
			;

			return Continuation;
		}

		TCContinuation<CFunctionInfo> f_UpdateFunctionCode
			(
				CStr const &_FunctionName
				, CCodeBlob const &_CodeBlob
				, CFunctionConfiguration const &_Config
			)
		{
			NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/code"_f << m_Credentials.m_Region << _FunctionName};
			CJSON Request;
			Request["ZipFile"] = _CodeBlob.m_Base64;

			if (_Config.m_bPublish)
				Request["Publish"] = *_Config.m_bPublish;

			TCContinuation<CFunctionInfo> Continuation;

			fg_DoAWSRequestJSON("Update function code", m_CurlActor, 200, AWSUrl, Request, CCurlActor::EMethod_PUT, m_Credentials, {}, "lambda")
				> Continuation / [=](CJSON &&_Results)
				{
					CFunctionInfo FunctionInfo;
					FunctionInfo.m_Version = _Results.f_GetMemberValue("Version", "").f_String();
					FunctionInfo.m_Arn = _Results.f_GetMemberValue("FunctionArn", "").f_String();

					if (FunctionInfo.m_Version.f_IsEmpty())
						return Continuation.f_SetException(DMibErrorInstance("No Version entry found for updated function"));
					if (FunctionInfo.m_Arn.f_IsEmpty())
						return Continuation.f_SetException(DMibErrorInstance("No FunctionArn entry found for updated function"));

					Continuation.f_SetResult(fg_Move(FunctionInfo));
				}
			;

			return Continuation;
		}

		TCContinuation<CJSON> f_GetFunctionVersions(CStr const &_FunctionName)
		{
			NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/versions?MaxItems=10000"_f << m_Credentials.m_Region << _FunctionName};
			return fg_DoAWSRequestJSON("Get function versions", m_CurlActor, 200, AWSUrl, {}, CCurlActor::EMethod_GET, m_Credentials, {}, "lambda");
		}

		TCContinuation<CFunctionInfo> f_PublishVersion(CStr const &_FunctionName)
		{
			NHTTP::CURL AWSUrl = CStr{"https://lambda.{}.amazonaws.com/2015-03-31/functions/{}/versions"_f << m_Credentials.m_Region << _FunctionName};
			TCContinuation<CFunctionInfo> Continuation;
			fg_DoAWSRequestJSON("Publish function", m_CurlActor, 201, AWSUrl, {}, CCurlActor::EMethod_POST, m_Credentials, {}, "lambda")
				> Continuation / [=](CJSON &&_Results)
				{
					CFunctionInfo FunctionInfo;
					FunctionInfo.m_Version = _Results.f_GetMemberValue("Version", "").f_String();
					FunctionInfo.m_Arn = _Results.f_GetMemberValue("FunctionArn", "").f_String();

					if (FunctionInfo.m_Version.f_IsEmpty())
						return Continuation.f_SetException(DMibErrorInstance("No Version entry found for updated function"));
					if (FunctionInfo.m_Arn.f_IsEmpty())
						return Continuation.f_SetException(DMibErrorInstance("No FunctionArn entry found for updated function"));

					Continuation.f_SetResult(fg_Move(FunctionInfo));
				}
			;

			return Continuation;
		}
	};

	CAwsLambdaActor::CAwsLambdaActor(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_CurlActor, _Credentials)}
	{
	}

	CAwsLambdaActor::~CAwsLambdaActor() = default;

	TCContinuation<CAwsLambdaActor::CFunctionInfo> CAwsLambdaActor::f_CreateOrUpdateFunction
		(
		 	CStr const &_FunctionName
		 	, TCMap<CStr, CStr> const &_Files
		 	, CFunctionConfiguration const &_Config
		)
	{
		auto &Internal = *mp_pInternal;

		TCContinuation<CFunctionInfo> Continuation;
		Internal.f_CreateCodeBlob(_Files) > Continuation / [=](CInternal::CCodeBlob &&_CodeBlob)
			{
				auto &Internal = *mp_pInternal;

				Internal.f_GetFunction(_FunctionName) > [=](TCAsyncResult<CJSON> &&_ExistingFunction)
					{
						auto &Internal = *mp_pInternal;

						if (!_ExistingFunction)
						{
							try
							{
								_ExistingFunction.f_Access();
							}
							catch (CExceptionAws const &_Exception)
							{
								if (_Exception.f_GetSpecific().m_StatusCode == 404)
								{
									Internal.f_CreateFunction(_FunctionName, _CodeBlob, _Config) > Continuation;
									return;
								}
							}
							catch (NException::CException const &)
							{
							}
							return Continuation.f_SetException(_ExistingFunction);
						}

						auto &ExistingFunction = fg_Const(*_ExistingFunction);

						try
						{
							CFunctionConfiguration ExistingConfig = CInternal::fsp_FunctionConfigFromJSON(ExistingFunction);

							auto &ExistingConfigJSON = ExistingFunction["Configuration"];

							NContainer::CByteVector HashData;
							NEncoding::fg_Base64Decode(ExistingConfigJSON["CodeSha256"].f_String(), HashData);

							auto Hash = NCryptography::CHashDigest_SHA256::fs_FromBytes(HashData.f_GetArray(), HashData.f_GetLen());

							CFunctionInfo LatestFunctionInfo
								{
									ExistingConfigJSON.f_GetMemberValue("FunctionArn", "").f_String()
									, ExistingConfigJSON.f_GetMemberValue("Version", "").f_String()
								}
							;

							TCContinuation<CFunctionInfo> UpdateFunctionCodeContinuation;
							if (Hash != _CodeBlob.m_SHA256)
								UpdateFunctionCodeContinuation = Internal.f_UpdateFunctionCode(_FunctionName, _CodeBlob, _Config);
							else
							{
								Internal.f_GetFunctionVersions(_FunctionName) > UpdateFunctionCodeContinuation / [=](CJSON const &_VersionsJSON)
									{
										auto &Internal = *mp_pInternal;
										try
										{
											for (auto &VersionJSON : _VersionsJSON["Versions"].f_Array())
											{
												NContainer::CByteVector HashData;
												NEncoding::fg_Base64Decode(VersionJSON["CodeSha256"].f_String(), HashData);
												auto CodeHash = NCryptography::CHashDigest_SHA256::fs_FromBytes(HashData.f_GetArray(), HashData.f_GetLen());
												if (CodeHash == _CodeBlob.m_SHA256)
												{
													CStr Version = VersionJSON.f_GetMemberValue("Version", "").f_String();

													if (Version == "$LATEST")
														continue;
													UpdateFunctionCodeContinuation.f_SetResult
														(
															CFunctionInfo
															{
																VersionJSON.f_GetMemberValue("FunctionArn", "").f_String()
																, Version
															}
														)
													;
													return;
												}
											}
											if (_Config.m_bPublish && *_Config.m_bPublish)
												Internal.f_PublishVersion(_FunctionName) > UpdateFunctionCodeContinuation;
											else
												UpdateFunctionCodeContinuation.f_SetResult(LatestFunctionInfo);
										}
										catch (NException::CException const &_Exception)
										{
											UpdateFunctionCodeContinuation.f_SetException(DMibErrorInstance("Unexpected return from get function versions: {}"_f << _Exception));
										}
									}
								;
							}

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

							TCContinuation<void> UpdateFunctionConfigurationContinuation;
							if (bChangedConfig)
								UpdateFunctionConfigurationContinuation = Internal.f_UpdateFunctionConfiguration(_FunctionName, _Config);
							else
								UpdateFunctionConfigurationContinuation.f_SetResult();

							UpdateFunctionCodeContinuation + UpdateFunctionConfigurationContinuation > Continuation / [Continuation](CFunctionInfo &&_Info, CVoidTag)
								{
									Continuation.f_SetResult(fg_Move(_Info));
								}
							;
						}
						catch (NException::CException const &_Exception)
						{
							Continuation.f_SetException(DMibErrorInstance("Unexpected return from get function: {}"_f << _Exception));
						}
					}
				;
			}
		;

		return Continuation;
	}
}
