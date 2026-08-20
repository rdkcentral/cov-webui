/* (c) 2021 Netflix, Inc. Do not copy or use without prior written permission from Netflix, Inc. */

/* Note that this file mostly resembles InterfaceNetworkLinux.cpp -
   it's so any updates to original file can be located more easily. */
#include "InterfaceNetwork.h"
#include "DevicePropertyProvider/DevicePropertiesProvider.h"
#include <algorithm>
#include <arpa/inet.h>
#include <asm/types.h>
#include <assert.h>
#define _BSD_SOURCE 1 /* for d_type */
#include <DpiLog.h>
#include <IpAddress.h>
#include <StringTools.h>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <float.h>
#include <fstream>
#include <link.h>
#include <linux/limits.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <linux/wireless.h>
#include <locale>
#include <mutex>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <nrddpi/InterfaceNetwork.h>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include "wirelesstools.h"
#include "cJSON.h"
#include "RDKDpiConfiguration.h"

#ifdef NRDP_HAS_ETHTOOL
#include <linux/ethtool.h>
#endif

#ifdef __ANDROID__
#include "ifaddrs.c"
#else
#include <ifaddrs.h>
#endif

#ifdef NRDP_HAS_NETLINK
#include "nl80211.h"
#include <netlink/attr.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#endif

struct nl_msg;
struct nl_sock;
struct nl_cb;

#define INETWORK_INVALID_STAT64 UINT64_MAX
#define INETWORK_INVALID_STAT32 UINT32_MAX
#define INETWORK_INVALID_STATDBL DBL_MAX

using namespace partner::device::thunderlink;

namespace partner
{
namespace device
{

struct DpiNetworkInterface
{
    DpiNetworkInterface()
        : physicalLayerType(NF_PHYSICAL_LAYER_TYPE_UNKNOWN)
        , physicalLayerSubtype(NF_PHYSICAL_LAYER_SUBTYPE_UNKNOWN)
        , isDefault(false)
        , linkConnected(NF_CONNECTION_STATE_UNKNOWN)
        , internetConnected(NF_CONNECTION_STATE_UNKNOWN)
    {
    }

    std::string name;                             // Interface name (ex: "eth0")
    NF_PhysicalLayerType physicalLayerType;       // e.g. mobile, gsm, wired, etc.
    NF_PhysicalLayerSubtype physicalLayerSubtype; // Device supported subtype.
                                                  // Ex: Device supports 1000M Ethernet but
                                                  // router is 10M, this reports 1000M.

    IpAddress ipAddress;                    // current IPv4 address
    std::vector<std::string> ipv6Addresses; // list of current IPv6
                                            // addresses

    std::string ipPrefix;                  // IPv4 mask in form of prefix
    std::vector<std::string> ipv6Prefixes; // IPv6 prefixes for all current IPv6 addresses

    DpiVariant macAddress; // Hardware address for WiFi or wired interfaces
    DpiVariant ssid;       // if connected via wifi, the wireless SSID

    // Set these fields if NF_PhysicalLayerType is: NET_TYPE_MOBILE, NET_TYPE_GSM, or NET_TYPE_CDMA.
    DpiVariant mobileCarrier;     // Unique mobile carrier name.
    DpiVariant mobileCountryCode; // Mobile country code.
    DpiVariant mobileNetworkCode; // Mobile network code.

    bool isDefault;                       // true if this interface is used to send
                                          // to the default gateway
    NF_ConnectionState linkConnected;     // able to communicate with wireless
                                          // access point for wireless, or if
                                          // ethernet is connected
    NF_ConnectionState internetConnected; // able to communicate with
                                          // internet endpoint
    // Driver information
    DpiVariant driverName;            // Driver name (ex: rt73usb)
    DpiVariant driverVersion;         // Driver version (ex: 4.4.0-64-generic)
    DpiVariant driverFirmwareVersion; // Firmware version (ex: 1.7)

    // Wireless only
    DpiVariant wirelessProtocol; // protocol (ex: "IEEE 802.11bgn")

    DpiVariant additionalInfo; // Additional device-dependent info that
                               // should be logged.  The string may
                               // contain JSON-formatted network setting
                               // info, for example.
};

struct DpiNetworkInterfaceStatistics
{
    // Wired and wireless
    DpiVariant txPackets;       // [STATISTIC]
    DpiVariant txError;         // [STATISTIC]
    DpiVariant txDropped;       // [STATISTIC]
    DpiVariant txFifoErrors;    // [STATISTIC]
    DpiVariant txCarrierErrors; // [STATISTIC]

    DpiVariant rxPackets;     // [STATISTIC]
    DpiVariant rxError;       // [STATISTIC]
    DpiVariant rxDropped;     // [STATISTIC]
    DpiVariant rxFifoErrors;  // [STATISTIC]
    DpiVariant rxFrameErrors; // [STATISTIC]

    // Link information
    DpiVariant linkTxBitrate; // Current transmit link bit-rate in Mbit/s (ex1: "6.5" ex2: "1000")
                              // Ex: Device supports 1000M Ethernet but
                              // router is 10M, this reports 10M. [STATISTIC]
    DpiVariant linkRxBitrate; // Current receive link bit-rate in Mbit/s (ex1: "6.5" ex2: "1000")
                              // Ex: Device supports 1000M Ethernet but
                              // router is 10M, this reports 10M. [STATISTIC]
    // Wireless only
    DpiVariant wirelessFrequency;          // units in MHz (ex: 2462)
    DpiVariant wirelessQuality;            // quality in arbitrary units (ex1: 90/100  ex2: 45) [STATISTIC]
    DpiVariant wirelessSignal;             // units in dBm or arbitrary units (ex1: -40 dBm  ex2: 90/100  ex3: 45) [STATISTIC]
    DpiVariant wirelessApAddress;          // MAC address of access point (ex: 01:23:45:67:89:AB)
    DpiVariant wirelessInactiveTime;       // Time since last activity (milliseconds) [STATISTIC]
    DpiVariant wirelessRxBytes;            // Total received bytes (MPDU length) from this station [STATISTIC]
    DpiVariant wirelessRxPackets;          // Total received packet (MSDUs and MMPDUs) from this station [STATISTIC]
    DpiVariant wirelessRxDropped;          // Total dropped packet (MSDUs and MMPDUs) from this station [STATISTIC]
    DpiVariant wirelessTxBytes;            // Total transmitted bytes (MPDU length) [STATISTIC]
    DpiVariant wirelessTxPackets;          // Total transmitted packets (MSDUs and MMPDUs) to this station [STATISTIC]
    DpiVariant wirelessTxRetries;          // Total retries (MPDUs) to this station [STATISTIC]
    DpiVariant wirelessTxFailed;           // Total failed packets (MPDUs) to this station [STATISTIC]
    DpiVariant wirelessExpectedThroughput; // Expected throughput in Mbit/s [STATISTIC]
};

class InterfaceNetwork
{
public:
    InterfaceNetwork();

    std::vector<std::string> getDNSList();
    std::vector<std::string> getCachedDNSList();
    std::vector<DpiNetworkInterface> getNetworkInterfaces();
    std::vector<DpiNetworkInterface> getCachedNetworkInterfaces();
#ifdef SUPPORT_CONTAINER_NETWORK
    void getContainerNetworkInfo();
#endif

    NF_IPVersion getIpConnectivityMode() { return NF_IP_DUAL; }

    NF_ResultCode getNetworkStatistics(const char *ifaceName, DpiNetworkInterfaceStatistics &stats);

private:
    void setIpConnectivityMode(NF_IPVersion);
    void DevicePropertiesNetworkCallback(const DevicePropertiesProvider::DevicePropertiesProviderNetworkEventData &data);
    void getIPForDpiInterface(DpiNetworkInterface &dpi);
    // NOT related to netlink interface - fills network link info (link rate, etc)
    void fillNetlinkInfo(DpiNetworkInterface &dpi);

private:
#ifdef NRDP_HAS_NETLINK
    typedef int (*NETLINK_CALLBACK)(struct nl_msg *, void *);

    struct netLinkContext
    {
        netLinkContext()
            : nls(0)
            , nl80211_id(0)
            , message(0)
            , callback(0)
        {
        }
        struct nl_sock *nls;
        int nl80211_id;
        DpiVariant callbackInactiveTime;
        DpiVariant callbackRxBytes;
        DpiVariant callbackRxPackets;
        DpiVariant callbackRxDropped;
        DpiVariant callbackTxBytes;
        DpiVariant callbackTxPackets;
        DpiVariant callbackTxRetries;
        DpiVariant callbackTxFailed;
        DpiVariant callbackTxBitrate;
        DpiVariant callbackRxBitrate;
        DpiVariant callbackExpectedThroughput;
        std::map<std::string, uint32_t> nameMap;
        struct nl_msg *message;
        struct nl_cb *callback;
    };

    bool netLinkInit(netLinkContext &, NETLINK_CALLBACK);
    void netLinkFini(netLinkContext &);
    static int netlinkInterfaceCallback(struct nl_msg *msg, void *arg);
    static int netlinkStationCallback(struct nl_msg *msg, void *arg);
#endif

private:
    NF_IPVersion mIpConnectivityMode;
    bool mSuppressNetlinkError;
    std::mutex mNetlinkMutex;
    std::vector<DpiNetworkInterface> mNif;
    std::vector<std::string> mDNSList;
};

}
} // namespace partner::device

using namespace partner;
using namespace partner::device;

static std::string translateIfname(const std::string orgname)
{
    if (orgname == "ETHERNET")
        return "eth2";
    else if (orgname == "WIFI")
        return "wlan2";
    else if (orgname == "eth2")
        return "ETHERNET";
    else if (orgname == "wlan2")
        return "WIFI";
    return orgname;
}

