// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Container/Map>

#include "Malterlib_Web_AWS_Credentials.h"

namespace NMib::NWeb
{
	struct CCurlActor;

	struct CAwsPricingActor : public NConcurrency::CActor
	{
		// EC2 price information
		struct CEC2PriceInfo
		{
			NStr::CStr m_InstanceType;		// e.g., "t3.small"
			fp64 m_OnDemandPricePerHour = 0;	// USD per hour
			NStr::CStr m_OperatingSystem;	// e.g., "Linux"

			// Instance attributes
			NStr::CStr m_ClockSpeed;				// e.g., "3.1 GHz"
			NStr::CStr m_CurrentGeneration;			// "Yes" or "No"
			NStr::CStr m_DedicatedEbsThroughput;	// e.g., "6000 Mbps"
			NStr::CStr m_Ecu;						// e.g., "168"
			NStr::CStr m_EnhancedNetworkingSupported;	// "Yes" or "No"
			NStr::CStr m_Memory;					// e.g., "192 GiB"
			NStr::CStr m_NetworkPerformance;		// e.g., "10 Gigabit"
			NStr::CStr m_NormalizationSizeFactor;	// e.g., "96"
			NStr::CStr m_PhysicalProcessor;			// e.g., "Intel Xeon Platinum 8175"
			NStr::CStr m_ProcessorArchitecture;		// e.g., "64-bit"
			NStr::CStr m_ProcessorFeatures;			// e.g., "Intel AVX; Intel AVX2; ..."
			NStr::CStr m_Storage;					// e.g., "2 x 900 NVMe SSD"
			NStr::CStr m_Tenancy;					// e.g., "Shared"
			NStr::CStr m_Vcpu;						// e.g., "48"

			// Reserved Instance pricing (key: "{Term}|{OfferingClass}|{Payment}")
			// e.g., "1yr|standard|No Upfront" -> 0.015
			NContainer::TCMap<NStr::CStr, fp64> m_ReservedPrices;
		};

		// EBS price information
		struct CEBSPriceInfo
		{
			fp64 m_GP3PricePerGBMonth = 0;		// gp3 storage cost per GB per month
			fp64 m_GP3IOPSPricePerMonth = 0;	// gp3 IOPS cost per IOPS per month (above 3000 baseline)
			fp64 m_GP3ThroughputPricePerMonth = 0;	// gp3 throughput cost per MB/s per month (above 125 baseline)
			fp64 m_SnapshotPricePerGBMonth = 0;	// EBS snapshot cost per GB per month (standard tier)
			fp64 m_SnapshotArchivePricePerGBMonth = 0;	// EBS snapshot cost per GB per month (archive tier)
		};

		// Network/VPC price information
		struct CNetworkPriceInfo
		{
			fp64 m_NATGatewayPricePerHour = 0;					// NAT Gateway per hour (per-AZ type)
			fp64 m_RegionalNATGatewayPricePerHour = 0;			// Regional NAT Gateway per hour (per active AZ)
			fp64 m_NATGatewayDataProcessedPerGB = 0;			// NAT Gateway data processing per GB
			fp64 m_RegionalNATGatewayDataProcessedPerGB = 0;	// Regional NAT Gateway data processing per GB
			fp64 m_PublicIPv4PricePerHour = 0;					// Public IPv4 address per hour
		};

		// Get EC2 on-demand instance prices for all instance types in a region
		// Note: This fetches Linux/Unix prices with pagination support
		NConcurrency::TCFuture<NContainer::TCVector<CEC2PriceInfo>> f_GetEC2Prices(NStr::CStr _Region);

		// Get EBS storage prices for a region
		NConcurrency::TCFuture<CEBSPriceInfo> f_GetEBSPrices(NStr::CStr _Region);

		// Get network/VPC related prices for a region
		NConcurrency::TCFuture<CNetworkPriceInfo> f_GetNetworkPrices(NStr::CStr _Region);

		CAwsPricingActor(NConcurrency::TCActor<CCurlActor> const &_CurlActor, CAwsCredentials const &_Credentials);
		~CAwsPricingActor();

	private:
		struct CInternal;

		// NAT Gateway price result (internal)
		struct CNATGatewayPrices
		{
			fp64 m_NATGatewayPricePerHour = 0;
			fp64 m_RegionalNATGatewayPricePerHour = 0;
			fp64 m_NATGatewayDataProcessedPerGB = 0;
			fp64 m_RegionalNATGatewayDataProcessedPerGB = 0;
		};

		// Snapshot price result (internal)
		struct CSnapshotPrices
		{
			fp64 m_StandardPricePerGBMonth = 0;
			fp64 m_ArchivePricePerGBMonth = 0;
		};

		// GP3 price result (internal)
		struct CGP3Prices
		{
			fp64 m_StoragePricePerGBMonth = 0;
			fp64 m_IOPSPricePerMonth = 0;
			fp64 m_ThroughputPricePerMBsMonth = 0;
		};

		// EBS price helpers
		NConcurrency::TCFuture<CGP3Prices> fp_QueryGP3Prices(NStr::CStr _LocationName);
		NConcurrency::TCFuture<CSnapshotPrices> fp_QuerySnapshotPrices(NStr::CStr _LocationName);

		// Network price helpers
		NConcurrency::TCFuture<CNATGatewayPrices> fp_QueryNATGatewayPrices(NStr::CStr _LocationName);
		NConcurrency::TCFuture<fp64> fp_QueryPublicIPv4Price(NStr::CStr _LocationName);

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NWeb;
#endif
