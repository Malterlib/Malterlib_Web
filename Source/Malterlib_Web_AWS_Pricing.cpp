// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Web/Curl>
#include <Mib/Encoding/Json>

#include "Malterlib_Web_AWS_Pricing.h"
#include "Malterlib_Web_AWS_Internal.h"

namespace NMib::NWeb
{
	using namespace NStr;
	using namespace NContainer;
	using namespace NConcurrency;
	using namespace NStorage;
	using namespace NEncoding;

	struct CAwsPricingActor::CInternal : public NConcurrency::CActorInternal
	{
		CInternal(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
			: m_CurlActor{_CurlActor}
			, m_Credentials{_Credentials}
		{
		}

		CAwsCredentials m_Credentials;
		TCActor<CCurlActor> m_CurlActor;
	};

	CAwsPricingActor::CAwsPricingActor(TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials)
		: mp_pInternal{fg_Construct(_CurlActor, _Credentials)}
	{
	}

	CAwsPricingActor::~CAwsPricingActor() = default;

	TCFuture<TCVector<CAwsPricingActor::CEC2PriceInfo>> CAwsPricingActor::f_GetEC2Prices(CStr _Region)
	{
		using namespace NStr;

		auto &Internal = *mp_pInternal;

		TCVector<CEC2PriceInfo> Results;

		// Pricing API is only available from us-east-1
		NHTTP::CURL URL{"https://api.pricing.us-east-1.amazonaws.com/"};

		// AWS Pricing API uses JSON-RPC with X-Amz-Target header
		TCMap<CStr, CStr> AWSHeaders;
		AWSHeaders["Content-Type"] = "application/x-amz-json-1.1";
		AWSHeaders["X-Amz-Target"] = "AWSPriceListService.GetProducts";

		// Create credentials for us-east-1 region
		CAwsCredentials Credentials = Internal.m_Credentials;
		Credentials.m_Region = "us-east-1";

		// Helper to extract string attribute from JSON object
		auto fGetAttr = [](CJsonSorted const *_pAttrs, ch8 const *_pName) -> CStr
		{
			if (auto *p = _pAttrs->f_GetMember(_pName, EJsonType_String))
				return p->f_String();
			return {};
		};

		CStr NextToken;

		// Pagination loop
		do
		{
			// Build request (no instanceType filter - get all)
			CJsonSorted Request;
			Request["ServiceCode"] = "AmazonEC2";
			Request["FormatVersion"] = "aws_v1";

			auto &Filters = Request["Filters"];

			// Region filter
			{
				auto &Filter = Filters.f_Insert();
				Filter["Type"] = "TERM_MATCH";
				Filter["Field"] = "regionCode";
				Filter["Value"] = _Region;
			}

			// Operating system filter (Linux)
			{
				auto &Filter = Filters.f_Insert();
				Filter["Type"] = "TERM_MATCH";
				Filter["Field"] = "operatingSystem";
				Filter["Value"] = "Linux";
			}

			// Tenancy filter (Shared)
			{
				auto &Filter = Filters.f_Insert();
				Filter["Type"] = "TERM_MATCH";
				Filter["Field"] = "tenancy";
				Filter["Value"] = "Shared";
			}

			// Pre-installed software (None)
			{
				auto &Filter = Filters.f_Insert();
				Filter["Type"] = "TERM_MATCH";
				Filter["Field"] = "preInstalledSw";
				Filter["Value"] = "NA";
			}

			// Capacity status (Used = on-demand)
			{
				auto &Filter = Filters.f_Insert();
				Filter["Type"] = "TERM_MATCH";
				Filter["Field"] = "capacitystatus";
				Filter["Value"] = "Used";
			}
			{
				auto &Filter = Filters.f_Insert();
				Filter["Type"] = "TERM_MATCH";
				Filter["Field"] = "marketoption";
				Filter["Value"] = "OnDemand";
			}

			// Add pagination token if we have one
			if (NextToken)
				Request["NextToken"] = NextToken;

			CJsonSorted Result = co_await fg_DoAWSRequestJson
				(
					"GetProducts (EC2)"
					, Internal.m_CurlActor
					, 200
					, URL
					, Request
					, CCurlActor::EMethod_POST
					, Credentials
					, AWSHeaders
					, "pricing"
				)
			;

			auto *pPriceList = Result.f_GetMember("PriceList", EJsonType_Array);
			if (pPriceList)
			{
				// The PriceList contains stringified JSON, so we need to parse each item
				for (auto const &PriceItem : pPriceList->f_Array())
				{
					if (PriceItem.f_Type() != EJsonType_String)
						continue;

					CJsonSorted PriceJson;
					{
						auto CaptureScope = co_await (g_CaptureExceptions % "Error parsing EC2 price JSON");
						PriceJson = CJsonSorted::fs_FromString(PriceItem.f_String());
					}

					// Extract product attributes
					auto *pProduct = PriceJson.f_GetMember("product", EJsonType_Object);
					if (!pProduct)
						continue;

					auto *pAttributes = pProduct->f_GetMember("attributes", EJsonType_Object);
					if (!pAttributes)
						continue;

					// Get instance type from attributes
					CStr InstanceType = fGetAttr(pAttributes, "instanceType");
					if (InstanceType.f_IsEmpty())
						continue;

					// Navigate: terms.OnDemand.*.priceDimensions.*.pricePerUnit.USD
					auto *pTerms = PriceJson.f_GetMember("terms", EJsonType_Object);
					if (!pTerms)
						continue;

					auto *pOnDemand = pTerms->f_GetMember("OnDemand", EJsonType_Object);
					if (!pOnDemand)
						continue;

					// OnDemand contains SKU-keyed objects - find the price
					CEC2PriceInfo *pInfo = nullptr;
					for (auto const &OnDemandTerm : pOnDemand->f_Object())
					{
						auto *pPriceDimensions = OnDemandTerm.f_Value().f_GetMember("priceDimensions", EJsonType_Object);
						if (!pPriceDimensions)
							continue;

						for (auto const &Dimension : pPriceDimensions->f_Object())
						{
							auto *pPricePerUnit = Dimension.f_Value().f_GetMember("pricePerUnit", EJsonType_Object);
							if (!pPricePerUnit)
								continue;

							auto *pUSD = pPricePerUnit->f_GetMember("USD", EJsonType_String);
							if (!pUSD)
								continue;

							pInfo = &Results.f_Insert();
							pInfo->m_InstanceType = InstanceType;
							pInfo->m_OperatingSystem = "Linux";
							pInfo->m_OnDemandPricePerHour = pUSD->f_String().f_ToFloatExact(fp64(0.0));

							// Extract all instance attributes
							pInfo->m_ClockSpeed = fGetAttr(pAttributes, "clockSpeed");
							pInfo->m_CurrentGeneration = fGetAttr(pAttributes, "currentGeneration");
							pInfo->m_DedicatedEbsThroughput = fGetAttr(pAttributes, "dedicatedEbsThroughput");
							pInfo->m_Ecu = fGetAttr(pAttributes, "ecu");
							pInfo->m_EnhancedNetworkingSupported = fGetAttr(pAttributes, "enhancedNetworkingSupported");
							pInfo->m_Memory = fGetAttr(pAttributes, "memory");
							pInfo->m_NetworkPerformance = fGetAttr(pAttributes, "networkPerformance");
							pInfo->m_NormalizationSizeFactor = fGetAttr(pAttributes, "normalizationSizeFactor");
							pInfo->m_PhysicalProcessor = fGetAttr(pAttributes, "physicalProcessor");
							pInfo->m_ProcessorArchitecture = fGetAttr(pAttributes, "processorArchitecture");
							pInfo->m_ProcessorFeatures = fGetAttr(pAttributes, "processorFeatures");
							pInfo->m_Storage = fGetAttr(pAttributes, "storage");
							pInfo->m_Tenancy = fGetAttr(pAttributes, "tenancy");
							pInfo->m_Vcpu = fGetAttr(pAttributes, "vcpu");

							break;
						}

						if (pInfo)
							break;
					}

					// Also parse Reserved terms for the same instance type
					auto *pReserved = pTerms->f_GetMember("Reserved", EJsonType_Object);
					if (pReserved && pInfo)
					{
						for (auto const &ReservedTerm : pReserved->f_Object())
						{
							// Get term attributes
							auto *pTermAttrs = ReservedTerm.f_Value().f_GetMember("termAttributes", EJsonType_Object);
							if (!pTermAttrs)
								continue;

							CStr LeaseLength;	// "1yr" or "3yr"
							CStr OfferingClass;	// "standard" or "convertible"
							CStr PurchaseOption;	// "No Upfront", "Partial Upfront", "All Upfront"

							if (auto *p = pTermAttrs->f_GetMember("LeaseContractLength", EJsonType_String))
								LeaseLength = p->f_String();
							if (auto *p = pTermAttrs->f_GetMember("OfferingClass", EJsonType_String))
								OfferingClass = p->f_String();
							if (auto *p = pTermAttrs->f_GetMember("PurchaseOption", EJsonType_String))
								PurchaseOption = p->f_String();

							if (LeaseLength.f_IsEmpty() || OfferingClass.f_IsEmpty() || PurchaseOption.f_IsEmpty())
								continue;

							// Get hourly price and upfront fee from priceDimensions
							auto *pPriceDimensions = ReservedTerm.f_Value().f_GetMember("priceDimensions", EJsonType_Object);
							if (!pPriceDimensions)
								continue;

							fp64 HourlyRate = 0;
							fp64 UpfrontFee = 0;

							for (auto const &Dimension : pPriceDimensions->f_Object())
							{
								auto *pUnit = Dimension.f_Value().f_GetMember("unit", EJsonType_String);
								if (!pUnit)
									continue;

								auto *pPricePerUnit = Dimension.f_Value().f_GetMember("pricePerUnit", EJsonType_Object);
								if (!pPricePerUnit)
									continue;

								auto *pUSD = pPricePerUnit->f_GetMember("USD", EJsonType_String);
								if (!pUSD)
									continue;

								CStr Unit = pUnit->f_String();
								if (Unit == "Hrs")
									HourlyRate = pUSD->f_String().f_ToFloatExact(fp64(0.0));
								else if (Unit == "Quantity")
									UpfrontFee = pUSD->f_String().f_ToFloatExact(fp64(0.0));
							}

							// Calculate term length in hours (using 365.25 days/year for accuracy)
							fp64 TermHours = (LeaseLength == "1yr") ? (365.25 * 24.0) : (3.0 * 365.25 * 24.0);

							// Amortize upfront fee into hourly rate
							fp64 EffectiveHourlyRate = HourlyRate + (UpfrontFee / TermHours);

							// Build key and store effective hourly price
							CStr Key = "{}|{}|{}"_f << LeaseLength << OfferingClass << PurchaseOption;
							pInfo->m_ReservedPrices[Key] = EffectiveHourlyRate;
						}
					}
				}
			}

			// Check for next page
			NextToken.f_Clear();
			if (auto *pNextToken = Result.f_GetMember("NextToken", EJsonType_String))
				NextToken = pNextToken->f_String();

		} while (NextToken);

		co_return Results;
	}

