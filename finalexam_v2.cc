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
using namespace ns3;

NS_LOG_COMPONENT_DEFINE("CsmaCaNoRtsCtsV2");

// Per-run counters updated by PHY traces.
static uint64_t g_phyRxDrops = 0;

static void
PhyRxDropCb(Ptr<const Packet> /*p*/, WifiPhyRxfailureReason /*r*/)
{
    ++g_phyRxDrops;
}

struct RunResult
{
    double aggregateThroughputMbps = 0.0; // sum at sink
    double avgDelayMs = 0.0;
    double pdr = 0.0;       // 0..1
    double collisions = 0.0; // PHY drops at receivers
};

static RunResult
RunOne(uint32_t nNodes, double simTime, uint32_t payload, uint32_t rngRun)
{
    // Independent RNG stream per repetition.
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(rngRun);

    // Reset trace counter for this run.
    g_phyRxDrops = 0;

    NodeContainer nodes;
    nodes.Create(nNodes);

    // PHY / channel.
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    Ptr<YansWifiChannel> channel = channelHelper.Create();

    YansWifiPhyHelper phy;
    phy.SetChannel(channel);

    // 802.11g, fixed 6 Mbps so contention -- not rate adaptation -- drives results.
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("ErpOfdmRate6Mbps"),
                                 "ControlMode", StringValue("ErpOfdmRate6Mbps"));

    // Ad-hoc MAC, RTS/CTS off (threshold above the max possible PSDU size so
    // RTS/CTS never triggers). WIFI_MAX_RTS_THRESHOLD = 4 692 480 in ns-3.47.
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

    // Internet stack + IPs.
    InternetStackHelper internet;
    internet.Install(nodes);
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifs = ipv4.Assign(devices);

    // Sink on node 0.
    const uint16_t port = 5001;
    Address sinkAddr(InetSocketAddress(ifs.GetAddress(0), port));
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApps = sinkHelper.Install(nodes.Get(0));
    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(simTime + 1.0));

    // Saturating CBR sources on every other node.
    // 2 Mbps per source * (N-1) sources easily exceeds the 6 Mbps PHY for N>=4,
    // so the MAC is in saturation for all N >= 4 and lightly loaded for N=2,3.
    OnOffHelper onoff("ns3::UdpSocketFactory", sinkAddr);
    onoff.SetAttribute("OnTime",
                       StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    onoff.SetAttribute("OffTime",
                       StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    onoff.SetAttribute("DataRate", StringValue("2Mbps"));
    onoff.SetAttribute("PacketSize", UintegerValue(payload));

    ApplicationContainer srcApps;
    for (uint32_t i = 1; i < nNodes; ++i)
    {
        srcApps.Add(onoff.Install(nodes.Get(i)));
    }
    // Stagger starts very slightly so they don't all fire on the exact same tick.
    Ptr<UniformRandomVariable> jitter = CreateObject<UniformRandomVariable>();
    jitter->SetAttribute("Min", DoubleValue(1.0));
    jitter->SetAttribute("Max", DoubleValue(1.05));
    for (uint32_t i = 0; i < srcApps.GetN(); ++i)
    {
        srcApps.Get(i)->SetStartTime(Seconds(jitter->GetValue()));
        srcApps.Get(i)->SetStopTime(Seconds(simTime));
    }

    // PHY RxDrop trace on every device (counts collision-induced drops).
    Config::ConnectWithoutContext(
        "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Phy/PhyRxDrop",
        MakeCallback(&PhyRxDropCb));

    // Flow monitor for throughput / delay / PDR.
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
    r.aggregateThroughputMbps = (dur > 0) ? (rxBytes * 8.0 / dur / 1e6) : 0.0;
    r.avgDelayMs = (rxPackets > 0) ? (delaySumSec / rxPackets) * 1000.0 : 0.0;
    r.pdr = (txPackets > 0) ? static_cast<double>(rxPackets) / txPackets : 0.0;
    r.collisions = static_cast<double>(g_phyRxDrops);

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
    std::string outCsv = "csma_ca_v2.csv";

    CommandLine cmd;
    cmd.AddValue("simTime", "Per-run simulation time (s)", simTime);
    cmd.AddValue("payload", "UDP payload bytes", payload);
    cmd.AddValue("minNodes", "Minimum node count (incl. sink)", minNodes);
    cmd.AddValue("maxNodes", "Maximum node count (incl. sink)", maxNodes);
    cmd.AddValue("repetitions", "Independent RNG runs per node count", repetitions);
    cmd.AddValue("out", "Output CSV path", outCsv);
    cmd.Parse(argc, argv);

    std::ofstream csv(outCsv);
    csv << "Nodes,AggThroughputMbps,AvgDelayMs,PDRPercent,PhyRxDrops\n";

    std::cout << "Ad-hoc 802.11g (6 Mbps), no RTS/CTS, single collision domain\n";
    std::cout << "Sources: N-1 saturating UDP @ 2 Mbps -> sink (node 0)\n";
    std::cout << "Repetitions per point: " << repetitions << "\n";
    std::cout << "----------------------------------------------------------\n";
    std::cout << std::left << std::setw(6) << "N"
              << std::setw(14) << "Thr(Mbps)"
              << std::setw(12) << "Delay(ms)"
              << std::setw(10) << "PDR(%)"
              << std::setw(12) << "Drops"
              << "\n";

    for (uint32_t n = minNodes; n <= maxNodes; ++n)
    {
        RunResult avg{};
        for (uint32_t r = 0; r < repetitions; ++r)
        {
            RunResult one = RunOne(n, simTime, payload, /*rngRun=*/r + 1);
            avg.aggregateThroughputMbps += one.aggregateThroughputMbps;
            avg.avgDelayMs += one.avgDelayMs;
            avg.pdr += one.pdr;
            avg.collisions += one.collisions;
        }
        avg.aggregateThroughputMbps /= repetitions;
        avg.avgDelayMs /= repetitions;
        avg.pdr /= repetitions;
        avg.collisions /= repetitions;

        std::cout << std::left << std::setw(6) << n
                  << std::setw(14) << std::fixed << std::setprecision(3)
                  << avg.aggregateThroughputMbps
                  << std::setw(12) << std::setprecision(3) << avg.avgDelayMs
                  << std::setw(10) << std::setprecision(2) << (avg.pdr * 100.0)
                  << std::setw(12) << std::setprecision(0) << avg.collisions
                  << "\n";

        csv << n << "," << avg.aggregateThroughputMbps << "," << avg.avgDelayMs
            << "," << (avg.pdr * 100.0) << "," << avg.collisions << "\n";
    }

    csv.close();
    return 0;
}
