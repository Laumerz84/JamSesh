#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
#else
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <netdb.h>
#endif

#include "JamReach.h"
#include "JamProtocol.h"

// Owner: net soldier. See JamReach.h for what this does and when.
namespace jam
{
namespace reach
{
namespace
{
   #ifdef _WIN32
    using RawSocket = SOCKET;
   #else
    using RawSocket = int;
   #endif

    constexpr juce::uint32 stunCookie = 0x2112A442;

    juce::uint32 msNow() noexcept { return juce::Time::getMillisecondCounter(); }

    juce::uint32 be32 (const juce::uint8* p) noexcept
    {
        return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | (juce::uint32) p[3];
    }

    // the router's network, not a VPN's (Tailscale and the like use 100.64/10)
    bool isLan (juce::uint32 ip) noexcept
    {
        const auto a = ip >> 24, b = (ip >> 16) & 255;
        return a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168);
    }

    // a host name or dotted address to an IPv4 address (host byte order, 0 = unknown)
    juce::uint32 resolve (const juce::String& host)
    {
        if (const auto ip = protocol::parseIPv4 (host))
            return ip;
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* found = nullptr;
        if (getaddrinfo (host.toRawUTF8(), nullptr, &hints, &found) != 0 || found == nullptr)
            return 0;
        const auto ip = (juce::uint32) ntohl (((const sockaddr_in*) found->ai_addr)->sin_addr.s_addr);
        freeaddrinfo (found);
        return ip;
    }

    // the local address on the same network as `ip` (the most leading bits in common)
    juce::uint32 closestTo (const juce::Array<juce::uint32>& locals, juce::uint32 ip)
    {
        juce::uint32 best = 0;
        int bestBits = -1;
        for (auto l : locals)
        {
            int bits = 0;
            while (bits < 32 && ((l ^ ip) & (0x80000000u >> bits)) == 0)
                ++bits;
            if (bits > bestBits || (bits == bestBits && isLan (l) && ! isLan (best)))
            {
                best = l;
                bestBits = bits;
            }
        }
        return best;
    }

    juce::String xmlEscape (const juce::String& s)
    {
        return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;");
    }

    juce::String childText (const juce::XmlElement& e, const juce::String& name)
    {
        for (auto* c : e.getChildIterator())
            if (c->hasTagNameIgnoringNamespace (name))
                return c->getAllSubText().trim();
        return {};
    }

    void collectServices (const juce::XmlElement& e, juce::Array<std::pair<juce::String, juce::String>>& out)
    {
        if (e.hasTagNameIgnoringNamespace ("service"))
            out.add ({ childText (e, "serviceType"), childText (e, "controlURL") });
        for (auto* c : e.getChildIterator())
            collectServices (*c, out);
    }

    Url resolveUrl (const juce::String& ref, Url base)
    {
        Url absolute;
        if (ref.startsWithIgnoreCase ("http://") && parseUrl (ref, absolute))
            return absolute;
        base.path = ref.startsWithChar ('/') ? ref : base.path.upToLastOccurrenceOf ("/", true, false) + ref;
        if (! base.path.startsWithChar ('/'))
            base.path = "/" + base.path;
        return base;
    }

    juce::MemoryBlock dechunk (const juce::MemoryBlock& in)
    {
        juce::MemoryBlock out;
        const auto* d = (const char*) in.getData();
        const int size = (int) in.getSize();
        int pos = 0;
        while (pos < size)
        {
            int lineEnd = pos;
            while (lineEnd + 1 < size && ! (d[lineEnd] == '\r' && d[lineEnd + 1] == '\n'))
                ++lineEnd;
            if (lineEnd + 1 >= size)
                break;
            const int chunk = juce::String (d + pos, (size_t) (lineEnd - pos)).upToFirstOccurrenceOf (";", false, false).trim().getHexValue32();
            if (chunk <= 0 || lineEnd + 2 + chunk > size)
                break;
            out.append (d + lineEnd + 2, (size_t) chunk);
            pos = lineEnd + 2 + chunk + 2;
        }
        return out;
    }