	TCFuture<CAwsPricingActor::CGP3Prices> CAwsPricingActor::fp_QueryGP3Prices(CStr _RegionCode)
	{
		auto &Internal = *mp_pInternal;

		NHTTP::CURL URL{"https://api.pricing.us-east-1.amazonaws.com/"};

		TCMap<CStr, CStr> AWSHeaders;
		AWSHeaders["Content-Type"] = "application/x-amz-json-1.1";
		AWSHeaders["X-Amz-Target"] = "AWSPriceListService.GetProducts";

		CAwsCredentials Credentials = Internal.m_Credentials;
		Credentials.m_Region = "us-east-1";

		CJsonSorted Request;
		Request["ServiceCode"] = "AmazonEC2";
		Request["FormatVersion"] = "aws_v1";

		auto &Filters = Request["Filters"];

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "regionCode";
			Filter["Value"] = _RegionCode;
		}

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "volumeApiName";
			Filter["Value"] = "gp3";
		}

		CJsonSorted Response = co_await fg_DoAWSRequestJson
			(
				"GetProducts (EBS gp3)"
				, Internal.m_CurlActor
				, 200
				, URL
				, Request
				, CCurlActor::EMethod_POST
				, Credentials
				, AWSHeaders
				, "pricing"
			)
		;