#ifdef NRDP_HAS_TRACING
static std::string networkInterfacesToString(const std::vector<DpiNetworkInterface> &interfaces)
{
    std::ostringstream str;
    for (std::vector<DpiNetworkInterface>::const_iterator it = interfaces.begin(); it != interfaces.end(); ++it)
    {
        if (it != interfaces.begin())
            str << '\n';
        str << "Name: " << it->name << " PType: " << it->physicalLayerType << " PSubType: " << it->physicalLayerSubtype
            << " IP: " << it->ipAddress.toString() << " IPV6: " << StringTools::join(it->ipv6Addresses, ", ") << " HWaddr: " << it->macAddress
            << " SSID: " << it->ssid << " Carrier: " << it->mobileCarrier << " ("
            << it->mobileCountryCode << ", " << it->mobileNetworkCode << ")"
            << " Link: " << it->linkConnected << " Internet: " << it->internetConnected;
        if (!it->additionalInfo.isNull())
            str << " Additional: " << it->additionalInfo;
        if (it->isDefault)
            str << " (Default)";
    }
    return str.str();
}
#endif

static int procnetdevVersion(const std::string &s)
{
    if (s.find("compressed") != std::string::npos)
    {
        return 3;
    }

    if (s.find("bytes") != std::string::npos)
    {
        return 2;
    }

    return 1;
}

struct procnetdevStruct
{
    unsigned long long rx_packets; /* total packets received       */
    unsigned long long tx_packets; /* total packets transmitted    */
    unsigned long long rx_bytes;   /* total bytes received         */
    unsigned long long tx_bytes;   /* total bytes transmitted      */
    unsigned long rx_errors;       /* bad packets received         */
    unsigned long tx_errors;       /* packet transmit problems     */
    unsigned long rx_dropped;      /* no space in linux buffers    */
    unsigned long tx_dropped;      /* no space available in linux  */
    unsigned long rx_multicast;    /* multicast packets received   */
    unsigned long rx_compressed;
    unsigned long tx_compressed;
    unsigned long collisions;

    /* detailed rx_errors: */
    unsigned long rx_length_errors;
    unsigned long rx_over_errors;   /* receiver ring buff overflow  */
    unsigned long rx_crc_errors;    /* recved pkt with crc error    */
    unsigned long rx_frame_errors;  /* recv'd frame alignment error */
    unsigned long rx_fifo_errors;   /* recv'r fifo overrun          */
    unsigned long rx_missed_errors; /* receiver missed packet     */
    /* detailed tx_errors */
    unsigned long tx_aborted_errors;
    unsigned long tx_carrier_errors;
    unsigned long tx_fifo_errors;
    unsigned long tx_heartbeat_errors;
    unsigned long tx_window_errors;
};

static bool getNetworkInterfaceInformation(const std::string &ifname, procnetdevStruct &stats)
{
    std::string deviceList = "/proc/net/dev";
    std::ifstream file(deviceList.c_str());
    std::string line;

    std::getline(file, line); // Eat the first line
    std::getline(file, line);

    int ver = procnetdevVersion(line);

    while (std::getline(file, line))
    {
        const std::string searchString = ifname + std::string(":");
        size_t found = line.find(searchString);
        if (found == std::string::npos)
        {
            continue;
        }

        const char *a = line.c_str() + searchString.size() + found;

        switch (ver)
        {
        case 3:
            sscanf(a,
                   "%llu %llu %lu %lu %lu %lu %lu %lu %llu %llu %lu %lu %lu %lu %lu %lu",
                   &stats.rx_bytes,
                   &stats.rx_packets,
                   &stats.rx_errors,
                   &stats.rx_dropped,
                   &stats.rx_fifo_errors,
                   &stats.rx_frame_errors,
                   &stats.rx_compressed,
                   &stats.rx_multicast,

                   &stats.tx_bytes,
                   &stats.tx_packets,
                   &stats.tx_errors,
                   &stats.tx_dropped,
                   &stats.tx_fifo_errors,
                   &stats.collisions,
                   &stats.tx_carrier_errors,
                   &stats.tx_compressed);
            break;
        case 2:
            sscanf(a, "%llu %llu %lu %lu %lu %lu %llu %llu %lu %lu %lu %lu %lu",
                   &stats.rx_bytes,
                   &stats.rx_packets,
                   &stats.rx_errors,
                   &stats.rx_dropped,
                   &stats.rx_fifo_errors,
                   &stats.rx_frame_errors,

                   &stats.tx_bytes,
                   &stats.tx_packets,
                   &stats.tx_errors,
                   &stats.tx_dropped,
                   &stats.tx_fifo_errors,
                   &stats.collisions,
                   &stats.tx_carrier_errors);
            break;
        case 1:
            sscanf(a, "%llu %lu %lu %lu %lu %llu %lu %lu %lu %lu %lu",
                   &stats.rx_packets,
                   &stats.rx_errors,
                   &stats.rx_dropped,
                   &stats.rx_fifo_errors,
                   &stats.rx_frame_errors,

                   &stats.tx_packets,
                   &stats.tx_errors,
                   &stats.tx_dropped,
                   &stats.tx_fifo_errors,
                   &stats.collisions,
                   &stats.tx_carrier_errors);
            break;
        default:
            return false;
        }
        return true;
    }

    return false;
}

#ifdef NRDP_HAS_ETHTOOL
// Should be in ethtool.h
#define ETHTOOL_LINK_MODE_TP_BIT 7
#define ETHTOOL_LINK_MODE_10baseT_Half_BIT 0
#define ETHTOOL_LINK_MODE_10baseT_Full_BIT 1
#define ETHTOOL_LINK_MODE_100baseT_Half_BIT 2
#define ETHTOOL_LINK_MODE_100baseT_Full_BIT 3
#define ETHTOOL_LINK_MODE_1000baseT_Half_BIT 4
#define ETHTOOL_LINK_MODE_1000baseT_Full_BIT 5

#define TEST_BIT(bit, data) (!!((1 << bit) & data))

static bool getLinkControlAndStatus(int sock, ifreq &ifr, bool &twistedPair, uint32_t &maxSpeed, uint32_t &linkSpeed)
{
    struct ethtool_cmd ecmd;
    memset(&ecmd, 0, sizeof(ecmd));
    ecmd.cmd = ETHTOOL_GSET;

    ifr.ifr_data = (caddr_t)&ecmd;
    if (ioctl(sock, SIOCETHTOOL, &ifr) < 0)
    {
        return false;
    }

    const uint32_t supported = ecmd.supported;
    const uint32_t speedInMbps = ethtool_cmd_speed(&ecmd);

    if (TEST_BIT(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, supported) ||
        TEST_BIT(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, supported))
    {
        maxSpeed = 1000;
    }
    else if (TEST_BIT(ETHTOOL_LINK_MODE_100baseT_Half_BIT, supported) ||
             TEST_BIT(ETHTOOL_LINK_MODE_100baseT_Full_BIT, supported))
    {
        maxSpeed = 100;
    }
    else if (TEST_BIT(ETHTOOL_LINK_MODE_10baseT_Half_BIT, supported) ||
             TEST_BIT(ETHTOOL_LINK_MODE_10baseT_Full_BIT, supported))
    {
        maxSpeed = 10;
    }
    else
    {
        maxSpeed = 0;
    }

    twistedPair = TEST_BIT(ETHTOOL_LINK_MODE_TP_BIT, supported);
    linkSpeed = (speedInMbps > maxSpeed) ? 0 : speedInMbps;

    return true;
}

static bool getDriverInformation(int sock, ifreq &ifr, DpiVariant &name, DpiVariant &version, DpiVariant &fwversion)
{
    struct ethtool_drvinfo info;
    memset(&info, 0, sizeof(info));
    info.cmd = ETHTOOL_GDRVINFO;

    ifr.ifr_data = (caddr_t)&info;
    if (ioctl(sock, SIOCETHTOOL, &ifr) < 0)
    {
        name = DpiVariant();
        version = DpiVariant();
        fwversion = DpiVariant();
        return false;
    }

    name = info.driver;
    version = info.version;
    fwversion = info.fw_version;
    return true;
}
#else
static bool getLinkControlAndStatus(int, ifreq &, bool &, uint32_t &, uint32_t &)
{
    NWARN("Network", "%s: no ethtool", __FUNC__);
    return false;
}

static bool getDriverInformation(int, ifreq &, DpiVariant &, DpiVariant &, DpiVariant &)
{
    NWARN("Network", "%s: no ethtool", __FUNC__);
    return false;
}
#endif

