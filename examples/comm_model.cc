#include "comm_model.h"
#include "ns3/constant-position-mobility-model.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/aodv-module.h"
#include "ns3/nstime.h"
#include "ns3/ssid.h"
#include <random>
// #include "ns3/sta-wifi-mac.h"

NS_LOG_COMPONENT_DEFINE ("ROSNS3Model");

CoModel::CoModel(std::vector<mobile_node_t> mobile_nodes, int sim_time, bool use_real_time):
                mobile_nodes(mobile_nodes),sim_time(sim_time), use_real_time(use_real_time) {
    pcap = true;
    print_routes = false;
    n_nodes = mobile_nodes.size();

    if (use_real_time) {
        GlobalValue::Bind ("SimulatorImplementationType", StringValue (
                            "ns3::RealtimeSimulatorImpl"));
    }
    // init roi_params
    Vector2d roi_means(0,0);
    Vector2d roi_vars(5,15);

    std::vector<mobile_node_t> ue_nodes = sample_ue(roi_means, roi_vars);
    unsigned int n_ues = ue_nodes.size();
    create_sta_nodes(ue_nodes);
    NS_LOG_INFO("Created sta nodes");

    create_backbone_nodes();
    create_mobility_model();
    create_backbone_devices();
    install_inet_stack();
    install_applications(n_ues);
};

std::vector<mobile_node_t> CoModel::sample_ue(Vector2d roi_means, Vector2d roi_vars) {
    int n_ues = 5;
    std::vector<mobile_node_t> ue_nodes;
    std::random_device rd{};
    std::mt19937 gen{rd()};
    std::normal_distribution<> d1{roi_means(0),roi_vars(0)};
    std::normal_distribution<> d2{roi_means(1),roi_vars(1)};

    for(int i=0; i<n_ues; i++) {
        Vector pose(d1(gen),d1(gen), 0);
        mobile_node_t ue_node = {.position=pose, .id=i};

        ue_nodes.push_back(ue_node);   
    }

    return ue_nodes;
}

void CoModel::run() {
    auto simulator_t = [this]() {
        Simulator::Stop (Seconds (sim_time));        
        Simulator::Run ();
        NS_LOG_DEBUG("Finished simulation.");
        report(std::cout);
        // Simulator::Destroy();
    };
    if(use_real_time) {
        NS_LOG_DEBUG("Simulation started in a new thread.");
        this->simulator = new std::thread(simulator_t);
    }
    else {
        NS_LOG_DEBUG("Simulation started.");
        simulator_t();
    }
};

void CoModel::update_mobility_model(std::vector<mobile_node_t> mobile_nodes) {
    this->mobile_nodes = mobile_nodes;
    for (uint32_t i = 0; i<n_nodes;i++) {
        Ptr<Node> node = backbone.Get (i);
        Ptr<MobilityModel> mob = node->GetObject<MobilityModel> ();
        mob->SetPosition(mobile_nodes[i].position);
    }
    NS_LOG_DEBUG("Simulating with the updated the mobility model.");
    
    if (!use_real_time) {
        run();
    }
};

void CoModel::create_mobility_model() {
    // add constant mobility model
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
    for (uint32_t i = 0;i<n_nodes;i++) {
        mobile_node_t mobile_node = mobile_nodes[i];
        positionAlloc->Add(mobile_node.position);
    }
    mobility.SetPositionAllocator(positionAlloc);
    mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobility.Install (backbone);
    NS_LOG_DEBUG("Created " << n_nodes << " nodes.");
};

void CoModel::report(std::ostream &) {};

void CoModel::create_backbone_nodes() {
    backbone.Create(n_nodes);
    for (uint32_t i = 0;i<n_nodes;i++) {
        std::ostringstream os;
        os << "node-" << i+1;
        Names::Add (os.str (), backbone.Get (i));
    }
};