		CGP3Prices Result;

		auto *pPriceList = Response.f_GetMember("PriceList", EJsonType_Array);
		if (pPriceList && !pPriceList->f_Array().f_IsEmpty())
		{
			for (auto const &PriceItem : pPriceList->f_Array())
			{
				if (PriceItem.f_Type() != EJsonType_String)
					continue;

				CJsonSorted PriceJson;
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Error parsing EBS gp3 price JSON");
					PriceJson = CJsonSorted::fs_FromString(PriceItem.f_String());
				}

				// Get product family to determine price type
				auto *pProduct = PriceJson.f_GetMember("product", EJsonType_Object);
				if (!pProduct)
					continue;

				auto *pProductFamily = pProduct->f_GetMember("productFamily", EJsonType_String);
				if (!pProductFamily)
					continue;

				CStr ProductFamily = pProductFamily->f_String();

				auto *pTerms = PriceJson.f_GetMember("terms", EJsonType_Object);
				if (!pTerms)
					continue;

				auto *pOnDemand = pTerms->f_GetMember("OnDemand", EJsonType_Object);
				if (!pOnDemand)
					continue;

				for (auto const &OnDemandTerm : pOnDemand->f_Object())
				{
					auto *pPriceDimensions = OnDemandTerm.f_Value().f_GetMember("priceDimensions", EJsonType_Object);
					if (!pPriceDimensions)
						continue;

					for (auto const &Dimension : pPriceDimensions->f_Object())
					{
						auto *pPricePerUnit = Dimension.f_Value().f_GetMember("pricePerUnit", EJsonType_Object);
						if (!pPricePerUnit)
							continue;

						auto *pUSD = pPricePerUnit->f_GetMember("USD", EJsonType_String);
						if (!pUSD)
							continue;

						auto *pUnit = Dimension.f_Value().f_GetMember("unit", EJsonType_String);
						if (!pUnit)
							co_return DMibErrorInstance("Internal error: Missing unit for Provisioned Throughput");

						fp64 Price = pUSD->f_String().f_ToFloatExact(fp64(0.0));

						if (ProductFamily == "Storage")
						{
							if (pUnit->f_String() != "GB-Mo")
								co_return DMibErrorInstance("Internal error: Expected unit to be 'GB-Mo', not '{}'"_f << pUnit->f_String());

							Result.m_StoragePricePerGBMonth = Price;
						}
						else if (ProductFamily == "System Operation")
						{
							if (pUnit->f_String() != "IOPS-Mo")
								co_return DMibErrorInstance("Internal error: Expected unit to be 'IOPS-Mo', not '{}'"_f << pUnit->f_String());

							Result.m_IOPSPricePerMonth = Price;
						}
						else if (ProductFamily == "Provisioned Throughput")
						{
							if (pUnit->f_String() != "GiBps-mo")
								co_return DMibErrorInstance("Internal error: Expected unit to be 'GiBps-mo', not '{}'"_f << pUnit->f_String());

							Result.m_ThroughputPricePerMBsMonth = Price / 1024.0;
						}

						break;
					}
					break;
				}
			}
		}

		co_return Result;
	}

	TCFuture<CAwsPricingActor::CSnapshotPrices> CAwsPricingActor::fp_QuerySnapshotPrices(CStr _RegionCode)
	{
		auto &Internal = *mp_pInternal;

		NHTTP::CURL URL{"https://api.pricing.us-east-1.amazonaws.com/"};

		TCMap<CStr, CStr> AWSHeaders;
		AWSHeaders["Content-Type"] = "application/x-amz-json-1.1";
		AWSHeaders["X-Amz-Target"] = "AWSPriceListService.GetProducts";

		CAwsCredentials Credentials = Internal.m_Credentials;
		Credentials.m_Region = "us-east-1";

		CJsonSorted Request;
		Request["ServiceCode"] = "AmazonEC2";
		Request["FormatVersion"] = "aws_v1";

		auto &Filters = Request["Filters"];

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "regionCode";
			Filter["Value"] = _RegionCode;
		}

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "productFamily";
			Filter["Value"] = "Storage Snapshot";
		}

		CJsonSorted Response = co_await fg_DoAWSRequestJson
			(
				"GetProducts (EBS Snapshot)"
				, Internal.m_CurlActor
				, 200
				, URL
				, Request
				, CCurlActor::EMethod_POST
				, Credentials
				, AWSHeaders
				, "pricing"
			)
		;

		CSnapshotPrices Result;

		auto *pPriceList = Response.f_GetMember("PriceList", EJsonType_Array);
		if (pPriceList && !pPriceList->f_Array().f_IsEmpty())
		{
			for (auto const &PriceItem : pPriceList->f_Array())
			{
				if (PriceItem.f_Type() != EJsonType_String)
					continue;

				CJsonSorted PriceJson;
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Error parsing EBS snapshot price JSON");
					PriceJson = CJsonSorted::fs_FromString(PriceItem.f_String());
				}

				// Check the usagetype to determine if it's standard or archive tier
				auto *pProduct = PriceJson.f_GetMember("product", EJsonType_Object);
				if (!pProduct)
					continue;

				auto *pAttributes = pProduct->f_GetMember("attributes", EJsonType_Object);
				if (!pAttributes)
					continue;

				auto *pUsageType = pAttributes->f_GetMember("usagetype", EJsonType_String);
				if (!pUsageType)
					continue;

				CStr UsageType = pUsageType->f_String();

				auto *pTerms = PriceJson.f_GetMember("terms", EJsonType_Object);
				if (!pTerms)
					continue;

				auto *pOnDemand = pTerms->f_GetMember("OnDemand", EJsonType_Object);
				if (!pOnDemand)
					continue;

				for (auto const &OnDemandTerm : pOnDemand->f_Object())
				{
					auto *pPriceDimensions = OnDemandTerm.f_Value().f_GetMember("priceDimensions", EJsonType_Object);
					if (!pPriceDimensions)
						continue;

					for (auto const &Dimension : pPriceDimensions->f_Object())
					{
						auto *pPricePerUnit = Dimension.f_Value().f_GetMember("pricePerUnit", EJsonType_Object);
						if (!pPricePerUnit)
							continue;

						auto *pUSD = pPricePerUnit->f_GetMember("USD", EJsonType_String);
						if (!pUSD)
							continue;

						fp64 Price = pUSD->f_String().f_ToFloatExact(fp64(0.0));

						// Match specific usage types for storage pricing:
						// - EBS:SnapshotUsage = standard snapshot storage
						// - EBS:SnapshotArchiveStorage = archive tier storage
						// Exclude: outposts variants, retrieval costs, early delete penalties
						if (UsageType.f_EndsWith(":SnapshotUsage"))
							Result.m_StandardPricePerGBMonth = Price;
						else if (UsageType.f_EndsWith(":SnapshotArchiveStorage"))
							Result.m_ArchivePricePerGBMonth = Price;

						break;
					}
					break;
				}
			}
		}

		co_return Result;
	}

	TCFuture<CAwsPricingActor::CEBSPriceInfo> CAwsPricingActor::f_GetEBSPrices(CStr _Region)
	{
		// Launch both queries in parallel
		auto GP3Future = fp_QueryGP3Prices(_Region);
		auto SnapshotFuture = fp_QuerySnapshotPrices(_Region);

		// Await both results
		CEBSPriceInfo Result;
		CGP3Prices GP3Prices = co_await fg_Move(GP3Future);
		Result.m_GP3PricePerGBMonth = GP3Prices.m_StoragePricePerGBMonth;
		Result.m_GP3IOPSPricePerMonth = GP3Prices.m_IOPSPricePerMonth;
		Result.m_GP3ThroughputPricePerMonth = GP3Prices.m_ThroughputPricePerMBsMonth;

		CSnapshotPrices SnapshotPrices = co_await fg_Move(SnapshotFuture);
		Result.m_SnapshotPricePerGBMonth = SnapshotPrices.m_StandardPricePerGBMonth;
		Result.m_SnapshotArchivePricePerGBMonth = SnapshotPrices.m_ArchivePricePerGBMonth;

		co_return Result;
	}

	TCFuture<CAwsPricingActor::CNATGatewayPrices> CAwsPricingActor::fp_QueryNATGatewayPrices(CStr _RegionCode)
	{
		auto &Internal = *mp_pInternal;

		NHTTP::CURL URL{"https://api.pricing.us-east-1.amazonaws.com/"};

		TCMap<CStr, CStr> AWSHeaders;
		AWSHeaders["Content-Type"] = "application/x-amz-json-1.1";
		AWSHeaders["X-Amz-Target"] = "AWSPriceListService.GetProducts";

		CAwsCredentials Credentials = Internal.m_Credentials;
		Credentials.m_Region = "us-east-1";

		CJsonSorted Request;
		Request["ServiceCode"] = "AmazonEC2";
		Request["FormatVersion"] = "aws_v1";

		auto &Filters = Request["Filters"];

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "regionCode";
			Filter["Value"] = _RegionCode;
		}

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "productFamily";
			Filter["Value"] = "NAT Gateway";
		}

		CJsonSorted Response = co_await fg_DoAWSRequestJson
			(
				"GetProducts (NAT Gateway)"
				, Internal.m_CurlActor
				, 200
				, URL
				, Request
				, CCurlActor::EMethod_POST
				, Credentials
				, AWSHeaders
				, "pricing"
			)
		;

		CNATGatewayPrices Result;

		auto *pPriceList = Response.f_GetMember("PriceList", EJsonType_Array);
		if (pPriceList && !pPriceList->f_Array().f_IsEmpty())
		{
			for (auto const &PriceItem : pPriceList->f_Array())
			{
				if (PriceItem.f_Type() != EJsonType_String)
					continue;

				CJsonSorted PriceJson;
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Error parsing NAT Gateway price JSON");
					PriceJson = CJsonSorted::fs_FromString(PriceItem.f_String());
				}

				// Check the usageType to determine if it's hourly or data processing
				auto *pProduct = PriceJson.f_GetMember("product", EJsonType_Object);
				if (!pProduct)
					continue;

				auto *pAttributes = pProduct->f_GetMember("attributes", EJsonType_Object);
				if (!pAttributes)
					continue;

				auto *pUsageType = pAttributes->f_GetMember("usagetype", EJsonType_String);
				if (!pUsageType)
					continue;

				CStr UsageType = pUsageType->f_String();

				auto *pTerms = PriceJson.f_GetMember("terms", EJsonType_Object);
				if (!pTerms)
					continue;

				auto *pOnDemand = pTerms->f_GetMember("OnDemand", EJsonType_Object);
				if (!pOnDemand)
					continue;

				for (auto const &OnDemandTerm : pOnDemand->f_Object())
				{
					auto *pPriceDimensions = OnDemandTerm.f_Value().f_GetMember("priceDimensions", EJsonType_Object);
					if (!pPriceDimensions)
						continue;

					for (auto const &Dimension : pPriceDimensions->f_Object())
					{
						auto *pPricePerUnit = Dimension.f_Value().f_GetMember("pricePerUnit", EJsonType_Object);
						if (!pPricePerUnit)
							continue;

						auto *pUSD = pPricePerUnit->f_GetMember("USD", EJsonType_String);
						if (!pUSD)
							continue;

						fp64 Price = pUSD->f_String().f_ToFloatExact(fp64(0.0));

						// Regional variants must be checked before non-regional
						// since the latter is a substring of the former
						if (UsageType.f_Find("RegionalNatGateway-Hours") >= 0)
							Result.m_RegionalNATGatewayPricePerHour = Price;
						else if (UsageType.f_Find("RegionalNatGateway-Bytes") >= 0)
							Result.m_RegionalNATGatewayDataProcessedPerGB = Price;
						else if (UsageType.f_Find("NatGateway-Hours") >= 0)
							Result.m_NATGatewayPricePerHour = Price;
						else if (UsageType.f_Find("NatGateway-Bytes") >= 0)
							Result.m_NATGatewayDataProcessedPerGB = Price;

						break;
					}
					break;
				}
			}
		}

		co_return Result;
	}

	TCFuture<fp64> CAwsPricingActor::fp_QueryPublicIPv4Price(CStr _RegionCode)
	{
		auto &Internal = *mp_pInternal;

		NHTTP::CURL URL{"https://api.pricing.us-east-1.amazonaws.com/"};

		TCMap<CStr, CStr> AWSHeaders;
		AWSHeaders["Content-Type"] = "application/x-amz-json-1.1";
		AWSHeaders["X-Amz-Target"] = "AWSPriceListService.GetProducts";

		CAwsCredentials Credentials = Internal.m_Credentials;
		Credentials.m_Region = "us-east-1";

		CJsonSorted Request;
		Request["ServiceCode"] = "AmazonVPC";
		Request["FormatVersion"] = "aws_v1";

		auto &Filters = Request["Filters"];

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "regionCode";
			Filter["Value"] = _RegionCode;
		}

		{
			auto &Filter = Filters.f_Insert();
			Filter["Type"] = "TERM_MATCH";
			Filter["Field"] = "group";
			Filter["Value"] = "VPCPublicIPv4Address";
		}

		CJsonSorted Response = co_await fg_DoAWSRequestJson
			(
				"GetProducts (Public IPv4)"
				, Internal.m_CurlActor
				, 200
				, URL
				, Request
				, CCurlActor::EMethod_POST
				, Credentials
				, AWSHeaders
				, "pricing"
			)
		;

		auto *pPriceList = Response.f_GetMember("PriceList", EJsonType_Array);
		if (pPriceList && !pPriceList->f_Array().f_IsEmpty())
		{
			for (auto const &PriceItem : pPriceList->f_Array())
			{
				if (PriceItem.f_Type() != EJsonType_String)
					continue;

				CJsonSorted PriceJson;
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Error parsing Public IPv4 price JSON");
					PriceJson = CJsonSorted::fs_FromString(PriceItem.f_String());
				}

				// Check for "In-use public IPv4 address" type
				auto *pProduct = PriceJson.f_GetMember("product", EJsonType_Object);
				if (!pProduct)
					continue;

				auto *pAttributes = pProduct->f_GetMember("attributes", EJsonType_Object);
				if (!pAttributes)
					continue;

				auto *pGroupDesc = pAttributes->f_GetMember("groupDescription", EJsonType_String);
				if (!pGroupDesc)
					continue;

				// We want "In-use public IPv4 address"
				if (pGroupDesc->f_String().f_Find("In-use") < 0)
					continue;

				auto *pTerms = PriceJson.f_GetMember("terms", EJsonType_Object);
				if (!pTerms)
					continue;

				auto *pOnDemand = pTerms->f_GetMember("OnDemand", EJsonType_Object);
				if (!pOnDemand)
					continue;

				for (auto const &OnDemandTerm : pOnDemand->f_Object())
				{
					auto *pPriceDimensions = OnDemandTerm.f_Value().f_GetMember("priceDimensions", EJsonType_Object);
					if (!pPriceDimensions)
						continue;

					for (auto const &Dimension : pPriceDimensions->f_Object())
					{
						auto *pPricePerUnit = Dimension.f_Value().f_GetMember("pricePerUnit", EJsonType_Object);
						if (!pPricePerUnit)
							continue;

						auto *pUSD = pPricePerUnit->f_GetMember("USD", EJsonType_String);
						if (!pUSD)
							continue;

						co_return pUSD->f_String().f_ToFloatExact(fp64(0.0));
					}
				}
			}
		}

		// Fallback to known price if API query returned empty
		// AWS charges $0.005/hour for all public IPv4 addresses (since Feb 2024)
		co_return 0.005;
	}

	TCFuture<CAwsPricingActor::CNetworkPriceInfo> CAwsPricingActor::f_GetNetworkPrices(CStr _Region)
	{
		// Launch both queries in parallel
		auto NATFuture = fp_QueryNATGatewayPrices(_Region);
		auto IPv4Future = fp_QueryPublicIPv4Price(_Region);

		// Await and merge results
		auto NATResult = co_await fg_Move(NATFuture);

		CNetworkPriceInfo Result;
		Result.m_NATGatewayPricePerHour = NATResult.m_NATGatewayPricePerHour;
		Result.m_RegionalNATGatewayPricePerHour = NATResult.m_RegionalNATGatewayPricePerHour;
		Result.m_NATGatewayDataProcessedPerGB = NATResult.m_NATGatewayDataProcessedPerGB;
		Result.m_RegionalNATGatewayDataProcessedPerGB = NATResult.m_RegionalNATGatewayDataProcessedPerGB;
		Result.m_PublicIPv4PricePerHour = co_await fg_Move(IPv4Future);

		co_return Result;
	}
}