static std::string macAddressToString(const std::vector<uint8_t> &mac)
{
    char ret[64];
    snprintf(ret, sizeof(ret), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ret;
}

#ifdef NRDP_HAS_NETLINK
static std::vector<uint8_t> stringToMacAddress(const std::string &s)
{
    std::vector<uint8_t> ret(6);
    std::vector<uint32_t> m(6);
    sscanf(s.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
    std::copy(m.begin(), m.end(), ret.begin());
    return ret;
}
#endif

static bool getMACAddress(int sock, ifreq &ifr, std::vector<uint8_t> &mac, sa_family_t &family)
{
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0)
    {
        return false;
    }

    family = ifr.ifr_hwaddr.sa_family;

    mac.resize(6);
    memcpy(&mac[0], ifr.ifr_hwaddr.sa_data, 6);
    return true;
}

static bool getAPMACAddress(int sock, ifreq &ifr, std::vector<uint8_t> &mac)
{
    if (ioctl(sock, SIOCGIWAP, &ifr) < 0)
    {
        return false;
    }

    mac.resize(6);
    memcpy(&mac[0], ifr.ifr_hwaddr.sa_data, 6);
    return true;
}

static bool validMACAddress(const std::vector<uint8_t> &mac)
{
    if ((mac[0] == 0) && (mac[1] == 0) && (mac[2] == 0) && (mac[3] == 0) && (mac[4] == 0) && (mac[5] == 0))
    {
        return false;
    }

    if ((mac[0] == 0xff) && (mac[1] == 0xff) && (mac[2] == 0xff) && (mac[3] == 0xff) && (mac[4] == 0xff) && (mac[5] == 0xff))
    {
        return false;
    }

    return true;
}

static inline unsigned long readHexNumber(char *&ch, bool *ok)
{
    while (*ch && isspace(static_cast<unsigned char>(*ch)))
    {
        ++ch;
    }
    char *end = 0;
    const unsigned long ret = strtoull(ch, &end, 16);
    if (!isspace(static_cast<unsigned char>(*end)))
    {
        if (ok)
            *ok = false;
        return 0;
    }
    else if (ok)
    {
        *ok = true;
    }
    ch = end;
    return ret;
}

enum Route
{
    None,
    Valid,
    Gateway
};

static inline Route route(const std::string &nf, NF_IPVersion version)
{
    assert(version == NF_IP_V4 || version == NF_IP_V6);
    Route ret = None;
    if (version == NF_IP_V4)
    {
        const char *fileName = "/proc/net/route";
        if (FILE *fp = fopen(fileName, "r"))
        {
            char line[1024];
            int nfBegin = 0, nfEnd = 0;
            unsigned int gateway = 0, flags = 0;
            while (!feof(fp))
            {
                if (!fgets(line, sizeof(line), fp))
                    break;
                const int r = sscanf(line, "%n%*s%n %*s %x %x %*s %*s %*s %*s %*s %*s %*s",
                                     &nfBegin, &nfEnd, &gateway, &flags);
                if (r == 2 && nfBegin == 0 && nfEnd == static_cast<int>(nf.size()) && !strncmp(nf.c_str(), line, nfEnd))
                {
                    if ((flags & 0x3) == 0x3 || gateway)
                    {
                        ret = Gateway;
                        break;
                    }
                    else if (flags != 0)
                    {
                        ret = Valid;
                    }
                }
            }
            fclose(fp);
        }
    }
    else
    {
        const char *fileName = "/proc/net/ipv6_route";
        if (FILE *fp = fopen(fileName, "r"))
        {
            char line[1024];
            int nfBegin = 0, nfEnd = 0, nexthopStart = 0, nexthopEnd = 0;
            unsigned int flags = 0;
            while (!feof(fp))
            {
                if (!fgets(line, sizeof(line), fp))
                    break;
                const int r = sscanf(line, "%*s %*s %*s %*s %n%*s%n %*s %*s %*s %x %n%*s%n",
                                     &nexthopStart, &nexthopEnd, &flags, &nfBegin, &nfEnd);
                if (r == 1 && nfEnd - nfBegin == static_cast<int>(nf.size()) && !strncmp(line + nfBegin, nf.c_str(), nf.size()))
                {
                    if ((flags & 0x3) == 0x3)
                    {
                        ret = Gateway;
                        break;
                    }
                    for (int i = nexthopStart; i < nexthopEnd; ++i)
                    {
                        // coverity[tainted_data]
                        if (line[i] != '0')
                        {
                            ret = Gateway;
                            break;
                        }
                    }
                    if (ret == Gateway)
                        break;
                    if (flags & 1)
                        ret = Valid;
                }
            }
            fclose(fp);
        }
    }
    return ret;
}

/*   Report if the network interface is a default Route or not*/
static bool isDefault(const std::string &nf, NF_IPVersion version)
{
    return route(nf, version) == Gateway;
}

// Helper function to default Route checking
static bool isValidRoute(const std::string &nf, NF_IPVersion version)
{
    return route(nf, version) != None;
}

/*   Report if the network interface is a default Route or not*/
static bool isWifi(const std::string &nf)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "/sys/class/net/%s/wireless", nf.c_str());
    struct stat st;
    return !stat(buf, &st) && S_ISDIR(st.st_mode);
}

/* Fetch the Wireless network interface ESSID */
struct WIFIINFO
{
    WIFIINFO()
        : layer(NF_PHYSICAL_LAYER_TYPE_UNKNOWN)
        , slayer(NF_PHYSICAL_LAYER_SUBTYPE_UNKNOWN)
    {
    }

    DpiVariant protocol;
    DpiVariant ssid;
    enum NF_PhysicalLayerType layer;
    enum NF_PhysicalLayerSubtype slayer;
    DpiVariant bitrate;    // units in Mbps
    DpiVariant frequency;  // units in MHz
    DpiVariant quality;    // Arbitrary
    DpiVariant signal;     // units in dBm or arbitrary units
    DpiVariant noise;      // units in dBm or arbitrary units
    DpiVariant throughput; // Expected throughput in Mbit/s
};

static void getWifiInfo(const std::string &ifname, WIFIINFO &info, int socket)
{
    struct iw_range range;
    struct iw_statistics stats;
    if (ifname.size() + 1 < IFNAMSIZ)
    {
        // TODO - refactore to restore/add missing functionality planned
        // Get Layer type
        // struct iwreq pwrq;
        // memset(&pwrq, 0, sizeof(pwrq));
        // strncpy(pwrq.ifr_name, ifname.c_str(), IFNAMSIZ);
        // if (ioctl(socket, SIOCGIWNAME, &pwrq) != -1)
        if (translateIfname(ifname) == "WIFI")
        {
            // char b[IFNAMSIZ + 1];
            // strncpy(&b[0], pwrq.u.name, IFNAMSIZ + 1);
            // b[IFNAMSIZ] = 0;
            // info.protocol = std::string(b);
            info.protocol = "IEEE 802.11a";
            info.layer = NF_PHYSICAL_LAYER_TYPE_WIFI;
        }
        else
        {
            // not a wifi interface. return out.
            info.layer = NF_PHYSICAL_LAYER_TYPE_WIRED;
            return;
        }

        struct iwreq wreq;
        memset(&wreq, 0, sizeof(struct iwreq));
        strncpy(wreq.ifr_name, ifname.c_str(), IFNAMSIZ - 1); // Set interface name
        wreq.ifr_name[IFNAMSIZ - 1] = '\0';

        char buffer[32];
        memset(buffer, 0, 32);
        wreq.u.essid.pointer = buffer;
        wreq.u.essid.length = 32;
        // Get SSID
        if (ioctl(socket, SIOCGIWESSID, &wreq) == -1)
        {
            info.ssid = DpiVariant();
        }
        else
        {
            info.ssid = std::string((char *)wreq.u.essid.pointer);
        }

        // Get frequency and other statistics
        if (nflx_wifi_get_range_info(socket, ifname.c_str(), &range) < 0)
        {
            info.frequency = DpiVariant();
            info.quality = DpiVariant();
            info.signal = DpiVariant();
            info.noise = DpiVariant();

            std::optional<DevicePropertiesProvider::SsidParams> params = DevicePropertiesProvider::getInstance().getConnectedSSIDInfo();
            if (params)
                info.signal = (*params).signalStrength;
        }
        else
        {
            if (nflx_wifi_get_ext(socket, ifname.c_str(), SIOCGIWFREQ, &wreq) >= 0)
            {
                const double freq = nflx_wifi_freq2float(&(wreq.u.freq));
                info.frequency = static_cast<double>(freq / 1000000);
            }

            // quality, signal and noise levels
            if (nflx_wifi_get_stats(socket, ifname.c_str(), ifname.size(), &stats, &range, 1) >= 0)
            {
                nflx_digest_stats(&stats.qual, &range, true, info.quality, info.signal, info.noise, info.throughput);
            }
        }

        // Assume the device strings are of this general format:
        // "IEEE 802.11" followed by the non-case sensitive version
        // IEEE 802.11abgn  ex: n
        // IEEE 802.11AC    ex: ac
        // IEEE 802.11bgn   ex: n
        // IEEE 802.11bg    ex: g

        info.slayer = NF_PHYSICAL_LAYER_SUBTYPE_UNKNOWN;

        static const std::string IEEE_STRING("IEEE 802.11");
        std::string data = info.protocol.string();
        std::string::size_type found = data.find(IEEE_STRING);
        std::transform(data.begin(), data.end(), data.begin(), ::tolower);

        while ((found == 0) && (data.size() > IEEE_STRING.size()))
        {

            if (data.find("ac", IEEE_STRING.size()) != std::string::npos)
            {
                info.slayer = NF_PHYSICAL_LAYER_SUBTYPE_802_11AC;
                break;
            }

            if (data.find("n", IEEE_STRING.size()) != std::string::npos)
            {
                info.slayer = NF_PHYSICAL_LAYER_SUBTYPE_802_11N;
                break;
            }

            if (data.find("g", IEEE_STRING.size()) != std::string::npos)
            {
                info.slayer = NF_PHYSICAL_LAYER_SUBTYPE_802_11G;
                break;
            }

            if (data.find("b", IEEE_STRING.size()) != std::string::npos)
            {
                info.slayer = NF_PHYSICAL_LAYER_SUBTYPE_802_11B;
                break;
            }

            if (data.find("a", IEEE_STRING.size()) != std::string::npos)
            {
                info.slayer = NF_PHYSICAL_LAYER_SUBTYPE_802_11A;
                break;
            }

            break;
        }
    }
}