    // the answer is all here: headers plus Content-Length bytes (without one, the server closes when done)
    bool complete (const juce::MemoryBlock& got)
    {
        const auto text = juce::String::fromUTF8 ((const char*) got.getData(), (int) got.getSize());
        const int headerEnd = text.indexOf ("\r\n\r\n");
        if (headerEnd < 0)
            return false;
        for (auto& line : juce::StringArray::fromLines (text.substring (0, headerEnd)))
            if (line.startsWithIgnoreCase ("content-length:"))
                return (int) got.getSize() - (int) text.substring (0, headerEnd + 4).getNumBytesAsUTF8()
                       >= line.fromFirstOccurrenceOf (":", false, false).trim().getIntValue();
        return false;
    }

    int addMapping (const Gateway& g, int port, juce::uint32 lanIp, int lease)
    {
        juce::StringPairArray args;
        args.set ("NewRemoteHost", {});
        args.set ("NewExternalPort", juce::String (port));
        args.set ("NewProtocol", "UDP");
        args.set ("NewInternalPort", juce::String (port));
        args.set ("NewInternalClient", protocol::ipToString (lanIp));
        args.set ("NewEnabled", "1");
        args.set ("NewPortMappingDescription", "Jam Sesh");
        args.set ("NewLeaseDuration", juce::String (lease));
        const auto r = soap (g, "AddPortMapping", args, 2000);
        if (r.status == 200)
            return 0;
        const int e = soapError (r.body);
        return e != 0 ? e : -1;
    }

    void deleteMapping (const Gateway& g, int port)
    {
        juce::StringPairArray args;
        args.set ("NewRemoteHost", {});
        args.set ("NewExternalPort", juce::String (port));
        args.set ("NewProtocol", "UDP");
        soap (g, "DeletePortMapping", args, 1500);
    }
}

//==============================================================================
juce::MemoryBlock stunRequest (const juce::uint8 (&transaction)[12])
{
    juce::uint8 b[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };   // binding request, no attributes
    std::memcpy (b + 8, transaction, 12);
    return { b, sizeof (b) };
}

bool parseStunResponse (const void* data, int size, const juce::uint8 (&transaction)[12], juce::uint32& ip, int& port)
{
    const auto* b = (const juce::uint8*) data;
    if (size < 20 || b[0] != 0x01 || b[1] != 0x01)   // binding success response
        return false;
    const int end = 20 + ((b[2] << 8) | b[3]);
    if (end > size || be32 (b + 4) != stunCookie || std::memcmp (b + 8, transaction, 12) != 0)
        return false;

    juce::uint32 plainIp = 0;
    int plainPort = 0;
    for (int pos = 20; pos + 4 <= end;)
    {
        const int type = (b[pos] << 8) | b[pos + 1], len = (b[pos + 2] << 8) | b[pos + 3];
        const auto* v = b + pos + 4;
        if (pos + 4 + len > end)
            break;
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01)   // XOR-MAPPED-ADDRESS / MAPPED-ADDRESS, IPv4
        {
            const int p = (v[2] << 8) | v[3];
            const auto a = be32 (v + 4);
            if (type == 0x0020)
            {
                ip = a ^ stunCookie;
                port = p ^ (int) (stunCookie >> 16);
                return true;
            }
            plainIp = a;
            plainPort = p;
        }
        pos += 4 + ((len + 3) & ~3);
    }
    if (plainIp == 0)
        return false;
    ip = plainIp;
    port = plainPort;
    return true;
}

juce::uint32 askStun (const juce::StringArray& servers, int timeoutMs, std::function<bool()> shouldStop)
{
    juce::Array<std::pair<juce::String, int>> targets;
    for (auto& server : servers)
    {
        const int p = server.fromLastOccurrenceOf (":", false, false).getIntValue();
        if (const auto ip = resolve (server.upToLastOccurrenceOf (":", false, false).trim()))
            if (p > 0 && p < 65536)
                targets.add ({ protocol::ipToString (ip), p });
    }
    juce::DatagramSocket s (false);
    if (targets.isEmpty() || ! s.bindToPort (0))
        return 0;

    juce::uint8 transaction[12];
    for (auto& t : transaction)
        t = (juce::uint8) juce::Random::getSystemRandom().nextInt (256);
    const auto request = stunRequest (transaction);

    const auto start = msNow();
    juce::uint32 lastSend = 0;
    for (int sends = 0; msNow() - start < (juce::uint32) timeoutMs;)
    {
        if (shouldStop && shouldStop())
            return 0;
        if (sends < 3 && (sends == 0 || msNow() - lastSend > 600))   // UDP: ask again if nothing came back
        {
            for (auto& [host, p] : targets)
                s.write (host, p, request.getData(), (int) request.getSize());
            lastSend = msNow();
            ++sends;
        }
        if (s.waitUntilReady (true, 50) != 1)
            continue;
        juce::uint8 buf[600];
        juce::String from;
        int fromPort = 0, mappedPort = 0;
        juce::uint32 ip = 0;
        const int n = s.read (buf, (int) sizeof (buf), false, from, fromPort);
        if (n > 0 && parseStunResponse (buf, n, transaction, ip, mappedPort) && ip != 0)
            return ip;
    }
    return 0;
}