// create the sta nodes and the ap nodes
void CoModel::create_sta_nodes(std::vector<mobile_node_t> ue_nodes) {
    stas.Create(ue_nodes.size());
    WifiHelper wifi_infra;

    YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default ();
    YansWifiPhyHelper wifiPhy = YansWifiPhyHelper::Default ();
    wifiPhy.SetChannel (wifiChannel.Create ());
    Ssid ssid = Ssid ("wifi-infra");
    wifi_infra.SetRemoteStationManager ("ns3::ArfWifiManager");
    WifiMacHelper mac_infra;

    // setup stas
    mac_infra.SetType("ns3::StaWifiMac", "Ssid", SsidValue (ssid));
    NetDeviceContainer sta_devices = wifi_infra.Install (wifiPhy, mac_infra, stas);

    // setup aps
    NodeContainer aps;
    for (uint32_t i=0; i<n_nodes; i++) {
        aps.Add(backbone);
    }
    mac_infra.SetType("ns3::ApWifiMac", "Ssid", SsidValue (ssid));
    NetDeviceContainer ap_devices = wifi_infra.Install(wifiPhy, mac_infra, aps);
    NetDeviceContainer infraDevices (ap_devices, sta_devices);
    NS_LOG_INFO("setup infra devices");

    // add ipv4 to infra nodes
    InternetStackHelper stack;
    Ipv4AddressHelper address;
    address.SetBase ("10.0.0.0", "255.255.255.0");
    stack.Install(stas);
    interfaces_stas = address.Assign(infraDevices);
    address.NewNetwork();
    NS_LOG_INFO("here 1");

    // constant mobility for sta nodes
    MobilityHelper mobility_sta;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
    for (unsigned int i = 0;i<ue_nodes.size();i++) {
        mobile_node_t ue_node = ue_nodes[i];
        positionAlloc->Add(ue_node.position);
    }
    NS_LOG_INFO("Setting STA mobility model");

    mobility_sta.SetPositionAllocator(positionAlloc);
    mobility_sta.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobility_sta.Install(stas);
    NS_LOG_DEBUG("Created STA and AP nodes.");

}

void CoModel::create_backbone_devices() {
    WifiMacHelper wifiMac;
    wifiMac.SetType ("ns3::AdhocWifiMac");
    YansWifiPhyHelper wifiPhy = YansWifiPhyHelper::Default ();
    YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default ();
    wifiPhy.SetChannel (wifiChannel.Create ());
    WifiHelper wifi;

    /**
     * https://www.nsnam.org/docs/release/3.21/doxygen/classns3_1_1_constant_rate_wifi_manager.html
     * 
     * ConstantRateWifiManager: This class uses always the same transmission rate for 
     * every packet sent. 
     * DataMode: The transmission mode to use for every data packet transmission 
     * (Initial value: OfdmRate6Mbps)
     * RtsCtsThreshold: If the size of the data packet + LLC header + MAC header + FCS 
     * trailer is bigger than this value, we use an RTS/CTS handshake before sending the 
     * data, as per IEEE Std. 802.11-2012, Section 9.3.5. This value will not have any 
     * ffect on some rate control algorithms.
     * 
     * **/
    wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager", "DataMode", 
                    StringValue ("OfdmRate54Mbps"), "RtsCtsThreshold", UintegerValue (0));
    devices = wifi.Install (wifiPhy, wifiMac, backbone); 

    if (pcap)
    {
        NS_LOG_DEBUG("Enabled packet capturing.");
        wifiPhy.EnablePcapAll (std::string ("rosns3"));
    }
    NS_LOG_DEBUG("Installed wifi devices.");

};

// install applications on sta nodes
void CoModel::install_applications(unsigned int n_ues) {
    // address of the last sta
    V4PingHelper ping (interfaces_stas.GetAddress (n_ues - 1));
    ping.SetAttribute ("Verbose", BooleanValue (true));
    const Time t = Seconds(1);
    ping.SetAttribute ("Interval", TimeValue(t));

    NS_LOG_DEBUG("here 1");

    // install ping app on 1st sta
    ApplicationContainer apps;
    // for(uint32_t i = 0; i<n_ues; i++ ) {
    apps.Add(ping.Install (stas.Get (0)));

    apps.Start (Seconds (1));
    apps.Stop (Seconds (sim_time) - Seconds (0.5));

    // p1.Start (Seconds (0));
    // p1.Stop (Seconds (sim_time) - Seconds (0.001));
    NS_LOG_DEBUG("Installed ping applications.");

};

void CoModel::install_inet_stack() {
    AodvHelper aodv;
    // you can configure AODV attributes here using aodv.Set(name, value)
    InternetStackHelper stack;
    stack.SetRoutingHelper (aodv); // has effect on the next Install ()
    stack.Install (backbone);
    Ipv4AddressHelper address;
    address.SetBase ("172.16.0.0", "255.255.255.0");
    interfaces = address.Assign (devices);

    if (print_routes)
    {
        Ptr<OutputStreamWrapper> routingStream = Create<OutputStreamWrapper> ("aodv.routes", std::ios::out);
        aodv.PrintRoutingTableAllAt (Seconds (sim_time-1), routingStream);
    }
    NS_LOG_DEBUG("Installed AODV internet stack.");

};