static void getWifiInfo(const std::string &ifname, NF_IPVersion version, WIFIINFO &info, int socket)
{
    if (!isValidRoute(ifname, version))
    {
        // Doing this to workaround some HW issue on certain Wireless adapter,
        // link is down but report its connected
        info.ssid = DpiVariant();
    }
    getWifiInfo(ifname, info, socket);
}

InterfaceNetwork::InterfaceNetwork()
{
    DevicePropertiesProvider::getInstance().registerNetworkCallback(
        [this](const DevicePropertiesProvider::DevicePropertiesProviderNetworkEventData &data)
        {
            DevicePropertiesNetworkCallback(data);
        });
}

/* looks like WPEFramework::JSONRPC used by ThunderLink is unable to proceed
 * method call done from event callback context - we need some mechanism to
 * cope with that. Moving it out of the context by calling from other thread
 * should do the trick
 */
void triggerAsyncFreshPushNetworkChangedEvent()
{
    static std::thread t;
    if (t.joinable())
        t.join();

    t = std::thread([]()
                    { TheNetworkAdapter::pushNetworkChangedEvent(false, true); });
}

void InterfaceNetwork::DevicePropertiesNetworkCallback(const DevicePropertiesProvider::DevicePropertiesProviderNetworkEventData &data)
{
    const DeviceProperties::DevicePropertiesEvent ev = data.event;
    const string status = data.status;
    const string interface = translateIfname(data.interface);

    switch (ev)
    {
    case DeviceProperties::DevicePropertiesEvent::NetworkConnectionStatusChanged:
    {
        if (interface.empty())
        {
            // Firebolt network status event currently does not include interface details.
            TheNetworkAdapter::pushNetworkChangedEvent(true, true);
            break;
        }

        // update network adaper status in cached list
        for (auto &it : mNif)
        {
            if (it.name == interface)
            {
                int oldlink = (int)it.linkConnected;
                if (status == "DISCONNECTED")
                {
                    it.linkConnected = NF_CONNECTION_STATE_DISCONNECTED;
                    it.internetConnected = NF_CONNECTION_STATE_DISCONNECTED;
                }
                else if (status == "CONNECTED")
                {
                    it.linkConnected = NF_CONNECTION_STATE_CONNECTED;
                    if (it.internetConnected == NF_CONNECTION_STATE_DISCONNECTED)
                        // just because cable is plugged in, it doesm't mean we got inet connection
                        it.internetConnected = NF_CONNECTION_STATE_UNKNOWN;
                }
                NINFO("Network", "updated status of %s from %d to %d. internet: %d", it.name.c_str(), oldlink, (int)it.linkConnected, (int)it.internetConnected);
                if (oldlink == (int)it.linkConnected)
                    return;
                break;
            }
        }
        TheNetworkAdapter::pushNetworkChangedEvent(true, true);
        break;
    }
    case DeviceProperties::DevicePropertiesEvent::NetworkIpAddressChanged:
    {
        const string ipv4Address = data.ipv4Address;
        const string ipv6Address = data.ipv6Address;
        bool ipPresent = status == "ACQUIRED";
        // ignore events other than lost and acquired ip addresses
        if (!ipPresent && (status != "LOST"))
            return;

        bool newConnection = false;
        for (auto &it : mNif)
        {
            if (it.name == interface)
            {
                if (ipPresent)
                {
                    if (!ipv4Address.empty())
                    {
                        it.ipAddress = IpAddress(ipv4Address);
                        it.internetConnected = NF_CONNECTION_STATE_CONNECTED;
                        newConnection = true;
                    }
                    if (!ipv6Address.empty())
                    {
                        it.ipv6Addresses.clear();
                        it.ipv6Addresses.push_back(ipv6Address);
                        it.internetConnected = NF_CONNECTION_STATE_CONNECTED;
                        newConnection = true;
                    }
                }
                else
                {
                    it.ipAddress = IpAddress();
                    it.ipv6Addresses.clear();
                    it.internetConnected = NF_CONNECTION_STATE_DISCONNECTED;
                }
                NINFO("Network", "updated ipaddr of %s to %s / %s. internet: %d", it.name.c_str(), ipv4Address.c_str(), ipv6Address.c_str(), (int)it.internetConnected);
                break;
            }
        }
        if (newConnection)
            /* some configs that could be changed but are not part of event (eg DNS)
             * has to be refreshed, so trigger event with fetching configs and
             * as it is inside event callback fitting method have to be used
             */
            triggerAsyncFreshPushNetworkChangedEvent();
        else
            TheNetworkAdapter::pushNetworkChangedEvent(true, false);

        break;
    }
    default:
        NWARN("Network", "DevicePropertiesNetworkCallback: unknown event %d", static_cast<int>(ev));
    }
}

std::vector<std::string> InterfaceNetwork::getCachedDNSList()
{
    return mDNSList;
}

void InterfaceNetwork::getIPForDpiInterface(DpiNetworkInterface &dpi)
{
    JsonObject ip, params;
    struct in_addr net_addr;
    int prefix = 0;

    params["interface"] = translateIfname(dpi.name);
    params["ipversion"] = "IPv4";
    ip = DevicePropertiesProvider::getInstance().getIPSettings(params);
    if (ip["success"].Boolean())
    {
        dpi.ipAddress = IpAddress(ip["ipaddr"].String());
        std::string net_mask = ip["netmask"].String();
        inet_pton(AF_INET, net_mask.c_str(), &net_addr);

        unsigned int net_addr_int = net_addr.s_addr;

        while (net_addr_int > 0) {
            net_addr_int = net_addr_int >> 1;
            prefix++;
        }

        if (prefix > 0)
            dpi.ipPrefix = std::to_string(prefix);

        std::vector<string> dns = StringTools::split(ip["primarydns"].String(), ',');
        if (!dns.empty())
        {
            for (auto &it : dns)
            {
                if (std::find(begin(mDNSList), end(mDNSList), it) == std::end(mDNSList))
                    mDNSList.push_back(it);
            }
        }
        std::vector<string> dns_sec = StringTools::split(ip["secondarydns"].String(), ',');
        if (!dns_sec.empty())
        {
            for (auto &it : dns_sec)
            {
                if (std::find(begin(mDNSList), end(mDNSList), it) == std::end(mDNSList))
                    mDNSList.push_back(it);
            }
        }
    }
    else
    {
        NERROR("Network", "Failed to get IPv4 data for interface %s", dpi.name.c_str());
    }

    //TODO : Add IPv6 address

    params["interface"] = translateIfname(dpi.name);
    params["ipversion"] = "IPv6";
    ip = DevicePropertiesProvider::getInstance().getIPSettings(params);
    if (ip["success"].Boolean())
    {
        dpi.ipv6Addresses.push_back(ip["ipaddr"].String());
        dpi.ipv6Prefixes.push_back(ip["prefix_length"].String());

        std::vector<string> dns = StringTools::split(ip["primarydns"].String(), ',');
        if (!dns.empty())
        {
            for (auto &it : dns)
            {
                if (std::find(begin(mDNSList), end(mDNSList), it) == std::end(mDNSList))
                    mDNSList.push_back(it);
            }
        }
    }
    else
    {
        NERROR("Network", "Failed to get IPv6 data for interface %s", dpi.name.c_str());
    }
}

static bool APMACAddressExists(int sock, ifreq &ifr, sa_family_t &family)
{
    // set the mac addr family
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0)
    {
        return false;
    }

    family = ifr.ifr_hwaddr.sa_family;

    // get the AP macaddr
    if (ioctl(sock, SIOCGIWAP, &ifr) < 0)
    {
        return false;
    }

    uint8_t mac[6] = {0};

    memcpy(&mac[0], ifr.ifr_hwaddr.sa_data, 6);

    // check whether AP macaddr is valid
    if ((mac[0] == 0) && (mac[1] == 0) && (mac[2] == 0) && (mac[3] == 0) && (mac[4] == 0) && (mac[5] == 0))
    {
        return false;
    }

    if ((mac[0] == 0xff) && (mac[1] == 0xff) && (mac[2] == 0xff) && (mac[3] == 0xff) && (mac[4] == 0xff) && (mac[5] == 0xff))
    {
        return false;
    }

    return true;
}

