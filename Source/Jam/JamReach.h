#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <memory>

// Reaching a host over the internet (owner: net soldier). Friends outside the host's home can only join
// when the session code holds the host's internet address and the host's router lets them in. While a
// player hosts, jam::Reach works that out on a thread of its own:
//   - asks public STUN servers (Google's, Cloudflare's) for this computer's internet address, which goes
//     into the session code. Nothing else is sent to them.
//   - asks the router to forward the host's UDP port to this PC (UPnP: SSDP discovery, then
//     AddPortMapping), renews that while hosting and removes it when hosting stops.
//   - from both answers tells the host whether friends over the internet can join, and what to do when
//     they can't (forward the port by hand, let someone else host, or a VPN).
// jam::Network starts it in startHost unless CLOD_JAM_BIND is set, so tests never touch a router. The
// pieces in jam::reach are free functions so tools/jam-test can check them against a fake router.
namespace jam
{
    enum class ReachState
    {
        off,           // not hosting, or a test run
        checking,      // asking the STUN servers and the router
        open,          // friends over the internet can join (the router forwards the port, or no router)
        forwardPort,   // the internet address is known, but the router didn't open the port
        shared,        // the router has no internet address of its own (provider-shared address, a second router)
        offline        // no internet address found: only the same network or a VPN
    };

    struct ReachInfo
    {
        ReachState state = ReachState::off;
        juce::uint32 publicIp = 0;   // this computer's internet address, host byte order (0 = unknown)
        juce::uint32 routerIp = 0;   // the internet address the router reports (UPnP; 0 = unknown)
        juce::uint32 lanIp = 0;      // this PC on the router's network
        int port = 0;
        juce::String text;           // for the host: what this means and what to do
    };

    class Reach
    {
    public:
        struct Config
        {
            juce::StringArray stunServers { "stun.l.google.com:19302", "stun.cloudflare.com:3478" };
            juce::String routerDescription;   // a router's description URL: skips discovery (tests)
            int leaseSeconds = 3600;          // the router mapping's lease, renewed at half time
            int discoverMs = 2500;
        };

        Reach();
        explicit Reach (Config);
        ~Reach();

        // hosting began on this UDP port: works in the background and calls onChange (on its own thread)
        // whenever info() changes
        void start (int port, std::function<void()> onChange);
        // hosting ended: removes the router mapping (waits for the router, a second or two at most)
        void stop();
        ReachInfo info() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        JUCE_DECLARE_NON_COPYABLE (Reach)
    };

    namespace reach
    {
        // STUN (RFC 5389): a binding request, and the mapped address from the server's answer
        juce::MemoryBlock stunRequest (const juce::uint8 (&transaction)[12]);
        bool parseStunResponse (const void* data, int size, const juce::uint8 (&transaction)[12], juce::uint32& ip, int& port);
        // asks the servers ("host:port") and returns the first address one reports (0 = none answered)
        juce::uint32 askStun (const juce::StringArray& servers, int timeoutMs, std::function<bool()> shouldStop = {});

        // a small HTTP/1.0 client for routers (plain HTTP, Connection: close, chunked answers decoded)
        struct Url { juce::String host; int port = 80; juce::String path = "/"; };
        struct HttpResult { int status = 0; juce::String body; };
        bool parseUrl (const juce::String& url, Url& out);
        HttpResult http (const Url&, const juce::String& method, const juce::String& extraHeaders,
                         const juce::String& body, int timeoutMs);

        // UPnP Internet Gateway Device
        juce::StringArray discoverRouters (int timeoutMs, std::function<bool()> shouldStop = {});   // description URLs
        juce::String ssdpLocation (const juce::String& ssdpAnswer);                                  // LOCATION header
        struct Gateway { Url control; juce::String serviceType; };
        juce::Array<Gateway> gatewayServices (const juce::String& descriptionXml, const Url& description);   // WANIPConnection first
        juce::String soapEnvelope (const juce::String& serviceType, const juce::String& action, const juce::StringPairArray& args);
        HttpResult soap (const Gateway&, const juce::String& action, const juce::StringPairArray& args, int timeoutMs);
        int soapError (const juce::String& body);                                    // the UPnP errorCode, 0 = none
        juce::String soapValue (const juce::String& body, const juce::String& tag);   // an out argument

        // addresses (host byte order)
        juce::Array<juce::uint32> localAddresses();      // this computer's IPv4 addresses, no loopback / link-local
        bool isPrivate (juce::uint32 ip);                // 0, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.168/16
        ReachInfo verdict (int port, juce::uint32 stunIp, juce::uint32 routerIp, juce::uint32 lanIp,
                           bool routerFound, bool mapped, int mapError, bool ownAddress);
    }
}
