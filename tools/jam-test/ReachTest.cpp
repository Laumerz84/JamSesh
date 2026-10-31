// Checks jam::Reach (Source/Jam/JamReach.h), which makes a host reachable over the internet, without touching
// a real router: a fake router (UPnP over HTTP) and a fake STUN server, both on 127.0.0.1 in this process.
// Owner: test soldier.
//   ClodJamTest --reach          the pieces, every verdict, and a real jam::Session hosting through the fakes
//   ClodJamTest --reach --live   also asks the real STUN servers and finds the real router, read only:
//                                discovery, its description and GetExternalIPAddress, never AddPortMapping
#include "../../Source/Jam/JamReach.h"
#include "../../Source/Jam/JamProtocol.h"
#include "../../Source/Jam/JamSession.h"
#include <atomic>
#include <cstdlib>
#include <iostream>

extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableA (const char*, const char*);

namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "PASS  " : "FAIL  ") << what << std::endl;
        if (! ok)
            ++failures;
    }

    void setEnv (const char* name, const juce::String& value)
    {
        _putenv_s (name, value.toRawUTF8());
        SetEnvironmentVariableA (name, value.toRawUTF8());
    }

    juce::String between (const juce::String& s, const juce::String& open, const juce::String& close)
    {
        return s.fromFirstOccurrenceOf (open, false, false).upToFirstOccurrenceOf (close, false, false);
    }

    // answers STUN binding requests with a fixed internet address
    struct FakeStun : juce::Thread
    {
        juce::DatagramSocket socket { false };
        juce::uint32 mapped;

        explicit FakeStun (juce::uint32 ip) : juce::Thread ("fake stun"), mapped (ip)
        {
            socket.bindToPort (0, "127.0.0.1");
            startThread();
        }
        ~FakeStun() override
        {
            signalThreadShouldExit();
            stopThread (2000);
        }
        juce::String address() const { return "127.0.0.1:" + juce::String (socket.getBoundPort()); }

        void run() override
        {
            while (! threadShouldExit())
            {
                if (socket.waitUntilReady (true, 50) != 1)
                    continue;
                juce::uint8 b[600];
                juce::String from;
                int fromPort = 0;
                const int n = socket.read (b, (int) sizeof (b), false, from, fromPort);
                if (n < 20 || b[0] != 0x00 || b[1] != 0x01)
                    continue;
                juce::uint8 r[32] = { 0x01, 0x01, 0x00, 0x0c };
                std::memcpy (r + 4, b + 4, 16);                               // cookie and transaction
                const juce::uint8 attr[4] = { 0x00, 0x20, 0x00, 0x08 };       // XOR-MAPPED-ADDRESS, IPv4
                std::memcpy (r + 20, attr, 4);
                r[24] = 0x00;
                r[25] = 0x01;
                const int xport = fromPort ^ 0x2112;
                r[26] = (juce::uint8) (xport >> 8);
                r[27] = (juce::uint8) xport;
                const auto x = mapped ^ 0x2112A442u;
                for (int i = 0; i < 4; ++i)
                    r[28 + i] = (juce::uint8) (x >> (24 - 8 * i));
                socket.write (from, fromPort, r, (int) sizeof (r));
            }
        }
    };

    // a UPnP router on 127.0.0.1: GET /desc.xml lists a WANPPPConnection before the WANIPConnection (the IP one
    // must be tried first); POST /ctl/IPConn answers GetExternalIPAddress, AddPortMapping, DeletePortMapping
    struct FakeRouter : juce::Thread
    {
        juce::StreamingSocket listener;
        juce::String externalIp;
        bool permanentOnly = false;
        int addError = 0;
        juce::CriticalSection lock;
        juce::StringArray calls;

        explicit FakeRouter (const juce::String& ext) : juce::Thread ("fake router"), externalIp (ext)
        {
            listener.createListener (0, "127.0.0.1");
            startThread();
        }
        ~FakeRouter() override
        {
            signalThreadShouldExit();
            stopThread (3000);
            listener.close();
        }
        juce::String description() const { return "http://127.0.0.1:" + juce::String (listener.getBoundPort()) + "/desc.xml"; }
        juce::StringArray log()
        {
            const juce::ScopedLock sl (lock);
            return calls;
        }

        void run() override
        {
            while (! threadShouldExit())
                if (listener.waitUntilReady (true, 50) == 1)
                    if (std::unique_ptr<juce::StreamingSocket> c { listener.waitForNextConnection() })
                        serve (*c);
        }

        void serve (juce::StreamingSocket& c)
        {
            juce::MemoryBlock got;
            juce::String text;
            const auto start = juce::Time::getMillisecondCounter();
            while (juce::Time::getMillisecondCounter() - start < 2000)
            {
                if (c.waitUntilReady (true, 50) != 1)
                    continue;
                char buf[4096];
                const int n = c.read (buf, (int) sizeof (buf), false);
                if (n <= 0)
                    break;
                got.append (buf, (size_t) n);
                text = got.toString();
                const int headerEnd = text.indexOf ("\r\n\r\n");
                const int length = between (text, "Content-Length:", "\r\n").trim().getIntValue();
                if (headerEnd >= 0 && text.length() - (headerEnd + 4) >= length)
                    break;
            }

            auto respond = [&c] (int status, const juce::String& body)
            {
                juce::String r;
                r << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Error") << "\r\nContent-Type: text/xml\r\n"
                  << "Content-Length: " << (int) body.getNumBytesAsUTF8() << "\r\nConnection: close\r\n\r\n" << body;
                c.write (r.toRawUTF8(), (int) r.getNumBytesAsUTF8());
            };
            auto fault = [] (int code)
            {
                return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
                       "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail><UPnPError "
                       "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" + juce::String (code) + "</errorCode>"
                       "<errorDescription>test</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
            };
            auto ok = [] (const juce::String& action, const juce::String& inner)
            {
                return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:"
                       + action + "Response xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">" + inner
                       + "</u:" + action + "Response></s:Body></s:Envelope>";
            };

            const juce::ScopedLock sl (lock);
            if (text.startsWith ("GET /desc.xml"))
            {
                respond (200, "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
                              "<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><deviceList><device>"
                              "<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>"
                              "<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType><serviceList>"
                              "<service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
                              "<controlURL>/ctl/PPPConn</controlURL></service>"
                              "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                              "<controlURL>ctl/IPConn</controlURL></service>"
                              "</serviceList></device></deviceList></device></deviceList></device></root>");
            }
            else if (text.startsWith ("POST /ctl/IPConn"))
            {
                const auto body = text.fromFirstOccurrenceOf ("\r\n\r\n", false, false);
                if (text.contains ("#GetExternalIPAddress"))
                {
                    calls.add ("GetExternalIPAddress");
                    respond (200, ok ("GetExternalIPAddress", "<NewExternalIPAddress>" + externalIp + "</NewExternalIPAddress>"));
                }
                else if (text.contains ("#AddPortMapping"))
                {
                    const auto lease = between (body, "<NewLeaseDuration>", "<");
                    calls.add ("AddPortMapping port=" + between (body, "<NewExternalPort>", "<") + " protocol="
                               + between (body, "<NewProtocol>", "<") + " lease=" + lease);
                    if (addError != 0)
                        respond (500, fault (addError));
                    else if (permanentOnly && lease != "0")
                        respond (500, fault (725));
                    else
                        respond (200, ok ("AddPortMapping", {}));
                }
                else if (text.contains ("#DeletePortMapping"))
                {
                    calls.add ("DeletePortMapping port=" + between (body, "<NewExternalPort>", "<"));
                    respond (200, ok ("DeletePortMapping", {}));
                }
                else
                {
                    respond (500, fault (401));
                }
            }
            else
            {
                respond (404, {});
            }
        }
    };

    jam::ReachInfo waitForVerdict (std::function<jam::ReachInfo()> info, double seconds = 10.0)
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
        auto r = info();
        while (r.state == jam::ReachState::checking && juce::Time::getMillisecondCounterHiRes() < until)
        {
            juce::Thread::sleep (20);
            r = info();
        }
        return r;
    }

    // one host session through jam::Reach with these fakes; the router log afterwards includes stop()
    jam::ReachInfo reachWith (const juce::String& stun, const juce::String& router, int port = 47977)
    {
        jam::Reach::Config config;
        config.stunServers = juce::StringArray (stun);
        config.routerDescription = router;
        jam::Reach reach (config);
        std::atomic<int> changes { 0 };
        reach.start (port, [&changes] { ++changes; });
        const auto r = waitForVerdict ([&reach] { return reach.info(); });
        reach.stop();
        check (changes.load() > 0 && reach.info().state == jam::ReachState::off, "  reports changes, off after stop");
        return r;
    }

    juce::String ip (juce::uint32 a) { return jam::protocol::ipToString (a); }

    //==============================================================================
    void pieces()
    {
        std::cout << "-- pieces" << std::endl;
        const juce::uint8 tx[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        const auto request = jam::reach::stunRequest (tx);
        const auto* q = (const juce::uint8*) request.getData();
        check (request.getSize() == 20 && q[1] == 0x01 && q[4] == 0x21 && q[7] == 0x42 && q[8] == 1 && q[19] == 12, "STUN request: binding, magic cookie, transaction");

        juce::uint8 answer[32] = { 0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xA4, 0x42 };
        std::memcpy (answer + 8, tx, 12);
        const juce::uint8 attr[12] = { 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0x9d, 0x8c, 0xea, 0x12, 0xd5, 0x45 };   // 203.0.113.7, XORed
        std::memcpy (answer + 20, attr, 12);
        juce::uint32 addr = 0;
        int port = 0;
        const bool read = jam::reach::parseStunResponse (answer, 32, tx, addr, port);
        check (read && ip (addr) == "203.0.113.7" && port == (0x9d8c ^ 0x2112),
               "STUN answer: XOR-MAPPED-ADDRESS read (" + ip (addr) + ":" + juce::String (port) + ")");
        const juce::uint8 otherTx[12] = {};
        check (! jam::reach::parseStunResponse (answer, 32, otherTx, addr, port), "STUN answer for another request ignored");

        jam::reach::Url u;
        check (jam::reach::parseUrl ("http://192.168.1.1:5000/rootDesc.xml", u) && u.host == "192.168.1.1" && u.port == 5000
                 && u.path == "/rootDesc.xml", "URL parsed");
        check (jam::reach::parseUrl ("http://10.0.0.1", u) && u.port == 80 && u.path == "/", "URL without port or path");
        check (jam::reach::ssdpLocation ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nLocation: http://192.168.1.1:1900/igd.xml\r\n\r\n")
                 == "http://192.168.1.1:1900/igd.xml", "SSDP answer: LOCATION read");
        check (jam::reach::soapError ("<s:Fault><detail><UPnPError><errorCode>718</errorCode></UPnPError></detail>") == 718, "SOAP fault: error code");
        check (jam::reach::soapValue ("<u:R><NewExternalIPAddress>203.0.113.7</NewExternalIPAddress></u:R>", "NewExternalIPAddress")
                 == "203.0.113.7", "SOAP answer: value");
        check (jam::reach::isPrivate (jam::protocol::parseIPv4 ("100.72.1.2")) && jam::reach::isPrivate (jam::protocol::parseIPv4 ("192.168.1.20"))
                 && ! jam::reach::isPrivate (jam::protocol::parseIPv4 ("203.0.113.7")), "private and shared ranges");

        const auto own = jam::reach::verdict (47800, jam::protocol::parseIPv4 ("203.0.113.7"), 0, 0, false, false, 0, true);
        check (own.state == jam::ReachState::open, "verdict: a PC with its own internet address is open");
    }

    void scenarios()
    {
        std::cout << "-- fake router and STUN server" << std::endl;
        const auto internet = jam::protocol::parseIPv4 ("203.0.113.7");
        FakeStun stun (internet);

        {
            FakeRouter router ("203.0.113.7");
            const auto r = reachWith (stun.address(), router.description());
            const auto log = router.log();
            check (r.state == jam::ReachState::open && r.publicIp == internet && r.routerIp == internet, "router opens the port: open (" + r.text + ")");
            check (log.contains ("AddPortMapping port=47977 protocol=UDP lease=3600"), "  asked for UDP 47977 with a 1-hour lease");
            check (log.contains ("DeletePortMapping port=47977"), "  removed the mapping when hosting stopped");
            check (log.indexOf ("GetExternalIPAddress") == 0, "  WANIPConnection tried first (relative control URL)");
        }
        {
            FakeRouter router ("203.0.113.7");
            router.permanentOnly = true;
            const auto r = reachWith (stun.address(), router.description());
            check (r.state == jam::ReachState::open && router.log().contains ("AddPortMapping port=47977 protocol=UDP lease=0"),
                   "router with permanent mappings only: retried without a lease, open");
        }
        {
            FakeRouter router ("100.72.13.4");
            const auto r = reachWith (stun.address(), router.description());
            check (r.state == jam::ReachState::shared, "router behind the provider's shared address: shared (" + r.text + ")");
        }
        {
            FakeRouter router ("198.51.100.9");
            const auto r = reachWith (stun.address(), router.description());
            check (r.state == jam::ReachState::shared, "router address differs from the internet address (second router): shared");
        }
        {
            FakeRouter router ("203.0.113.7");
            router.addError = 718;
            const auto r = reachWith (stun.address(), router.description());
            check (r.state == jam::ReachState::forwardPort && r.text.contains ("another device") && r.publicIp == internet,
                   "port taken by another device: forward-port advice (" + r.text + ")");
        }
        {
            const auto r = reachWith (stun.address(), "http://127.0.0.1:1/desc.xml");
            check (r.state == jam::ReachState::forwardPort && r.text.contains ("didn't answer") && r.publicIp == internet,
                   "no router: internet address known, forward-port advice (" + r.text + ")");
        }
        {
            const auto r = reachWith ("127.0.0.1:1", "http://127.0.0.1:1/desc.xml");
            check (r.state == jam::ReachState::offline && r.publicIp == 0, "no STUN answer and no router: offline (" + r.text + ")");
        }
    }

    // a real jam::Session hosting on 127.0.0.1 through the fakes: the internet address joins the code
    void session()
    {
        std::cout << "-- jam::Session hosting through the fakes" << std::endl;
        FakeStun stun (jam::protocol::parseIPv4 ("203.0.113.7"));
        FakeRouter router ("203.0.113.7");
        setEnv ("CLOD_JAM_BIND", "127.0.0.1");
        setEnv ("CLOD_JAM_REACH", "stun=" + stun.address() + ",router=" + router.description());
        {
            juce::SharedResourcePointer<jam::Session> s;
            juce::String err;
            const bool hosting = s->host ("reach test", 47978, err);
            check (hosting, "host on 127.0.0.1:47978 " + err);
            const auto r = waitForVerdict ([&s] { return s->reachInfo(); });
            jam::protocol::CodeInfo code;
            const bool parsed = jam::protocol::parseCode (s->sessionCode(), code);
            juce::StringArray addrs;
            for (auto a : code.addresses)
                addrs.add (ip (a));
            check (r.state == jam::ReachState::open, "the session reports open");
            check (parsed && addrs.contains ("127.0.0.1") && addrs.contains ("203.0.113.7") && code.port == 47978,
                   "the code holds the local and the internet address (" + addrs.joinIntoString (", ") + ")");
            s->leave();
            check (s->reachInfo().state == jam::ReachState::off && router.log().contains ("DeletePortMapping port=47978"),
                   "leaving removes the router mapping");
        }
        setEnv ("CLOD_JAM_REACH", {});
    }

    // read only: what the real STUN servers and the real router say (never AddPortMapping)
    void live()
    {
        std::cout << "-- live, read only" << std::endl;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto internet = jam::reach::askStun (jam::Reach::Config().stunServers, 3000);
        std::cout << "internet address (STUN): " << (internet != 0 ? ip (internet) : juce::String ("no answer"))
                  << " in " << juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 0) << " ms" << std::endl;
        juce::StringArray locals;
        for (auto a : jam::reach::localAddresses())
            locals.add (ip (a));
        std::cout << "local addresses: " << locals.joinIntoString (", ") << std::endl;

        const auto routers = jam::reach::discoverRouters (3000);
        std::cout << "routers found (UPnP): " << routers.size() << std::endl;
        for (auto& d : routers)
        {
            std::cout << "  description " << d << std::endl;
            jam::reach::Url u;
            if (! jam::reach::parseUrl (d, u))
                continue;
            const auto desc = jam::reach::http (u, "GET", {}, {}, 2000);
            std::cout << "  HTTP " << desc.status << ", " << desc.body.length() << " characters" << std::endl;
            for (auto& g : jam::reach::gatewayServices (desc.body, u))
            {
                const auto ext = jam::reach::soap (g, "GetExternalIPAddress", juce::StringPairArray(), 2000);
                std::cout << "  " << g.serviceType << " at " << g.control.host << ":" << g.control.port << g.control.path
                          << " -> HTTP " << ext.status << ", router's address "
                          << jam::reach::soapValue (ext.body, "NewExternalIPAddress") << ", error " << jam::reach::soapError (ext.body) << std::endl;
            }
        }
    }
}

int runReachTests (bool withLive)
{
    pieces();
    scenarios();
    session();
    if (withLive)
        live();
    std::cout << (failures == 0 ? "REACH: PASS" : "REACH: FAIL (" + juce::String (failures) + ")") << std::endl;
    return failures == 0 ? 0 : 1;
}