void InterfaceNetwork::fillNetlinkInfo(DpiNetworkInterface &dpi)
{
    size_t i = 0;
    struct ifreq ifr;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock != -1)
    {
        memcpy(ifr.ifr_name, dpi.name.c_str(), dpi.name.size() + 1);
        ifr.ifr_addr.sa_family = AF_INET;
    }

    getDriverInformation(sock, ifr, dpi.driverName, dpi.driverVersion, dpi.driverFirmwareVersion);
    NDBG("Network", "fni: %s driverName: %s version: %s fwversion: %s", ifr.ifr_name,
         dpi.driverName.string().c_str(),
         dpi.driverVersion.string().c_str(),
         dpi.driverFirmwareVersion.string().c_str());

    sa_family_t family = ARPHRD_VOID;
    const bool gotDeviceMac = dpi.macAddress != "";
    const bool gotApMac = APMACAddressExists(sock, ifr, family);
    NDBG("Network", "gotAPMac = %d", (int)gotApMac);

    WIFIINFO info;
    memset(&info, 0, sizeof(info));
    getWifiInfo(dpi.name, info, sock);
    dpi.ssid = gotApMac ? info.ssid : DpiVariant();
    dpi.physicalLayerSubtype = info.slayer;
    dpi.physicalLayerType = info.layer; // ((dpi.ssid != DpiVariant()) || isWifi(dpi.name)) ? NF_PHYSICAL_LAYER_TYPE_WIFI : NF_PHYSICAL_LAYER_TYPE_WIRED;
    dpi.wirelessProtocol = info.protocol;
    NDBG("Network", "ssid = %s, slayer = %d, layer = %d, protocol = %s", info.ssid.string().c_str(), info.slayer, dpi.physicalLayerType, info.protocol.string().c_str());

    // This only will work with Ethernet (and not Wifi).
    uint32_t maxLinkSpeedInMbps = 0;
    uint32_t linkSpeedInMbps = 0;
    (void)linkSpeedInMbps; // unused in that case
    bool twistedPair = false;
    bool gotSpeed = getLinkControlAndStatus(sock, ifr, twistedPair, maxLinkSpeedInMbps, linkSpeedInMbps);

    if ((family == ARPHRD_ETHER) && twistedPair && gotSpeed)
    {
        switch (maxLinkSpeedInMbps)
        {
        case 10000:
            dpi.physicalLayerSubtype = NF_PHYSICAL_LAYER_SUBTYPE_10000MBPS_ETH;
            break;
        case 1000:
            dpi.physicalLayerSubtype = NF_PHYSICAL_LAYER_SUBTYPE_1000MBPS_ETH;
            break;
        case 100:
            dpi.physicalLayerSubtype = NF_PHYSICAL_LAYER_SUBTYPE_100MBPS_ETH;
            break;
        case 10:
            dpi.physicalLayerSubtype = NF_PHYSICAL_LAYER_SUBTYPE_10MBPS_ETH;
            break;
        default:
            dpi.physicalLayerSubtype = NF_PHYSICAL_LAYER_SUBTYPE_UNKNOWN;
            break;
        }
    }

    if (sock != -1)
    {
        ::close(sock);
    }
}

/*  Check if  network interface has been physically enabled or not */
static NF_ConnectionState isLinkConnected(const std::string &nf)
{
    char uri[1024];
    snprintf(uri, sizeof(uri) - 1, "/sys/class/net/%s/carrier", nf.c_str());
    if (FILE *f = fopen(uri, "r")) {
        char link = '0';
        const int r = fread(&link, sizeof(link), 1, f);
        fclose(f);
        if (r == 1 && link == '1') {
            // Check if the interface is indeed up
            snprintf(uri, sizeof(uri) - 1, "/sys/class/net/%s/operstate", nf.c_str());
            if (FILE *f1 = fopen(uri, "r")) {
                char tmp[2] = {0};
                const int r1 = fread(tmp, 1, 2, f1);
                (void)r1;
                fclose(f1);
                if (tmp[0] == 'u' && tmp[1] == 'p'){
                    return NF_CONNECTION_STATE_CONNECTED;
                }
            }
        }
    }
    return NF_CONNECTION_STATE_DISCONNECTED;
}

/*  Check if network interface has good internet connection.
 *  Applicable to only devices with Connectivity manager middleware e.g. Android, iOS */
static NF_ConnectionState isInternetConnected(const std::string &nf)
{
    static_cast<void>(nf);
    return NF_CONNECTION_STATE_UNKNOWN;
}

#ifdef SUPPORT_CONTAINER_NETWORK
void InterfaceNetwork::getContainerNetworkInfo()
{
    struct IFace
    {
        IpAddress ipv4Address;
        std::vector<std::string> ipv6Addresses;
    };

    ifaddrs *addrs;
    getifaddrs(&addrs);

    std::map<std::string, std::vector<std::string> > ipv6Addresses;
    std::map<std::string, IFace> interfaces;

    for (ifaddrs *iface = addrs; iface; iface = iface->ifa_next) {
        if (iface->ifa_addr) {
            const std::string name = iface->ifa_name;
            IFace &ref = interfaces[name];
            if (iface->ifa_addr->sa_family == AF_INET) {
                if (ref.ipv6Addresses.empty())
                    ref.ipv4Address = IpAddress::fromSockaddr(*reinterpret_cast<const sockaddr_in*>(iface->ifa_addr));
            }
            //TODO : Add ipv6 address
            else if (iface->ifa_addr->sa_family == AF_INET6 && ref.ipv6Addresses.empty()) {
                    const std::map<std::string, std::vector<std::string> >::iterator it = ipv6Addresses.find(name);
                    if (it != ipv6Addresses.end())
                        std::swap(ref.ipv6Addresses, it->second);
           }
        }
    }

    for (std::map<std::string, IFace>::iterator it = interfaces.begin(); it != interfaces.end(); ++it) {
        DpiNetworkInterface dpiIface;
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        int sock = -1;
        if (it->first.size() < sizeof(ifr.ifr_name)) {
            sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock != -1) {
                assert(it->first.size() + 1 <= sizeof(ifr.ifr_name));
                memcpy(ifr.ifr_name, it->first.c_str(), it->first.size() + 1);
                ifr.ifr_addr.sa_family = AF_INET;
            }
        }
        sa_family_t family = ARPHRD_VOID;
        std::vector<uint8_t> deviceMac(6,0);

        const bool gotDeviceMac = getMACAddress(sock, ifr, deviceMac, family);
        dpiIface.name = it->first;

        std::swap(dpiIface.ipv6Addresses, it->second.ipv6Addresses);
        std::swap(dpiIface.ipAddress, it->second.ipv4Address);
        dpiIface.macAddress = gotDeviceMac?macAddressToString(deviceMac):DpiVariant();
        dpiIface.linkConnected = isLinkConnected(it->first);
        dpiIface.internetConnected = isInternetConnected(it->first);
        /* Host interface is considering as default interface */
        const NF_IPVersion version = !dpiIface.ipAddress.isValid() && !dpiIface.ipv6Addresses.empty() ? NF_IP_V6 : NF_IP_V4;
        dpiIface.isDefault = isDefault(it->first, version);

        fillNetlinkInfo(dpiIface);

        mNif.push_back(dpiIface);
    }
}
#endif

std::vector<DpiNetworkInterface> InterfaceNetwork::getNetworkInterfaces()
{
    mNif.clear();
    mDNSList.clear();

#ifdef SUPPORT_CONTAINER_NETWORK
    getContainerNetworkInfo();
#endif

    std::string defaultIface = translateIfname(DevicePropertiesProvider::getInstance().getDefaultNetworkInterface());
    if ("" == defaultIface)
        NERROR("Network", "Thunderlink returned empty default interface");

    JsonObject ifaces = DevicePropertiesProvider::getInstance().getNetworkInterfaces();
    if (ifaces["success"].Boolean())
    {
        JsonArray ifsArray = ifaces["interfaces"].Array();
        if (0 == ifsArray.Length())
        {
            NERROR("Network", "Thunderlink returned empty interface list");
        }
        else
        {
            NF_ConnectionState internetConnected = DevicePropertiesProvider::getInstance().isInternetConnected() ? NF_CONNECTION_STATE_CONNECTED : NF_CONNECTION_STATE_DISCONNECTED;
            JsonArray::Iterator index(ifsArray.Elements());

            while (index.Next() == true)
            {
                JsonObject iface = index.Current().Object();
                DpiNetworkInterface dpiIface;

                dpiIface.name = translateIfname(iface["interface"].String());
                dpiIface.macAddress = iface["macAddress"].String();
                dpiIface.linkConnected = (iface["enabled"].Boolean() && (dpiIface.name == defaultIface)) ? NF_CONNECTION_STATE_CONNECTED : NF_CONNECTION_STATE_DISCONNECTED;
                dpiIface.internetConnected = (iface["connected"].Boolean() && (dpiIface.name == defaultIface)) ? NF_CONNECTION_STATE_CONNECTED : NF_CONNECTION_STATE_DISCONNECTED;
                dpiIface.isDefault = dpiIface.name == defaultIface;

                getIPForDpiInterface(dpiIface);
                fillNetlinkInfo(dpiIface);

                NINFO("Network", "iface %s mac %s link %d inet %d ipv4 %s isDefault %d ssid %s drivername %s driverfw %s",
                                dpiIface.name.c_str(),
                                dpiIface.macAddress.c_str(),
                                (int)dpiIface.linkConnected,
                                (int)dpiIface.internetConnected,
                                dpiIface.ipAddress.toString().c_str(),
                                (int)dpiIface.isDefault,
                                dpiIface.ssid.c_str(),
                                dpiIface.driverName.c_str(),
                                dpiIface.driverFirmwareVersion.c_str());

                 mNif.push_back(dpiIface);
            }
        }
    }
    else
    {
    NERROR("Network", "Failed to get network iterfaces via Thunderlink: %s", ifaces["success"].String().c_str());
    }

    return mNif;
}

std::vector<DpiNetworkInterface> InterfaceNetwork::getCachedNetworkInterfaces()
{
    return mNif;
}

