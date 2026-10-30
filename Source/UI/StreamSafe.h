#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Capture-safe drawing. JUCE 8 draws every window through Direct2D, which hands its frames to a DXGI
// swap chain. Screen sharing the host's window (Discord, OBS window capture) then picks up only that
// GPU surface: the stream shows just the plugin, glitching, instead of the whole FL Studio window.
// The software renderer paints with GDI like the host does, so the plugin is captured as part of the
// host's window. The 3D views keep the rule too: they render off-screen and are painted as an image
// (Render3D/OffscreenGL.h).
namespace streamsafe
{
    // Puts one window on the software renderer; cheap when it already is. Call it before the window's
    // first paint where possible (ClodScopeEditor::parentHierarchyChanged, preparePopupMenuWindow).
    inline void useSoftwareRenderer (juce::ComponentPeer& peer)
    {
       #if JUCE_WINDOWS
        static const int software = peer.getAvailableRenderingEngines().indexOf ("Software Renderer");
        if (software >= 0 && peer.getCurrentRenderingEngine() != software)
        {
            peer.setCurrentRenderingEngine (software);
            peer.getComponent().repaint();
        }
       #else
        juce::ignoreUnused (peer);
       #endif
    }

    inline void useSoftwareRenderer (juce::Component& c)
    {
        if (auto* peer = c.getPeer())
            useSoftwareRenderer (*peer);
    }

    // Every window this plugin has open (editors of all instances, menus, call-outs): a catch-all for
    // windows that JUCE opens on its own.
    inline void useSoftwareRendererEverywhere()
    {
        for (int i = juce::ComponentPeer::getNumPeers(); --i >= 0;)
            if (auto* peer = juce::ComponentPeer::getPeer (i))
                useSoftwareRenderer (*peer);
    }
}
