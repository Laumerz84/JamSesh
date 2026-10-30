#include "JamReplica.h"
#include "JamSession.h"
#include "../PluginProcessor.h"
#include <map>

// Owner: net soldier.
//
// One hidden ClodScopeProcessor (jamReplica = true) per raw-input friend, made on the message thread
// when their first full state arrives. The audio thread finds it through a fixed slot table (atomic id
// + atomic pointer + a busy count); a replaced or removed replica is only deleted on the message
// thread once no audio callback is inside it.
namespace jam
{
    struct Replicas::Impl : private juce::Timer
    {
        explicit Impl (Session& s) : session (s) {}

        ~Impl() override
        {
            stopTimer();
            for (auto& s : slots)
            {
                s.id.store (noPeer);
                s.proc.store (nullptr);
            }
            for (auto& s : slots)   // nobody should be processing by now; don't free under a running callback
                for (int i = 0; i < 200 && s.busy.load() != 0; ++i)
                    juce::Thread::sleep (1);
        }

        Session& session;
        double rate = 48000.0;
        int block = 512;

        struct Slot
        {
            std::atomic<PeerId> id { noPeer };
            std::atomic<ClodScopeProcessor*> proc { nullptr };
            std::atomic<int> busy { 0 };
        };
        Slot slots[maxPlayers];

        // message thread
        struct Friend
        {
            std::unique_ptr<ClodScopeProcessor> proc;
            juce::MemoryBlock state;
            std::map<juce::String, float> params;   // knobs moved since that state
            int slot = -1;
        };
        std::map<PeerId, Friend> friends;
        struct Dead { std::unique_ptr<ClodScopeProcessor> proc; int slot; };
        std::vector<Dead> graveyard;

        std::unique_ptr<ClodScopeProcessor> make (const Friend& f) const
        {
            auto p = std::make_unique<ClodScopeProcessor> (true);
            p->setPlayConfigDetails (2, 2, rate, block);
            p->prepareToPlay (rate, block);
            if (f.state.getSize() > 0)
                p->setStateInformation (f.state.getData(), (int) f.state.getSize());
            for (auto& [id, v] : f.params)
                apply (*p, id, v);
            return p;
        }

        static void apply (ClodScopeProcessor& p, const juce::String& id, float value)
        {
            if (auto* param = p.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (value));
        }

        // publish a new replica for this friend; the old one dies later
        void install (PeerId id, Friend& f, std::unique_ptr<ClodScopeProcessor> fresh)
        {
            if (f.slot < 0)
                for (int i = 0; i < maxPlayers; ++i)
                    if (slots[i].id.load() == noPeer && slots[i].proc.load() == nullptr)
                    {
                        f.slot = i;
                        break;
                    }
            if (f.slot < 0)
                return;

            auto& s = slots[f.slot];
            s.proc.store (fresh.get());
            s.id.store (fresh != nullptr ? id : noPeer);
            retire (std::move (f.proc), f.slot);
            f.proc = std::move (fresh);
        }

        void retire (std::unique_ptr<ClodScopeProcessor> old, int slot)
        {
            if (old == nullptr)
                return;
            graveyard.push_back ({ std::move (old), slot });
            startTimer (50);
        }

        void timerCallback() override
        {
            graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                             [this] (const Dead& d) { return slots[d.slot].busy.load() == 0; }),
                             graveyard.end());
            if (graveyard.empty())
                stopTimer();
        }
    };

    Replicas::Replicas (Session& s) : impl (std::make_unique<Impl> (s)) {}
    Replicas::~Replicas() = default;

    void Replicas::prepare (double sampleRate, int maxBlockSize)
    {
        auto& m = *impl;
        if (sampleRate <= 0 || maxBlockSize <= 0 || (sampleRate == m.rate && maxBlockSize == m.block))
            return;
        m.rate = sampleRate;
        m.block = maxBlockSize;
        for (auto& [id, f] : m.friends)   // never re-prepare one the audio thread may be using: swap in a new one
            if (f.proc != nullptr)
                m.install (id, f, m.make (f));
    }

    void Replicas::stateArrived (PeerId id, const juce::MemoryBlock& pluginState)
    {
        auto& m = *impl;
        if (id == noPeer || pluginState.isEmpty())
            return;
        auto& f = m.friends[id];
        f.state = pluginState;
        f.params.clear();
        if (f.proc != nullptr)
            f.proc->setStateInformation (pluginState.getData(), (int) pluginState.getSize());   // as any host does mid-play
        else
            m.install (id, f, m.make (f));
    }

    void Replicas::parameterArrived (PeerId id, const juce::String& paramId, float value)
    {
        auto& m = *impl;
        auto it = m.friends.find (id);
        if (it == m.friends.end())
            return;   // no state yet: the full state that comes first carries it
        it->second.params[paramId] = value;
        if (it->second.proc != nullptr)
            Impl::apply (*it->second.proc, paramId, value);
    }

    void Replicas::removePeer (PeerId id)
    {
        auto& m = *impl;
        auto it = m.friends.find (id);
        if (it == m.friends.end())
            return;
        auto& f = it->second;
        if (f.slot >= 0)
        {
            m.slots[f.slot].id.store (noPeer);
            m.slots[f.slot].proc.store (nullptr);
            m.retire (std::move (f.proc), f.slot);
        }
        m.friends.erase (it);
    }

    bool Replicas::process (PeerId id, juce::AudioBuffer<float>& io, int numSamples, juce::MidiBuffer& midi) noexcept
    {
        auto& m = *impl;
        if (id == noPeer || numSamples <= 0 || numSamples > m.block || io.getNumChannels() < 2 || numSamples > io.getNumSamples())
            return false;

        for (auto& s : m.slots)
        {
            if (s.id.load (std::memory_order_acquire) != id)
                continue;
            s.busy.fetch_add (1);
            bool done = false;
            auto* p = s.proc.load();
            if (p != nullptr && s.id.load() == id)
            {
                juce::AudioBuffer<float> view (io.getArrayOfWritePointers(), 2, numSamples);   // no allocation (<= 32 channels)
                p->processBlock (view, midi);
                done = true;
            }
            s.busy.fetch_sub (1);
            return done;
        }
        return false;
    }
}