NF_ResultCode InterfaceNetwork::getNetworkStatistics(const char *ifaceName, DpiNetworkInterfaceStatistics &stats)
{
    NF_ResultCode rc = NF_RC_SUCCESS;
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    int sock = -1;
    if (strlen(ifaceName) < sizeof(ifr.ifr_name))
    {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock != -1)
        {
            assert(strlen(ifaceName) + 1 <= sizeof(ifr.ifr_name));
            memcpy(ifr.ifr_name, ifaceName, strlen(ifaceName) + 1);
            ifr.ifr_addr.sa_family = AF_INET;
        }
    }

    std::vector<uint8_t> apMac(6, 0);
    const bool gotApMac = getAPMACAddress(sock, ifr, apMac);
    const bool validApMac = validMACAddress(apMac);

    WIFIINFO info;
    std::string ifname(ifaceName);
    getWifiInfo(ifname, info, sock);
    stats.wirelessFrequency = static_cast<double>(info.frequency.dbl());
    stats.wirelessQuality = static_cast<double>(info.quality.dbl());
    stats.wirelessSignal = static_cast<double>(info.signal.dbl());
    stats.wirelessExpectedThroughput = static_cast<double>(info.throughput.dbl());
    stats.wirelessApAddress = (gotApMac && validApMac) ? macAddressToString(apMac) : DpiVariant();

    // This only will work with Ethernet (and not Wifi).
    uint32_t maxLinkSpeedInMbps = 0;
    uint32_t linkSpeedInMbps = 0;
    (void)maxLinkSpeedInMbps; // unused in that case
    bool twistedPair = false;
    bool gotSpeed = getLinkControlAndStatus(sock, ifr, twistedPair, maxLinkSpeedInMbps, linkSpeedInMbps);
    sa_family_t family = ARPHRD_VOID;

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) >= 0)
    {
        family = ifr.ifr_hwaddr.sa_family;
    }
    if ((family == ARPHRD_ETHER) && twistedPair && gotSpeed)
    {
        stats.linkTxBitrate = linkSpeedInMbps;
        stats.linkRxBitrate = linkSpeedInMbps;
    }

    procnetdevStruct s;
    if (getNetworkInterfaceInformation(ifname, s))
    {
        stats.txPackets = s.tx_packets;
        stats.txError = s.tx_errors;
        stats.txDropped = s.tx_dropped;
        stats.txFifoErrors = s.tx_fifo_errors;
        stats.txCarrierErrors = s.tx_carrier_errors;
        stats.rxPackets = s.rx_packets;
        stats.rxError = s.rx_errors;
        stats.rxDropped = s.rx_dropped;
        stats.rxFifoErrors = s.rx_fifo_errors;
        stats.rxFrameErrors = s.rx_frame_errors;
    }

    if (sock != -1)
    {
        ::close(sock);
    }

#ifdef NRDP_HAS_NETLINK
    if (Configuration::netlinkEnabled())
    {
        // Only reliable way to get all callbacks is to teardown and bring up each time
        std::map<std::string, uint32_t> nameMap;

        netLinkContext nl_ctx;

        ScopedMutex sm(&mNetlinkMutex);

        if (netLinkInit(nl_ctx, netlinkInterfaceCallback))
        {
            genlmsg_put(nl_ctx.message, NL_AUTO_PORT, NL_AUTO_SEQ, nl_ctx.nl80211_id, 0, NLM_F_DUMP, NL80211_CMD_GET_INTERFACE, 0);
            nl_send_auto(nl_ctx.nls, nl_ctx.message);
            nl_recvmsgs(nl_ctx.nls, nl_ctx.callback);
            nameMap = nl_ctx.nameMap;
        }
        netLinkFini(nl_ctx);

        // It's possible Wireless Extensions supports a device but nl80211/cfg80211 doesn't.
        if (nameMap.find(ifname) != nameMap.end())
        {
            const uint32_t ifIndex = nameMap[ifname];
            std::vector<uint8_t> apAddress = stringToMacAddress(stats.wirelessApAddress.string());
            if (netLinkInit(nl_ctx, netlinkStationCallback))
            {
                genlmsg_put(nl_ctx.message, NL_AUTO_PORT, NL_AUTO_SEQ, nl_ctx.nl80211_id, 0, 0, NL80211_CMD_GET_STATION, 0);
                NLA_PUT(nl_ctx.message, NL80211_ATTR_IFINDEX, 4, &ifIndex);
                NLA_PUT(nl_ctx.message, NL80211_ATTR_MAC, 6, &apAddress[0]);
                nl_send_auto(nl_ctx.nls, nl_ctx.message);
                nl_recvmsgs(nl_ctx.nls, nl_ctx.callback);
            // cppcheck-suppress unusedLabelConfiguration
            nla_put_failure:
                stats.linkTxBitrate = nl_ctx.callbackTxBitrate;
                stats.linkRxBitrate = nl_ctx.callbackRxBitrate;
                stats.wirelessInactiveTime = nl_ctx.callbackInactiveTime;
                stats.wirelessRxBytes = nl_ctx.callbackRxBytes;
                stats.wirelessRxPackets = nl_ctx.callbackRxPackets;
                stats.wirelessRxDropped = nl_ctx.callbackRxDropped;
                stats.wirelessTxBytes = nl_ctx.callbackTxBytes;
                stats.wirelessTxPackets = nl_ctx.callbackTxPackets;
                stats.wirelessTxRetries = nl_ctx.callbackTxRetries;
                stats.wirelessTxFailed = nl_ctx.callbackTxFailed;
                stats.wirelessExpectedThroughput = nl_ctx.callbackExpectedThroughput;
            }
            netLinkFini(nl_ctx);
        }
    }
#endif

    return rc;
}

#ifdef NRDP_HAS_NETLINK
static void parse_bitrate(struct nlattr *bitrate_attr, DpiVariant &bitrate)
{
    int rate = 0;
    struct nlattr *rinfo[NL80211_RATE_INFO_MAX + 1];
    struct nla_policy rate_policy[NL80211_RATE_INFO_MAX + 1];
    memset(&rate_policy, 0, sizeof(rate_policy));
    rate_policy[NL80211_RATE_INFO_BITRATE].type = NLA_U16;
    rate_policy[NL80211_RATE_INFO_BITRATE32].type = NLA_U32;
    // rate_policy[NL80211_RATE_INFO_MCS].type = NLA_U8;
    // rate_policy[NL80211_RATE_INFO_40_MHZ_WIDTH].type = NLA_FLAG;
    // rate_policy[NL80211_RATE_INFO_SHORT_GI].type = NLA_FLAG;

    if (nla_parse_nested(rinfo, NL80211_RATE_INFO_MAX, bitrate_attr, rate_policy))
    {
        NERROR("NETWORK", "failed to parse nested rate attributes!");
        return;
    }

    if (rinfo[NL80211_RATE_INFO_BITRATE32])
    {
        rate = nla_get_u32(rinfo[NL80211_RATE_INFO_BITRATE32]);
    }
    else if (rinfo[NL80211_RATE_INFO_BITRATE])
    {
        rate = nla_get_u16(rinfo[NL80211_RATE_INFO_BITRATE]);
    }

    if (rate > 0)
    {
        bitrate = static_cast<double>(rate) / 10.0;
    }
    else
    {
        bitrate = DpiVariant();
    }
}

int InterfaceNetwork::netlinkInterfaceCallback(struct nl_msg *msg, void *arg)
{
    netLinkContext *ctx = static_cast<netLinkContext *>(arg);

    struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = static_cast<genlmsghdr *>(nlmsg_data(nlmsg_hdr(msg)));

    nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), nullptr);

    bool gotIndex = false;
    bool gotName = false;
    const char *name = nullptr;
    uint32_t index;

    if (tb_msg[NL80211_ATTR_IFINDEX])
    {
        index = nla_get_u32(tb_msg[NL80211_ATTR_IFINDEX]);
        gotIndex = true;
    }

    if (tb_msg[NL80211_ATTR_IFNAME])
    {
        name = nla_get_string(tb_msg[NL80211_ATTR_IFNAME]);
        gotName = true;
    }

    if (gotIndex && gotName)
    {
        ctx->nameMap[name] = index;
    }

    return NL_SKIP;
}