//==============================================================================
bool parseUrl (const juce::String& url, Url& out)
{
    auto s = url.trim();
    if (! s.startsWithIgnoreCase ("http://"))
        return false;
    s = s.substring (7);
    const auto hostPort = s.upToFirstOccurrenceOf ("/", false, false);
    out.path = s.containsChar ('/') ? s.fromFirstOccurrenceOf ("/", true, false) : juce::String ("/");
    out.host = hostPort.containsChar (':') ? hostPort.upToLastOccurrenceOf (":", false, false) : hostPort;
    out.port = hostPort.containsChar (':') ? hostPort.fromLastOccurrenceOf (":", false, false).getIntValue() : 80;
    return out.host.isNotEmpty() && out.port > 0 && out.port < 65536;
}

HttpResult http (const Url& u, const juce::String& method, const juce::String& extraHeaders, const juce::String& body, int timeoutMs)
{
    HttpResult result;
    juce::StreamingSocket s;
    if (! s.connect (u.host, u.port, juce::jmax (100, timeoutMs)))
        return result;

    juce::String request;
    request << method << " " << (u.path.isEmpty() ? juce::String ("/") : u.path) << " HTTP/1.0\r\n"
            << "Host: " << u.host << ":" << u.port << "\r\n"
            << "Connection: close\r\n" << extraHeaders;
    if (method == "POST" || body.isNotEmpty())
        request << "Content-Length: " << (int) body.getNumBytesAsUTF8() << "\r\n";
    request << "\r\n" << body;
    const int length = (int) request.getNumBytesAsUTF8();
    if (s.write (request.toRawUTF8(), length) != length)
        return result;

    juce::MemoryBlock got;
    const auto start = msNow();
    while (msNow() - start < (juce::uint32) timeoutMs && got.getSize() < (1u << 20) && ! complete (got))
    {
        const int ready = s.waitUntilReady (true, 50);
        if (ready < 0)
            break;
        if (ready == 0)
            continue;
        char buf[4096];
        const int n = s.read (buf, (int) sizeof (buf), false);
        if (n <= 0)
            break;   // the router closed the connection: that's the whole answer
        got.append (buf, (size_t) n);
    }

    const auto* d = (const char*) got.getData();
    const int size = (int) got.getSize();
    int headerEnd = -1;
    for (int i = 0; i + 3 < size && headerEnd < 0; ++i)
        if (d[i] == '\r' && d[i + 1] == '\n' && d[i + 2] == '\r' && d[i + 3] == '\n')
            headerEnd = i;
    if (headerEnd < 0)
        return result;

    const auto headers = juce::String::fromUTF8 (d, headerEnd);
    result.status = headers.upToFirstOccurrenceOf ("\r\n", false, false).fromFirstOccurrenceOf (" ", false, false).getIntValue();
    juce::MemoryBlock content (d + headerEnd + 4, (size_t) (size - headerEnd - 4));
    for (auto& line : juce::StringArray::fromLines (headers))
        if (line.startsWithIgnoreCase ("transfer-encoding:") && line.containsIgnoreCase ("chunked"))
            content = dechunk (content);
    result.body = juce::String::fromUTF8 ((const char*) content.getData(), (int) content.getSize());
    return result;
}

