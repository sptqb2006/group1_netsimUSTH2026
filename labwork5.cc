#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/yans-wifi-helper.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
using namespace ns3;

NS_LOG_COMPONENT_DEFINE("CsmaCaNoRtsCtsV3");

// Per-run counters updated by trace callbacks.
static uint64_t g_phyRxDrops = 0;
static uint64_t g_macTxFailed = 0;       // fires on every ACK-timeout retry
static uint64_t g_macTxFinalFailed = 0;  // fires when retry budget exhausted

static void
PhyRxDropCb(Ptr<const Packet> /*p*/, WifiPhyRxfailureReason /*r*/)
{
    ++g_phyRxDrops;
}

static void
MacTxFailedCb(Mac48Address /*addr*/)
{
    ++g_macTxFailed;
}

static void
MacTxFinalFailedCb(Mac48Address /*addr*/)
{
    ++g_macTxFinalFailed;
}

struct RunResult
{
    double offeredLoadMbps = 0.0;
    double aggregateThroughputMbps = 0.0;
    double avgDelayMs = 0.0;
    double pdr = 0.0;          // 0..1
    double phyDrops = 0.0;     // PHY collisions at receivers
    double macRetries = 0.0;   // total ACK-timeout retries at senders
    double macFinalDrops = 0.0; // frames dropped after retry budget
};