int InterfaceNetwork::netlinkStationCallback(struct nl_msg *msg, void *arg)
{
    netLinkContext *ctx = static_cast<netLinkContext *>(arg);

    struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = static_cast<genlmsghdr *>(nlmsg_data(nlmsg_hdr(msg)));

    nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), nullptr);

    if (!tb_msg[NL80211_ATTR_STA_INFO])
    {
        return NL_SKIP;
    }

    struct nlattr *sinfo[NL80211_STA_INFO_MAX + 1];
    struct nla_policy stats_policy[NL80211_STA_INFO_MAX + 1];
    memset(&stats_policy, 0, sizeof(stats_policy));
    stats_policy[NL80211_STA_INFO_INACTIVE_TIME].type = NLA_U32;
    stats_policy[NL80211_STA_INFO_RX_BYTES].type = NLA_U32;
    stats_policy[NL80211_STA_INFO_TX_BYTES].type = NLA_U32;
    stats_policy[NL80211_STA_INFO_RX_BYTES64].type = NLA_U64;
    stats_policy[NL80211_STA_INFO_TX_BYTES64].type = NLA_U64;
    stats_policy[NL80211_STA_INFO_RX_PACKETS].type = NLA_U32;
    stats_policy[NL80211_STA_INFO_TX_PACKETS].type = NLA_U32;
    // stats_policy[NL80211_STA_INFO_BEACON_RX].type = NLA_U64;
    // stats_policy[NL80211_STA_INFO_SIGNAL].type = NLA_U8;
    // stats_policy[NL80211_STA_INFO_T_OFFSET].type = NLA_U64;
    stats_policy[NL80211_STA_INFO_TX_BITRATE].type = NLA_NESTED;
    stats_policy[NL80211_STA_INFO_RX_BITRATE].type = NLA_NESTED;
    // stats_policy[NL80211_STA_INFO_LLID].type = NLA_U16;
    // stats_policy[NL80211_STA_INFO_PLID].type = NLA_U16;
    // stats_policy[NL80211_STA_INFO_PLINK_STATE].type = NLA_U8;
    stats_policy[NL80211_STA_INFO_TX_RETRIES].type = NLA_U32;
    stats_policy[NL80211_STA_INFO_TX_FAILED].type = NLA_U32;
    // stats_policy[NL80211_STA_INFO_BEACON_LOSS].type = NLA_U32;
    stats_policy[NL80211_STA_INFO_RX_DROP_MISC].type = NLA_U64;
    // stats_policy[NL80211_STA_INFO_STA_FLAGS].minlen = sizeof(struct nl80211_sta_flag_update);
    // stats_policy[NL80211_STA_INFO_LOCAL_PM].type = NLA_U32;
    // stats_policy[NL80211_STA_INFO_PEER_PM].type = NLA_U32;
    // stats_policy[NL80211_STA_INFO_NONPEER_PM].type = NLA_U32;
    // stats_policy[NL80211_STA_INFO_CHAIN_SIGNAL].type = NLA_NESTED;
    // stats_policy[NL80211_STA_INFO_CHAIN_SIGNAL_AVG].type = NLA_NESTED;
    // stats_policy[NL80211_STA_INFO_TID_STATS].type = NLA_NESTED;
    // stats_policy[NL80211_STA_INFO_BSS_PARAM].type = NLA_NESTED;
    // stats_policy[NL80211_STA_INFO_RX_DURATION].type = NLA_U64;
    stats_policy[NL80211_STA_INFO_EXPECTED_THROUGHPUT].type = NLA_U32;

    if (nla_parse_nested(sinfo, NL80211_STA_INFO_MAX, tb_msg[NL80211_ATTR_STA_INFO], stats_policy))
    {
        return NL_SKIP;
    }

    if (sinfo[NL80211_STA_INFO_INACTIVE_TIME])
    {
        ctx->callbackInactiveTime = nla_get_u32(sinfo[NL80211_STA_INFO_INACTIVE_TIME]);
    }

    if (sinfo[NL80211_STA_INFO_RX_BYTES64])
    {
        ctx->callbackRxBytes = static_cast<unsigned long long>(nla_get_u64(sinfo[NL80211_STA_INFO_RX_BYTES64]));
    }
    else if (sinfo[NL80211_STA_INFO_RX_BYTES])
    {
        ctx->callbackRxBytes = nla_get_u32(sinfo[NL80211_STA_INFO_RX_BYTES]);
    }

    if (sinfo[NL80211_STA_INFO_RX_PACKETS])
    {
        ctx->callbackRxPackets = nla_get_u32(sinfo[NL80211_STA_INFO_RX_PACKETS]);
    }

    if (sinfo[NL80211_STA_INFO_RX_DROP_MISC])
    {
        ctx->callbackRxDropped = nla_get_u32(sinfo[NL80211_STA_INFO_RX_DROP_MISC]);
    }

    if (sinfo[NL80211_STA_INFO_TX_BYTES64])
    {
        ctx->callbackTxBytes = static_cast<unsigned long long>(nla_get_u64(sinfo[NL80211_STA_INFO_TX_BYTES64]));
    }
    else if (sinfo[NL80211_STA_INFO_TX_BYTES])
    {
        ctx->callbackTxBytes = nla_get_u32(sinfo[NL80211_STA_INFO_TX_BYTES]);
    }

    if (sinfo[NL80211_STA_INFO_TX_PACKETS])
    {
        ctx->callbackTxPackets = nla_get_u32(sinfo[NL80211_STA_INFO_TX_PACKETS]);
    }

    if (sinfo[NL80211_STA_INFO_TX_RETRIES])
    {
        ctx->callbackTxRetries = nla_get_u32(sinfo[NL80211_STA_INFO_TX_RETRIES]);
    }

    if (sinfo[NL80211_STA_INFO_TX_FAILED])
    {
        ctx->callbackTxFailed = nla_get_u32(sinfo[NL80211_STA_INFO_TX_FAILED]);
    }

    if (sinfo[NL80211_STA_INFO_TX_BITRATE])
    {
        parse_bitrate(sinfo[NL80211_STA_INFO_TX_BITRATE], ctx->callbackTxBitrate);
    }

    if (sinfo[NL80211_STA_INFO_RX_BITRATE])
    {
        parse_bitrate(sinfo[NL80211_STA_INFO_RX_BITRATE], ctx->callbackRxBitrate);
    }

    if (sinfo[NL80211_STA_INFO_EXPECTED_THROUGHPUT])
    {
        double thr = nla_get_u32(sinfo[NL80211_STA_INFO_EXPECTED_THROUGHPUT]);
        thr = thr * 1000 / 1024;
        thr /= 1000.0;
        ctx->callbackExpectedThroughput = thr;
    }

    return NL_SKIP;
}

bool InterfaceNetwork::netLinkInit(netLinkContext &ctx, NETLINK_CALLBACK cb)
{
    ctx.callbackInactiveTime = DpiVariant();
    ctx.callbackRxBytes = DpiVariant();
    ctx.callbackRxPackets = DpiVariant();
    ctx.callbackRxDropped = DpiVariant();
    ctx.callbackTxBytes = DpiVariant();
    ctx.callbackTxPackets = DpiVariant();
    ctx.callbackTxRetries = DpiVariant();
    ctx.callbackTxFailed = DpiVariant();
    ctx.callbackTxBitrate = DpiVariant();
    ctx.callbackRxBitrate = DpiVariant();
    ctx.callbackExpectedThroughput = DpiVariant();
    ctx.nameMap.clear();
    ctx.nls = nullptr;
    ctx.nl80211_id = 0;
    ctx.message = nullptr;
    ctx.callback = nullptr;

    do
    {
        ctx.nls = nl_socket_alloc();
        if (!ctx.nls)
        {
            if (!mSuppressNetlinkError)
            {
                NERROR("NETWORK", "Failed to allocate netlink socket.");
            }

            break;
        }

        if (genl_connect(ctx.nls))
        {
            if (!mSuppressNetlinkError)
            {
                NERROR("NETWORK", "Failed to connect to generic netlink.");
            }

            break;
        }

        ctx.nl80211_id = genl_ctrl_resolve(ctx.nls, "nl80211");
        if (ctx.nl80211_id < 0)
        {
            if (!mSuppressNetlinkError)
            {
                NERROR("NETWORK", "nl80211 not found.");
            }

            break;
        }

        ctx.message = nlmsg_alloc();
        if (!ctx.message)
        {
            if (!mSuppressNetlinkError)
            {
                NERROR("NETWORK", "Failed to allocate netlink message.");
            }

            break;
        }

        ctx.callback = nl_cb_alloc(NL_CB_DEFAULT);
        if (!ctx.callback)
        {
            if (!mSuppressNetlinkError)
            {
                NERROR("NETWORK", "Failed to allocate netlink interface callback.");
            }

            break;
        }

        if (nl_cb_set(ctx.callback, NL_CB_VALID, NL_CB_CUSTOM, cb, &ctx))
        {
            if (!mSuppressNetlinkError)
            {
                NERROR("NETWORK", "Failed to set up callbacks.");
            }

            break;
        }

        return true;
    } while (0);

    mSuppressNetlinkError = true;
    return false;
}

void InterfaceNetwork::netLinkFini(netLinkContext &ctx)
{
    if (ctx.callback)
    {
        nl_cb_put(ctx.callback);
    }

    if (ctx.message)
    {
        nlmsg_free(ctx.message);
    }

    if (ctx.nls)
    {
        nl_socket_free(ctx.nls);
    }

    ctx.callback = nullptr;
    ctx.message = nullptr;
    ctx.nls = nullptr;
    ctx.callbackInactiveTime = DpiVariant();
    ctx.callbackRxBytes = DpiVariant();
    ctx.callbackRxPackets = DpiVariant();
    ctx.callbackRxDropped = DpiVariant();
    ctx.callbackTxBytes = DpiVariant();
    ctx.callbackTxPackets = DpiVariant();
    ctx.callbackTxRetries = DpiVariant();
    ctx.callbackTxFailed = DpiVariant();
    ctx.callbackTxBitrate = DpiVariant();
    ctx.callbackRxBitrate = DpiVariant();
    ctx.callbackExpectedThroughput = DpiVariant();
    ctx.nameMap.clear();
}
#endif

static bool niInterfaceEqual(const DpiNetworkInterface &lhs, const DpiNetworkInterface &rhs)
{
    return ((lhs.name == rhs.name) &&
            (lhs.physicalLayerType == rhs.physicalLayerType) &&
            (lhs.physicalLayerSubtype == rhs.physicalLayerSubtype) &&
            (lhs.ipAddress == rhs.ipAddress) &&
            (lhs.ipv6Addresses == rhs.ipv6Addresses) &&
            (lhs.macAddress == rhs.macAddress) &&
            (lhs.ssid == rhs.ssid) &&
            (lhs.mobileCarrier == rhs.mobileCarrier) &&
            (lhs.mobileCountryCode == rhs.mobileCountryCode) &&
            (lhs.mobileNetworkCode == rhs.mobileNetworkCode) &&
            (lhs.isDefault == rhs.isDefault) &&
            (lhs.linkConnected == rhs.linkConnected) &&
            (lhs.internetConnected == rhs.internetConnected) &&
            (lhs.driverName == rhs.driverName) &&
            (lhs.driverVersion == rhs.driverVersion) &&
            (lhs.driverFirmwareVersion == rhs.driverFirmwareVersion) &&
            (lhs.wirelessProtocol == rhs.wirelessProtocol) &&
            (lhs.additionalInfo == rhs.additionalInfo));
}