//==============================================================================
juce::StringArray discoverRouters (int timeoutMs, std::function<bool()> shouldStop)
{
    // one socket per home network address, so the search goes out on the router's network (not a VPN's)
    juce::OwnedArray<juce::DatagramSocket> sockets;
    auto addSocket = [&sockets] (juce::uint32 ip)
    {
        auto s = std::make_unique<juce::DatagramSocket> (false);
        if (! s->bindToPort (0, ip != 0 ? protocol::ipToString (ip) : juce::String()))
            return;
        if (ip != 0)
        {
            in_addr a {};
            a.s_addr = htonl (ip);
            setsockopt ((RawSocket) s->getRawSocketHandle(), IPPROTO_IP, IP_MULTICAST_IF, (const char*) &a, (int) sizeof (a));
        }
        sockets.add (s.release());
    };
    for (auto ip : localAddresses())
        if (isLan (ip))
            addSocket (ip);
    if (sockets.isEmpty())
        addSocket (0);

    const char* const targets[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                    "urn:schemas-upnp-org:service:WANIPConnection:1",
                                    "urn:schemas-upnp-org:service:WANPPPConnection:1" };
    juce::StringArray found;
    const auto start = msNow();
    for (int sends = 0; msNow() - start < (juce::uint32) timeoutMs;)
    {
        if (shouldStop && shouldStop())
            break;
        if (sends < 2 && msNow() - start >= (juce::uint32) (sends * 700))
        {
            for (auto* s : sockets)
                for (auto* t : targets)
                {
                    const juce::String m = juce::String ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                                         "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + t + "\r\n\r\n";
                    s->write ("239.255.255.250", 1900, m.toRawUTF8(), (int) m.getNumBytesAsUTF8());
                }
            ++sends;
        }
        for (auto* s : sockets)
            while (s->waitUntilReady (true, 0) == 1)
            {
                char buf[2048];
                juce::String from;
                int fromPort = 0;
                const int n = s->read (buf, (int) sizeof (buf), false, from, fromPort);
                if (n <= 0)
                    break;
                const auto location = ssdpLocation (juce::String::fromUTF8 (buf, n));
                if (location.isNotEmpty())
                    found.addIfNotAlreadyThere (location);
            }
        if (! found.isEmpty() && msNow() - start > 1000)   // routers answer at once; give a second one a moment
            break;
        juce::Thread::sleep (20);
    }
    return found;
}

juce::String ssdpLocation (const juce::String& ssdpAnswer)
{
    for (auto& line : juce::StringArray::fromLines (ssdpAnswer))
        if (line.startsWithIgnoreCase ("location:"))
            return line.fromFirstOccurrenceOf (":", false, false).trim();
    return {};
}

juce::Array<Gateway> gatewayServices (const juce::String& descriptionXml, const Url& description)
{
    juce::Array<Gateway> ipConnections, pppConnections;
    const auto doc = juce::XmlDocument::parse (descriptionXml);
    if (doc == nullptr)
        return {};
    Url base = description, urlBase;
    if (parseUrl (childText (*doc, "URLBase"), urlBase))
        base = urlBase;

    juce::Array<std::pair<juce::String, juce::String>> services;
    collectServices (*doc, services);
    for (auto& [type, control] : services)
    {
        if (control.isEmpty())
            continue;
        const Gateway g { resolveUrl (control, base), type };
        if (type.containsIgnoreCase ("WANIPConnection:"))
            ipConnections.add (g);
        else if (type.containsIgnoreCase ("WANPPPConnection:"))
            pppConnections.add (g);
    }
    ipConnections.addArray (pppConnections);
    return ipConnections;
}

juce::String soapEnvelope (const juce::String& serviceType, const juce::String& action, const juce::StringPairArray& args)
{
    juce::String e;
    e << "<?xml version=\"1.0\"?>\r\n"
      << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      << "<s:Body><u:" << action << " xmlns:u=\"" << xmlEscape (serviceType) << "\">";
    for (int i = 0; i < args.size(); ++i)
        e << "<" << args.getAllKeys()[i] << ">" << xmlEscape (args.getAllValues()[i]) << "</" << args.getAllKeys()[i] << ">";
    e << "</u:" << action << "></s:Body></s:Envelope>\r\n";
    return e;
}