static RunResult
RunOne(uint32_t nNodes, double simTime, uint32_t payload,
       double perSrcKbps, uint32_t rngRun)
{
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(rngRun);

    g_phyRxDrops = 0;
    g_macTxFailed = 0;
    g_macTxFinalFailed = 0;

    NodeContainer nodes;
    nodes.Create(nNodes);

    // Default Yans channel + PHY.
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    Ptr<YansWifiChannel> channel = channelHelper.Create();

    YansWifiPhyHelper phy;
    phy.SetChannel(channel);

    // 802.11g, fixed 6 Mbps -- isolates contention from rate adaptation.
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("ErpOfdmRate6Mbps"),
                                 "ControlMode", StringValue("ErpOfdmRate6Mbps"));

    // Ad-hoc MAC; RTS/CTS disabled (threshold above max PSDU).
    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold",
                       UintegerValue(4692480));

    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    // Topology: sink at origin, sources on a 1 m circle around it.
    // 1 m << carrier-sense range -> guaranteed single collision domain.
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0)); // sink = node 0
    const double radius = 1.0;
    for (uint32_t i = 1; i < nNodes; ++i)
    {
        double theta = 2.0 * M_PI * (i - 1) / static_cast<double>(nNodes - 1);
        pos->Add(Vector(radius * std::cos(theta), radius * std::sin(theta), 0.0));
    }
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    InternetStackHelper internet;
    internet.Install(nodes);
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifs = ipv4.Assign(devices);

    // UDP sink at node 0.
    const uint16_t port = 5001;
    Address sinkAddr(InetSocketAddress(ifs.GetAddress(0), port));
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApps = sinkHelper.Install(nodes.Get(0));
    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(simTime + 1.0));

    // Low-rate CBR sources: offered = (N-1) * perSrcKbps stays well below the
    // ~4.6 Mbps saturation throughput for every N <= 30 when perSrcKbps <= ~150.
    std::ostringstream rateStr;
    rateStr << static_cast<uint64_t>(perSrcKbps * 1000.0) << "bps";

    OnOffHelper onoff("ns3::UdpSocketFactory", sinkAddr);
    onoff.SetAttribute("OnTime",
                       StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    onoff.SetAttribute("OffTime",
                       StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    onoff.SetAttribute("DataRate", StringValue(rateStr.str()));
    onoff.SetAttribute("PacketSize", UintegerValue(payload));

    ApplicationContainer srcApps;
    for (uint32_t i = 1; i < nNodes; ++i)
    {
        srcApps.Add(onoff.Install(nodes.Get(i)));
    }
    Ptr<UniformRandomVariable> jitter = CreateObject<UniformRandomVariable>();
    jitter->SetAttribute("Min", DoubleValue(1.0));
    jitter->SetAttribute("Max", DoubleValue(1.05));
    for (uint32_t i = 0; i < srcApps.GetN(); ++i)
    {
        srcApps.Get(i)->SetStartTime(Seconds(jitter->GetValue()));
        srcApps.Get(i)->SetStopTime(Seconds(simTime));
    }

    // PHY collision drops (frame destroyed at receiver).
    Config::ConnectWithoutContext(
        "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/PhyRxDrop",
        MakeCallback(&PhyRxDropCb));
    // MAC-level retry counter (ACK timer expired at sender). The trace lives
    // on the RemoteStationManager, not directly on the Mac.
    Config::ConnectWithoutContext(
        "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/"
        "MacTxDataFailed",
        MakeCallback(&MacTxFailedCb));
    // Final drop after retry budget exhausted.
    Config::ConnectWithoutContext(
        "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/"
        "MacTxFinalDataFailed",
        MakeCallback(&MacTxFinalFailedCb));

    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> fm = fmHelper.InstallAll();

    Simulator::Stop(Seconds(simTime + 1.0));
    Simulator::Run();

    fm->CheckForLostPackets();
    auto stats = fm->GetFlowStats();

    uint64_t txPackets = 0;
    uint64_t rxPackets = 0;
    uint64_t rxBytes = 0;
    double delaySumSec = 0.0;
    double firstTx = simTime;
    double lastRx = 0.0;

    for (const auto& kv : stats)
    {
        txPackets += kv.second.txPackets;
        rxPackets += kv.second.rxPackets;
        rxBytes += kv.second.rxBytes;
        delaySumSec += kv.second.delaySum.GetSeconds();
        if (kv.second.rxPackets > 0)
        {
            firstTx = std::min(firstTx, kv.second.timeFirstTxPacket.GetSeconds());
            lastRx = std::max(lastRx, kv.second.timeLastRxPacket.GetSeconds());
        }
    }

    RunResult r;
    double dur = lastRx - firstTx;
    r.offeredLoadMbps = (nNodes - 1) * perSrcKbps / 1000.0;
    r.aggregateThroughputMbps = (dur > 0) ? (rxBytes * 8.0 / dur / 1e6) : 0.0;
    r.avgDelayMs = (rxPackets > 0) ? (delaySumSec / rxPackets) * 1000.0 : 0.0;
    r.pdr = (txPackets > 0) ? static_cast<double>(rxPackets) / txPackets : 0.0;
    r.phyDrops = static_cast<double>(g_phyRxDrops);
    r.macRetries = static_cast<double>(g_macTxFailed);
    r.macFinalDrops = static_cast<double>(g_macTxFinalFailed);

    Simulator::Destroy();
    return r;
}

int
main(int argc, char* argv[])
{
    double simTime = 10.0;
    uint32_t payload = 1024;
    uint32_t minNodes = 2;
    uint32_t maxNodes = 30;
    uint32_t repetitions = 3;
    // 100 kbps per source -> at N=30, offered = 2.9 Mbps << ~4.6 Mbps saturation.
    double perSrcKbps = 100.0;
    std::string outCsv = "labwork5.csv";

    CommandLine cmd;
    cmd.AddValue("simTime", "Per-run simulation time (s)", simTime);
    cmd.AddValue("payload", "UDP payload bytes", payload);
    cmd.AddValue("minNodes", "Minimum node count (incl. sink)", minNodes);
    cmd.AddValue("maxNodes", "Maximum node count (incl. sink)", maxNodes);
    cmd.AddValue("repetitions", "Independent RNG runs per node count", repetitions);
    cmd.AddValue("perSrcKbps",
                 "Per-source CBR rate (kbps). Keep low to stay under-saturated.",
                 perSrcKbps);
    cmd.AddValue("out", "Output CSV path", outCsv);
    cmd.Parse(argc, argv);

    std::ofstream csv(outCsv);
    csv << "Nodes,OfferedMbps,ThroughputMbps,AvgDelayMs,PDRPercent,"
        << "PhyRxDrops,MacRetries,MacFinalDrops\n";

    for (uint32_t n = minNodes; n <= maxNodes; ++n)
    {
        RunResult avg{};
        for (uint32_t r = 0; r < repetitions; ++r)
        {
            RunResult one = RunOne(n, simTime, payload, perSrcKbps, r + 1);
            avg.offeredLoadMbps = one.offeredLoadMbps;
            avg.aggregateThroughputMbps += one.aggregateThroughputMbps;
            avg.avgDelayMs += one.avgDelayMs;
            avg.pdr += one.pdr;
            avg.phyDrops += one.phyDrops;
            avg.macRetries += one.macRetries;
            avg.macFinalDrops += one.macFinalDrops;
        }
        avg.aggregateThroughputMbps /= repetitions;
        avg.avgDelayMs /= repetitions;
        avg.pdr /= repetitions;
        avg.phyDrops /= repetitions;
        avg.macRetries /= repetitions;
        avg.macFinalDrops /= repetitions;

        std::cout << n << " nodes, average traffic "
                  << std::fixed << std::setprecision(3)
                  << avg.offeredLoadMbps << " Mbps\n";
        std::cout << "    Aggregate Throughput at Sink         : "
                  << std::setprecision(4)
                  << avg.aggregateThroughputMbps << " Mbps\n";
        std::cout << "    Average End-to-End Delay             : "
                  << std::setprecision(3)
                  << avg.avgDelayMs              << " milliseconds\n";
        std::cout << "    Packet Delivery Ratio                : "
                  << std::setprecision(2)
                  << (avg.pdr * 100.0)           << " %\n";
        std::cout << "    PHY Receive Drops (collisions)       : "
                  << std::setprecision(2)
                  << avg.phyDrops                << " frames\n";
        std::cout << "    MAC Transmit Data Failures (retries) : "
                  << avg.macRetries              << " events\n";
        std::cout << "    MAC Final Data Failures (final drops): "
                  << avg.macFinalDrops           << " frames\n\n";

        csv << n << "," << avg.offeredLoadMbps << "," << avg.aggregateThroughputMbps
            << "," << avg.avgDelayMs << "," << (avg.pdr * 100.0)
            << "," << avg.phyDrops << "," << avg.macRetries
            << "," << avg.macFinalDrops << "\n";
    }

    csv.close();
    return 0;
}