namespace TheNetworkAdapter
{

static struct
{
    bool isOpen = false;
    InterfaceNetwork *mInterfaceNetwork;
    NF_InterfaceNetworkEventDispatcher *mNetworkChangedEventer;
} data_;

#define CONDITIONAL_COPY_STRING(n)                            \
    {                                                         \
        if (!it.n.isNull())                                   \
        {                                                     \
            NF_StringCreate(&in->n);                          \
            NF_StringSetString(in->n, it.n.string().c_str()); \
        }                                                     \
    }

// inEvent specifies whether thius was called from within an event,
// in that case use cached interface list and not the fresh one
void pushNetworkChangedEvent(bool inEvent, bool updateDns)
{
    std::vector<DpiNetworkInterface> interfaces;

    if (inEvent)
    {
        NINFO("Network", "Pushing cached network interfaces");
        interfaces = data_.mInterfaceNetwork->getCachedNetworkInterfaces();
    }
    else
    {
        NINFO("Network", "Pushing fresh network interfaces");
        interfaces = data_.mInterfaceNetwork->getNetworkInterfaces();
    }

    std::vector<NF_NetworkInterfaceConfiguration> cnis(interfaces.size());

    int i = 0;
    for (auto &it : interfaces)
    {
        NF_NetworkInterfaceConfiguration *in = &cnis[i];
        memset(in, 0, sizeof(NF_NetworkInterfaceConfiguration));

        NF_StringCreate(&in->name);
        NF_StringSetString(in->name, it.name.c_str());
        NDBG("Network", "pushing %s (%s) in update", it.name.c_str(), it.ipAddress.toString().c_str());
        in->physicalLayerType = it.physicalLayerType;
        in->physicalLayerSubtype = it.physicalLayerSubtype;

        std::string tmpAdditionalInfo = "{";
        // ipv4 address
        if ((it.ipAddress.isValid() && (it.ipAddress.version() == NF_IP_V4)))
        {
            NF_StringCreate(&in->ipv4Address);
            NF_StringSetString(in->ipv4Address, it.ipAddress.toString().c_str());
            if( !it.ipPrefix.empty())
            {
                tmpAdditionalInfo += "\"prefixLength\":" + it.ipPrefix;
            }
        }

        // ipv6 address
        NF_StringArrayCreate(&in->ipv6Addresses);
        for (auto &iit : it.ipv6Addresses)
        {
            NF_StringArrayPushBack(in->ipv6Addresses, iit.c_str());
        }

        if (it.ipv6Prefixes.size())
        {
            if (tmpAdditionalInfo.length() > 1)
            {
                tmpAdditionalInfo += ",";
            }

            tmpAdditionalInfo += "\"ipv6PrefixLengths\":\"[";
            for (auto &iit : it.ipv6Prefixes)
            {
                tmpAdditionalInfo += iit + ",";
            }

            tmpAdditionalInfo.pop_back(); // remove last coma
            tmpAdditionalInfo += "]\"";
        }

        if (tmpAdditionalInfo.length() > 1)
        {
            tmpAdditionalInfo += "}";
            NF_StringCreate(&in->additionalInfo);
            NF_StringSetString(in->additionalInfo, tmpAdditionalInfo.c_str());
        }
        else
        {
            CONDITIONAL_COPY_STRING(additionalInfo);
        }

        CONDITIONAL_COPY_STRING(macAddress);
        CONDITIONAL_COPY_STRING(ssid);
        CONDITIONAL_COPY_STRING(mobileCarrier);
        CONDITIONAL_COPY_STRING(mobileCountryCode);
        CONDITIONAL_COPY_STRING(mobileNetworkCode);
        in->isDefault = it.isDefault;
        in->linkConnected = it.linkConnected;
        in->internetConnected = it.internetConnected;
        CONDITIONAL_COPY_STRING(driverName);
        CONDITIONAL_COPY_STRING(driverVersion);
        CONDITIONAL_COPY_STRING(driverFirmwareVersion);
        CONDITIONAL_COPY_STRING(wirelessProtocol);

        ++i;
    }

    // DNS
    NF_StringArray *cdns = nullptr;
    if (updateDns)
    {
        auto dnslist = data_.mInterfaceNetwork->getCachedDNSList();
        NF_StringArrayCreate(&cdns);
        for (auto &d : dnslist)
        {
            NF_StringArrayPushBack(cdns, d.c_str());
            NDBG("Network", "DNS: %s", d.c_str());
        }
    }

    data_.mNetworkChangedEventer->networkChanged(cnis.data(), cnis.size(), cdns);

    for (auto &it : cnis)
    {
        NF_StringDestroy(&it.name);
        NF_StringDestroy(&it.ipv4Address);
        NF_StringArrayDestroy(&it.ipv6Addresses);
        NF_StringDestroy(&it.macAddress);
        NF_StringDestroy(&it.ssid);
        NF_StringDestroy(&it.mobileCarrier);
        NF_StringDestroy(&it.mobileCountryCode);
        NF_StringDestroy(&it.mobileNetworkCode);
        NF_StringDestroy(&it.driverName);
        NF_StringDestroy(&it.driverVersion);
        NF_StringDestroy(&it.driverFirmwareVersion);
        NF_StringDestroy(&it.wirelessProtocol);
        NF_StringDestroy(&it.additionalInfo);
    }
    if (cdns)
    {
        NF_StringArrayDestroy(&cdns);
    }
}

#undef CONDITIONAL_COPY_STRING
#define CONDITIONAL_COPY_STRING(n)                                     \
    do                                                                 \
    {                                                                  \
        if (!dpiStats.n.isNull())                                      \
        {                                                              \
            NF_StringSetString(stats->n, dpiStats.n.string().c_str()); \
        }                                                              \
    } while (false)
#define CONDITIONAL_COPY_UINT32(n)                                  \
    do                                                              \
    {                                                               \
        if (!dpiStats.n.isNull())                                   \
            stats->n = static_cast<uint32_t>(dpiStats.n.integer()); \
        else                                                        \
            stats->n = INETWORK_INVALID_STAT32;                     \
    } while (false)
#define CONDITIONAL_COPY_UINT64(n)                                  \
    do                                                              \
    {                                                               \
        if (!dpiStats.n.isNull())                                   \
            stats->n = static_cast<uint64_t>(dpiStats.n.integer()); \
        else                                                        \
            stats->n = INETWORK_INVALID_STAT64;                     \
    } while (false)
#define CONDITIONAL_COPY_DBL(n)                               \
    do                                                        \
    {                                                         \
        if (!dpiStats.n.isNull())                             \
            stats->n = static_cast<double>(dpiStats.n.dbl()); \
        else                                                  \
            stats->n = INETWORK_INVALID_STATDBL;              \
    } while (false)

static NF_ResultCode getStatistics(const char *ifaceName, NF_NetworkInterfaceStatistics *stats)
{
    NF_ResultCode rc;
    DpiNetworkInterfaceStatistics dpiStats;
    rc = data_.mInterfaceNetwork->getNetworkStatistics(ifaceName, dpiStats);
    NDBG("Network", "getStatistics called for %s. retcode %d", ifaceName, rc);
    if (rc == NF_RC_SUCCESS)
    {
        CONDITIONAL_COPY_UINT64(txPackets);
        CONDITIONAL_COPY_UINT64(txError);
        CONDITIONAL_COPY_UINT64(txDropped);
        CONDITIONAL_COPY_UINT32(txFifoErrors);
        CONDITIONAL_COPY_UINT32(txCarrierErrors);
        CONDITIONAL_COPY_UINT64(rxPackets);
        CONDITIONAL_COPY_UINT64(rxError);
        CONDITIONAL_COPY_UINT64(rxDropped);
        CONDITIONAL_COPY_UINT32(rxFifoErrors);
        CONDITIONAL_COPY_UINT32(rxFrameErrors);
        CONDITIONAL_COPY_UINT32(linkTxBitrate);
        CONDITIONAL_COPY_UINT32(linkRxBitrate);
        CONDITIONAL_COPY_DBL(wirelessFrequency);
        CONDITIONAL_COPY_DBL(wirelessQuality);
        CONDITIONAL_COPY_DBL(wirelessSignal);
        CONDITIONAL_COPY_STRING(wirelessApAddress);
        CONDITIONAL_COPY_UINT32(wirelessInactiveTime);
        CONDITIONAL_COPY_UINT32(wirelessRxBytes);
        CONDITIONAL_COPY_UINT32(wirelessRxPackets);
        CONDITIONAL_COPY_UINT32(wirelessRxDropped);
        CONDITIONAL_COPY_UINT32(wirelessTxBytes);
        CONDITIONAL_COPY_UINT32(wirelessTxPackets);
        CONDITIONAL_COPY_UINT32(wirelessTxRetries);
        CONDITIONAL_COPY_UINT32(wirelessTxFailed);
        CONDITIONAL_COPY_DBL(wirelessExpectedThroughput);
    }
    return rc;
}

static NF_ResultCode open(NF_InterfaceNetworkEventDispatcher *eventDispatcher)
{
    if (!data_.isOpen)
    {
        data_.mNetworkChangedEventer = eventDispatcher;
        data_.mInterfaceNetwork = new InterfaceNetwork();
        data_.isOpen = true;
        pushNetworkChangedEvent(false, true);
    }
    return data_.isOpen ? NF_RC_SUCCESS : NF_RC_FAIL;
}

static NF_ResultCode close()
{
    if (data_.isOpen)
    {
        delete data_.mInterfaceNetwork;
    }
    return NF_RC_SUCCESS;
}

static void destroy()
{
    data_.mNetworkChangedEventer = nullptr;
}

} // TheNetworkAdapter

const NF_Interface *getNetworkInterface()
{
    static const NF_InterfaceNetwork ni = {
        {NF_IID_NETWORK_V1,
         TheNetworkAdapter::destroy},
        TheNetworkAdapter::open,
        TheNetworkAdapter::close,
        TheNetworkAdapter::getStatistics};
    return reinterpret_cast<const NF_Interface *>(&ni);
}