HttpResult soap (const Gateway& g, const juce::String& action, const juce::StringPairArray& args, int timeoutMs)
{
    const juce::String headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + g.serviceType + "#" + action + "\"\r\n";
    return http (g.control, "POST", headers, soapEnvelope (g.serviceType, action, args), timeoutMs);
}

int soapError (const juce::String& body)
{
    const int i = body.indexOfIgnoreCase ("errorCode>");
    return i < 0 ? 0 : body.substring (i + 10).upToFirstOccurrenceOf ("<", false, false).trim().getIntValue();
}

juce::String soapValue (const juce::String& body, const juce::String& tag)
{
    const int i = body.indexOf (tag + ">");
    return i < 0 ? juce::String() : body.substring (i + tag.length() + 1).upToFirstOccurrenceOf ("<", false, false).trim();
}

//==============================================================================
juce::Array<juce::uint32> localAddresses()
{
    juce::Array<juce::uint32> out;
    for (auto& a : juce::IPAddress::getAllAddresses (false))
    {
        const auto ip = protocol::parseIPv4 (a.toString());   // Windows also lists multicast groups: skip 224+
        if (ip != 0 && (ip >> 24) != 127 && (ip >> 24) < 224 && (ip >> 16) != 0xa9fe)
            out.addIfNotAlreadyThere (ip);
    }
    return out;
}

bool isPrivate (juce::uint32 ip)
{
    const auto a = ip >> 24, b = (ip >> 16) & 255;
    return ip == 0 || isLan (ip) || a == 127 || (a == 100 && b >= 64 && b < 128) || (a == 169 && b == 254);
}

ReachInfo verdict (int port, juce::uint32 stunIp, juce::uint32 routerIp, juce::uint32 lanIp,
                   bool routerFound, bool mapped, int mapError, bool ownAddress)
{
    ReachInfo r;
    r.port = port;
    r.lanIp = lanIp;
    r.routerIp = routerIp;
    r.publicIp = stunIp != 0 ? stunIp : (routerIp != 0 && ! isPrivate (routerIp) ? routerIp : 0);
    const juce::String p (port);
    const juce::String firewall = " Windows may ask whether Jam Sesh can use the network: allow it.";

    if (ownAddress)
    {
        r.state = ReachState::open;
        r.text = "Friends over the internet can join: this PC has its own internet address." + firewall;
    }
    else if (routerIp != 0 && (isPrivate (routerIp) || (stunIp != 0 && routerIp != stunIp)))
    {
        r.state = ReachState::shared;
        r.text = "Friends over the internet can't reach you as the host: your router has no internet address of its own "
                 "(your internet provider shares one between customers, or a second router sits in front of yours). "
                 "Let a friend host, or use a VPN such as Tailscale.";
    }
    else if (mapped)
    {
        r.state = ReachState::open;
        r.text = "Friends over the internet can join: your router opened UDP port " + p + " for Jam Sesh." + firewall;
    }
    else if (r.publicIp != 0)
    {
        const juce::String pc = lanIp != 0 ? " (" + protocol::ipToString (lanIp) + ")" : juce::String();
        r.state = ReachState::forwardPort;
        if (mapError == 718)   // ConflictInMappingEntry
            r.text = "Your router's UDP port " + p + " already goes to another device. Host on another port, or forward "
                     "this one to this PC" + pc + " in your router's settings.";
        else
            r.text = juce::String (routerFound ? "Your router didn't open the port by itself."
                                               : "Your router didn't answer (its UPnP setting is off or missing).")
                   + " To let friends in over the internet, forward UDP port " + p + " to this PC" + pc
                   + " in your router's settings, or use a VPN such as Tailscale. Your internet address is in the code.";
    }
    else
    {
        r.state = ReachState::offline;
        r.text = "Couldn't find your internet address, so only friends on your network or VPN can join.";
    }
    return r;
}
}

//==============================================================================
struct Reach::Impl : private juce::Thread
{
    explicit Impl (Config c) : juce::Thread ("Jam Sesh reach"), config (std::move (c)) {}
    ~Impl() override { stop(); }

    Config config;
    mutable juce::CriticalSection lock;
    ReachInfo current;
    std::function<void()> onChange;
    int port = 0;

