// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Test/Exception>
#include <curl/curl.h>
#include <curl/external_resolver.h>

namespace
{
	using namespace NMib;
	using namespace NMib::NStr;

	struct CExternalResolver_Tests : NTest::CTest
	{
		void f_DoTests()
		{
			DMibTestSuite("OverrideLifecycleAndBlockingLookup")
			{
				DMibAssert(curl_global_init(CURL_GLOBAL_DEFAULT), ==, CURLE_OK);
				auto GlobalCleanup = g_OnScopeExit / []
					{
						curl_global_cleanup();
					}
				;
				CStr Diagnostics;
				CURL *pEasy = curl_easy_init();
				CURLM *pMulti = curl_multi_init();
				DMibAssertTrue(pEasy && pMulti);
				auto Cleanup = g_OnScopeExit / [&]
					{
						curl_multi_remove_handle(pMulti, pEasy);
						curl_easy_cleanup(pEasy);
						curl_multi_cleanup(pMulti);
					}
				;

				struct CLookup
				{
					umint m_Starts = 0;
					umint m_Polls = 0;
					umint m_Cancels = 0;
				};

				CLookup Lookup;

				curl_external_resolver Resolver{};
				Resolver.user = &Lookup;
				Resolver.start = [](void *_pUser, CURL *, char const *, int) -> void *
					{
						++static_cast<CLookup *>(_pUser)->m_Starts;

						return _pUser;
					}
				;
				Resolver.poll = [](void *_pLookup, curl_external_address const **, size_t *) -> int
					{
						++static_cast<CLookup *>(_pLookup)->m_Polls;

						return 0;
					}
				;
				Resolver.cancel = [](void *_pLookup)
					{
						++static_cast<CLookup *>(_pLookup)->m_Cancels;
					}
				;

				DMibExpect(curl_multi_set_external_resolver(pMulti, &Resolver), ==, CURLM_OK);
				DMibExpect(curl_multi_set_external_resolver(pMulti, nullptr), ==, CURLM_OK);
				curl_external_resolver Invalid{};
				DMibExpect(curl_multi_set_external_resolver(pMulti, &Invalid), ==, CURLM_BAD_FUNCTION_ARGUMENT);
				{
					DMibTestPath("Reinstall");

					DMibExpect(curl_multi_set_external_resolver(pMulti, &Resolver), ==, CURLM_OK);
				}

				char Error[CURL_ERROR_SIZE] = {};
				curl_easy_setopt(pEasy, CURLOPT_URL, "http://localhost:1/");
				curl_easy_setopt(pEasy, CURLOPT_PROXY, "");
				curl_easy_setopt(pEasy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
				curl_easy_setopt(pEasy, CURLOPT_VERBOSE, 1L);
				curl_easy_setopt(pEasy, CURLOPT_DEBUGDATA, &Diagnostics);
				curl_easy_setopt
					(
						pEasy
						, CURLOPT_DEBUGFUNCTION
						, +[](CURL *, curl_infotype _Type, char *_pText, size_t _nBytes, void *_pUser) -> int
						{
							if (_Type == CURLINFO_TEXT)
								*static_cast<CStr *>(_pUser) += CStr(_pText, _nBytes);

							return 0;
						}
					)
				;
				curl_easy_setopt(pEasy, CURLOPT_INTERFACE, "host!blocking-resolver-test.invalid");
				curl_easy_setopt(pEasy, CURLOPT_ERRORBUFFER, Error);
				curl_easy_setopt(pEasy, CURLOPT_TIMEOUT_MS, 2000L);
				DMibAssert(curl_multi_add_handle(pMulti, pEasy), ==, CURLM_OK);
				DMibExpect(curl_multi_set_external_resolver(pMulti, nullptr), ==, CURLM_BAD_FUNCTION_ARGUMENT);

				int nRunning = 1;
				while (nRunning)
				{
					if (curl_multi_perform(pMulti, &nRunning) != CURLM_OK)
						DMibError("Could not drive the test transfer");
					if (nRunning && curl_multi_poll(pMulti, nullptr, 0, 10, nullptr) != CURLM_OK)
						DMibError("Could not poll the test transfer");
				}

				int nRemaining;
				auto *pMessage = curl_multi_info_read(pMulti, &nRemaining);
				DMibAssertTrue(pMessage != nullptr);

				DMibExpect(pMessage->msg, ==, CURLMSG_DONE);
				DMibExpect(pMessage->data.result, ==, CURLE_INTERFACE_FAILED);
				DMibExpectTrue(Diagnostics.f_Find("External resolver cannot complete a blocking lookup") >= 0);
				DMibExpect(Lookup.m_Starts, ==, 1);
				DMibExpect(Lookup.m_Polls, >, 0);
				DMibExpect(Lookup.m_Cancels, ==, 1);

				DMibAssert(curl_multi_remove_handle(pMulti, pEasy), ==, CURLM_OK);
				{
					DMibTestPath("RestoreAfterTransfer");

					DMibExpect(curl_multi_set_external_resolver(pMulti, nullptr), ==, CURLM_OK);
				}
			};
		}
	};

	DMibTestRegister(CExternalResolver_Tests, Malterlib::Web);
}