    void publish (const ReachInfo& r)
    {
        std::function<void()> callback;
        {
            const juce::ScopedLock sl (lock);
            current = r;
            callback = onChange;
        }
        if (callback)
            callback();
    }

    void start (int p, std::function<void()> callback)
    {
        stop();
        {
            const juce::ScopedLock sl (lock);
            port = p;
            onChange = std::move (callback);
            current = {};
            current.state = ReachState::checking;
            current.port = p;
            current.text = "Checking whether friends over the internet can reach you... Copy the code when this is done.";
        }
        startThread (juce::Thread::Priority::low);
    }

    void stop()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            notify();
            stopThread (6000);
        }
        const juce::ScopedLock sl (lock);
        current = {};
        onChange = nullptr;
    }

    void run() override
    {
        using namespace reach;
        int p = 0;
        {
            const juce::ScopedLock sl (lock);
            p = port;
        }
        auto stopping = [this] { return threadShouldExit(); };
        const auto locals = localAddresses();

        // 1. the internet address, for the code (quick)
        const auto stunIp = askStun (config.stunServers, 2500, stopping);
        if (threadShouldExit())
            return;
        if (stunIp != 0)
        {
            ReachInfo partial;
            {
                const juce::ScopedLock sl (lock);
                partial = current;
            }
            partial.publicIp = stunIp;
            publish (partial);
        }

        // 2. the router: forward the port to this PC
        juce::StringArray descriptions;
        if (config.routerDescription.isNotEmpty())
            descriptions.add (config.routerDescription);
        else
            descriptions = discoverRouters (config.discoverMs, stopping);

        bool routerFound = false, mapped = false;
        int mapError = 0, lease = config.leaseSeconds;
        juce::uint32 routerIp = 0, lanIp = 0;
        Gateway gateway;
        for (auto& d : descriptions)
        {
            Url description;
            if (mapped || threadShouldExit() || ! parseUrl (d, description))
                continue;
            const auto desc = http (description, "GET", {}, {}, 2000);
            if (desc.status != 200)
                continue;
            for (auto& g : gatewayServices (desc.body, description))
            {
                if (mapped || threadShouldExit())
                    break;
                const auto ext = soap (g, "GetExternalIPAddress", juce::StringPairArray(), 2000);
                if (ext.status == 0)
                    continue;   // no answer at all
                routerFound = true;
                if (const auto ip = protocol::parseIPv4 (soapValue (ext.body, "NewExternalIPAddress")))
                    routerIp = ip;
                lanIp = closestTo (locals, resolve (g.control.host));
                int e = addMapping (g, p, lanIp, lease);
                if (e == 725 && lease != 0)   // OnlyPermanentLeasesSupported
                {
                    lease = 0;
                    e = addMapping (g, p, lanIp, lease);
                }
                if (e == 0)
                {
                    mapped = true;
                    gateway = g;
                }
                else if (mapError <= 0)   // keep the router's own reason over "no answer"
                {
                    mapError = e;
                }
            }
        }
        if (lanIp == 0)
            for (auto ip : locals)
                if (isLan (ip))
                {
                    lanIp = ip;
                    break;
                }
        publish (verdict (p, stunIp, routerIp, lanIp, routerFound, mapped, mapError, stunIp != 0 && locals.contains (stunIp)));

        // 3. keep the mapping while hosting (renewed at half the lease)
        auto renewAt = msNow() + (juce::uint32) lease * 500u;
        while (! threadShouldExit())
        {
            wait (1000);
            if (mapped && lease > 0 && (juce::int32) (msNow() - renewAt) > 0)
            {
                addMapping (gateway, p, lanIp, lease);
                renewAt = msNow() + (juce::uint32) lease * 500u;
            }
        }

        // 4. hosting stopped
        if (mapped)
            deleteMapping (gateway, p);
    }
};

Reach::Reach() : Reach (Config {}) {}
Reach::Reach (Config c) : impl (std::make_unique<Impl> (std::move (c))) {}
Reach::~Reach() { impl->stop(); }

void Reach::start (int port, std::function<void()> onChange) { impl->start (port, std::move (onChange)); }
void Reach::stop() { impl->stop(); }

ReachInfo Reach::info() const
{
    const juce::ScopedLock sl (impl->lock);
    return impl->current;
}
}
