#include "PresetUI.h"
#include "PresetWidgets.h"

using namespace presetui;

namespace
{
    // Each category gets a tint so the list reads at a glance
    juce::Colour categoryColour (const juce::String& category)
    {
        const juce::Colour tints[] = { Palette::green, Palette::blue, Palette::violet, Palette::amber, Palette::pink };
        const int i = PresetManager::categories().indexOf (category);
        return i < 0 ? Palette::dim : tints[i % 5];
    }

    using HoverList = std::vector<std::unique_ptr<widgets_detail::HoverReporter>>;

    void addHover (HoverList& list, juce::Component& c, const juce::String& id, HelpTarget* help)
    {
        list.push_back (std::make_unique<widgets_detail::HoverReporter> (c, id, help));
    }
}

//==============================================================================
// PresetBar
//==============================================================================
struct PresetBar::Impl : public PresetManager::Listener
{
    // The clickable name field
    struct NameBox : public juce::Component
    {
        explicit NameBox (Impl& o) : owner (o) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

        void paint (juce::Graphics& g) override
        {
            auto& m = owner.manager;
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            const bool over = isMouseOver (true);
            g.setColour (Palette::viewBg);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (over ? colours::select.withAlpha (0.7f) : Palette::panelEdge.brighter (0.1f));
            g.drawRoundedRectangle (r, 5.0f, 1.0f);

            auto area = getLocalBounds().reduced (8, 0);
            drawChevron (g, area.removeFromRight (14).toFloat(), 2, over ? Palette::text : Palette::dim, 1.4f);
            area.removeFromRight (4);

            const bool modified = m.isModified();
            if (modified)
            {
                auto dot = area.removeFromLeft (10).toFloat();
                g.setColour (colours::modified);
                g.fillEllipse (dot.withSizeKeepingCentre (6.0f, 6.0f));
                area.removeFromLeft (2);
            }

            auto category = m.getCurrentCategory();
            if (m.isCurrentUserPreset())
                category = "User - " + category;
            const auto catFont = Theme::font (10.5f);
            const int catW = juce::jmin ((int) std::ceil (textWidth (catFont, category)) + 6, area.getWidth() / 3);
            if (category.isNotEmpty() && catW > 24)
            {
                g.setColour (Palette::dim);
                g.setFont (catFont);
                g.drawText (category, area.removeFromRight (catW), juce::Justification::centredRight, true);
            }

            g.setColour (Palette::text);
            g.setFont (Theme::font (13.0f, true));
            g.drawText (m.getCurrentName() + (modified ? " *" : ""), area, juce::Justification::centredLeft, true);
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()) && owner.owner.onBrowse != nullptr)
                owner.owner.onBrowse();
        }

        Impl& owner;
    };

    Impl (PresetBar& o, PresetManager& m, HelpTarget* h)
        : owner (o), manager (m), help (h), nameBox (*this)
    {
        prevButton.onClick = [this] { manager.loadPrevious(); };
        nextButton.onClick = [this] { manager.loadNext(); };
        favButton.onClick = [this] { manager.toggleFavourite (manager.getCurrentKey()); };
        saveButton.onClick = [this] { openSaveDialog (SaveDialog::Mode::save); };
        menuButton.onClick = [this] { showMenu(); };

        for (juce::Component* c : { (juce::Component*) &prevButton, (juce::Component*) &nextButton,
                                    (juce::Component*) &nameBox, (juce::Component*) &favButton,
                                    (juce::Component*) &saveButton, (juce::Component*) &menuButton })
            owner.addAndMakeVisible (c);

        addHover (hovers, prevButton, "preset.prev", help);
        addHover (hovers, nextButton, "preset.next", help);
        addHover (hovers, nameBox, "preset.name", help);
        addHover (hovers, favButton, "preset.fav", help);
        addHover (hovers, saveButton, "preset.save", help);
        addHover (hovers, menuButton, "preset.menu", help);

        manager.addListener (this);
        update();
    }

    ~Impl() override
    {
        manager.removeListener (this);
    }

    void presetChanged() override     { update(); }
    void presetListChanged() override { update(); }

    void update()
    {
        favButton.setToggleState (manager.isFavourite (manager.getCurrentKey()), juce::dontSendNotification);
        favButton.repaint();
        nameBox.repaint();
    }

    // Dialogs cover the editor (so they follow its scaling); the top-level window as a fallback
    juce::Component& dialogHost()
    {
        if (auto* ed = owner.findParentComponentOfClass<juce::AudioProcessorEditor>())
            return *ed;
        return *owner.getTopLevelComponent();
    }

    void openSaveDialog (SaveDialog::Mode mode)
    {
        auto* d = modal.open (dialogHost(), std::make_unique<SaveDialog> (manager, mode, [this] { modal.close(); }));
        d->focusName();
    }

    void confirmDelete()
    {
        const auto key = manager.getCurrentKey();
        auto* e = manager.findEntry (key);
        if (e == nullptr || e->factory)
            return;
        modal.open (dialogHost(), std::make_unique<ConfirmDialog> (
            "Delete \"" + e->name + "\"?",
            "The preset file is moved to the recycle bin. The sound you hear now stays as it is.",
            "Delete",
            [this, key] { manager.deleteUserPreset (key); },
            [this] { modal.close(); }));
    }

    void loadFromFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a Jam Sesh preset", manager.getPresetsFolder(), "*.clodpreset");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto f = fc.getResult();
                                  if (f.existsAsFile())
                                      manager.loadFile (f);
                              });
    }

    void showMenu()
    {
        const bool user = manager.isCurrentUserPreset();
        const bool canRevert = manager.isModified() && manager.currentEntry() != nullptr;

        juce::PopupMenu m;
        m.setLookAndFeel (&owner.getLookAndFeel());   // the plugin's look, which also keeps the menu stream-safe
        m.addSectionHeader ("Preset");
        m.addItem ("Save As...", [this] { openSaveDialog (SaveDialog::Mode::saveAs); });
        m.addItem ("Rename...", user, false, [this] { openSaveDialog (SaveDialog::Mode::rename); });
        m.addItem ("Delete...", user, false, [this] { confirmDelete(); });
        m.addSeparator();
        m.addItem ("Revert to saved", canRevert, false, [this] { manager.revert(); });
        m.addItem ("Init (all sound controls to default)", [this] { manager.loadInit(); });
        m.addSeparator();
        m.addItem ("Browse presets...", [this] { if (owner.onBrowse != nullptr) owner.onBrowse(); });
        m.addItem ("Load preset file...", [this] { loadFromFile(); });
        m.addItem ("Open presets folder", [this] { manager.revealPresetsFolder(); });
        m.addItem ("Refresh preset list", [this] { manager.rescanUserPresets(); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton).withMinimumWidth (220));
    }

    void layout()
    {
        auto r = owner.getLocalBounds().reduced (0, 1);
        const int h = r.getHeight();
        prevButton.setBounds (r.removeFromLeft (h));
        r.removeFromLeft (3);
        nextButton.setBounds (r.removeFromLeft (h));
        r.removeFromLeft (6);
        menuButton.setBounds (r.removeFromRight (h));
        r.removeFromRight (4);
        saveButton.setBounds (r.removeFromRight (54));
        r.removeFromRight (4);
        favButton.setBounds (r.removeFromRight (h));
        r.removeFromRight (4);
        nameBox.setBounds (r);
    }

    PresetBar& owner;
    PresetManager& manager;
    HelpTarget* help;

    IconButton prevButton { IconButton::prev }, nextButton { IconButton::next };
    IconButton favButton { IconButton::star }, menuButton { IconButton::menu };
    FlatButton saveButton { "Save" };
    NameBox nameBox;
    HoverList hovers;
    ModalHost modal;
    std::unique_ptr<juce::FileChooser> chooser;
};

PresetBar::PresetBar (PresetManager& m, HelpTarget* h) : impl (std::make_unique<Impl> (*this, m, h)) {}
PresetBar::~PresetBar() = default;
void PresetBar::paint (juce::Graphics&) {}
void PresetBar::resized() { impl->layout(); }
int PresetBar::idealWidth() const { return 440; }

//==============================================================================
// PresetBrowser
//==============================================================================
namespace
{
    class CategoryList : public SmoothList
    {
    public:
        explicit CategoryList (PresetManager& m) : SmoothList (28), manager (m)
        {
            groups.add (PresetManager::allGroup());
            groups.add (PresetManager::favouritesGroup());
            groups.add (PresetManager::userGroup());
            groups.addArray (PresetManager::categories());
            counts.resize ((size_t) groups.size());
        }

        void updateCounts()
        {
            for (int i = 0; i < groups.size(); ++i)
                counts[(size_t) i] = manager.countInGroup (groups[i]);
            repaint();
        }

        int getNumRows() const override { return groups.size(); }

        void paintRow (juce::Graphics& g, int row, juce::Rectangle<int> b, bool hovered) override
        {
            const auto name = groups[row];
            const bool sel = name == selected;
            auto r = b.toFloat().reduced (4.0f, 1.5f);

            if (row == firstCategoryRow)
            {
                g.setColour (Palette::panelEdge);
                g.fillRect (r.getX() + 6.0f, (float) b.getY(), r.getWidth() - 12.0f, 1.0f);
            }

            if (sel || hovered)
            {
                g.setColour (sel ? colours::select.withAlpha (0.18f) : Palette::bright.withAlpha (0.035f));
                g.fillRoundedRectangle (r, 5.0f);
            }
            if (sel)
            {
                g.setColour (colours::select);
                g.fillRoundedRectangle (r.getX(), r.getY() + 5.0f, 2.5f, r.getHeight() - 10.0f, 1.2f);
            }

            auto icon = juce::Rectangle<float> (r.getX() + 10.0f, r.getCentreY() - 7.0f, 14.0f, 14.0f);
            if (name == PresetManager::favouritesGroup())
                drawStar (g, icon, true, colours::star);
            else if (name == PresetManager::userGroup())
            {
                g.setColour (Palette::violet);
                g.fillEllipse (icon.getCentreX() - 3.0f, icon.getY() + 1.5f, 6.0f, 6.0f);
                g.fillRoundedRectangle (icon.getCentreX() - 5.5f, icon.getY() + 8.5f, 11.0f, 5.0f, 2.5f);
            }
            else if (name == PresetManager::allGroup())
            {
                g.setColour (Palette::text.withAlpha (0.7f));
                for (int i = 0; i < 4; ++i)
                    g.fillRoundedRectangle (icon.getX() + 1.5f + (float) (i % 2) * 6.0f, icon.getY() + 1.5f + (float) (i / 2) * 6.0f, 4.5f, 4.5f, 1.0f);
            }
            else
            {
                g.setColour (categoryColour (name));
                g.fillEllipse (icon.withSizeKeepingCentre (7.0f, 7.0f));
            }

            const int count = counts[(size_t) row];
            auto text = r.withTrimmedLeft (32.0f).withTrimmedRight (8.0f);
            g.setFont (Theme::font (11.0f));
            g.setColour (Palette::dim.withAlpha (count > 0 ? 1.0f : 0.5f));
            g.drawText (juce::String (count), text.removeFromRight (30.0f), juce::Justification::centredRight, false);

            g.setFont (Theme::font (12.5f, sel));
            g.setColour (sel ? Palette::text : (count > 0 ? Palette::text.withAlpha (0.85f) : Palette::dim));
            g.drawText (name, text, juce::Justification::centredLeft, true);
        }

        void rowMouseDown (int row, const juce::MouseEvent&, juce::Rectangle<int>) override
        {
            select (groups[row]);
        }

        void select (const juce::String& group)
        {
            selected = groups.contains (group) ? group : PresetManager::allGroup();
            repaint();
            if (onChange != nullptr)
                onChange();
        }

        void setSelectedQuietly (const juce::String& group)
        {
            selected = groups.contains (group) ? group : PresetManager::allGroup();
            scrollToRow (groups.indexOf (selected), false);
            repaint();
        }

        juce::String selected { PresetManager::allGroup() };
        std::function<void()> onChange;

    private:
        static constexpr int firstCategoryRow = 3;
        PresetManager& manager;
        juce::StringArray groups;
        std::vector<int> counts;
    };

    //==============================================================================
    class GenreBar : public juce::Component
    {
    public:
        GenreBar()
        {
            for (auto& g : PresetManager::genres())
            {
                auto* c = chips.add (new Chip (g));
                c->onClick = [this] { changed(); };
                addAndMakeVisible (c);
            }
            clearButton.onClick = [this]
            {
                for (auto* c : chips)
                    c->setToggleState (false, juce::dontSendNotification);
                changed();
            };
            addChildComponent (clearButton);
        }

        int heightFor (int width) const { return layout (width, false); }
        void resized() override { layout (getWidth(), true); }

        juce::StringArray selected() const
        {
            juce::StringArray s;
            for (auto* c : chips)
                if (c->getToggleState())
                    s.add (c->getButtonText());
            return s;
        }

        void setSelected (const juce::StringArray& s)
        {
            for (auto* c : chips)
                c->setToggleState (s.contains (c->getButtonText()), juce::dontSendNotification);
            clearButton.setVisible (! s.isEmpty());
        }

        std::function<void()> onChange;

    private:
        int layout (int width, bool apply) const
        {
            constexpr int h = 24, gap = 6;
            int x = 0, y = 0;
            for (auto* c : chips)
            {
                const int w = c->idealWidth();
                if (x > 0 && x + w > width)
                {
                    x = 0;
                    y += h + gap;
                }
                if (apply)
                    c->setBounds (x, y, w, h);
                x += w + gap;
            }
            if (x + 54 > width)
            {
                x = 0;
                y += h + gap;
            }
            if (apply)
                const_cast<FlatButton&> (clearButton).setBounds (x, y, 54, h);
            return y + h;
        }

        void changed()
        {
            clearButton.setVisible (! selected().isEmpty());
            if (onChange != nullptr)
                onChange();
        }

        juce::OwnedArray<Chip> chips;
        FlatButton clearButton { "Clear", FlatButton::ghost };
    };

    //==============================================================================
    class PresetList : public SmoothList
    {
    public:
        PresetList (PresetManager& m, const std::vector<int>& rows) : SmoothList (50), manager (m), filtered (rows) {}

        int getNumRows() const override { return (int) filtered.size(); }

        const PresetManager::Entry* entryAt (int row) const
        {
            if (row < 0 || row >= getNumRows())
                return nullptr;
            const int i = filtered[(size_t) row];
            const auto& all = manager.entries();
            return i >= 0 && i < (int) all.size() ? &all[(size_t) i] : nullptr;
        }

        static juce::Rectangle<float> starArea (juce::Rectangle<int> b)
        {
            return { (float) b.getX() + 12.0f, (float) b.getCentreY() - 8.0f, 16.0f, 16.0f };
        }

        void paintRow (juce::Graphics& g, int row, juce::Rectangle<int> b, bool hovered) override
        {
            auto* e = entryAt (row);
            if (e == nullptr)
                return;

            const bool sel = row == selectedRow;
            const bool cur = e->key == manager.getCurrentKey();
            const bool fav = manager.isFavourite (e->key);
            auto r = b.toFloat().reduced (4.0f, 2.0f);

            if (sel || hovered)
            {
                g.setColour (sel ? colours::select.withAlpha (0.17f) : Palette::bright.withAlpha (0.035f));
                g.fillRoundedRectangle (r, 6.0f);
            }
            if (sel)
            {
                g.setColour (colours::select.withAlpha (0.55f));
                g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
            }
            if (cur)
            {
                g.setColour (colours::loaded);
                g.fillRoundedRectangle (r.getX(), r.getY() + 6.0f, 3.0f, r.getHeight() - 12.0f, 1.5f);
            }

            const auto starR = starArea (b);
            const bool starHover = hovered && starR.expanded (4.0f).contains (getMouseXYRelative().toFloat());
            drawStar (g, starR, fav, fav ? colours::star : Palette::dim.withAlpha (starHover ? 1.0f : (hovered || sel ? 0.6f : 0.3f)));

            const float x = r.getX() + 38.0f;
            const float right = r.getRight() - 10.0f;
            const auto top = juce::Rectangle<float> (x, r.getY() + 6.0f, right - x, 18.0f);
            const auto bottom = juce::Rectangle<float> (x, r.getY() + 25.0f, right - x, 16.0f);

            // line 1: name ... genre pills
            const float maxPills = top.getWidth() * 0.45f;
            const float pillsStart = drawPillRow (g, top.withTrimmedLeft (top.getWidth() - maxPills).reduced (0.0f, 1.0f),
                                                  e->genres, Palette::dim, true);
            g.setFont (Theme::font (13.5f, true));
            g.setColour (cur ? colours::loaded : Palette::text);
            auto nameText = e->name;
            if (cur && manager.isModified())
                nameText << " *";
            g.drawText (nameText, top.withRight (pillsStart - 6.0f), juce::Justification::centredLeft, true);

            // line 2: CATEGORY  description ... USER
            auto line = bottom;
            if (! e->factory)
            {
                const auto ur = line.removeFromRight (40.0f).reduced (0.0f, 1.0f);
                drawPill (g, ur, "USER", Palette::violet, false, 9.0f);
                line.removeFromRight (6.0f);
            }
            const auto catFont = Theme::font (10.0f, true);
            const auto catText = e->category.toUpperCase();
            const float catW = juce::jmin (line.getWidth() * 0.4f, textWidth (catFont, catText) + 2.0f);
            g.setFont (catFont);
            g.setColour (categoryColour (e->category).withAlpha (0.9f));
            g.drawText (catText, line.removeFromLeft (catW), juce::Justification::centredLeft, true);
            line.removeFromLeft (8.0f);
            g.setFont (Theme::font (11.5f));
            g.setColour (Palette::dim);
            g.drawText (e->description, line, juce::Justification::centredLeft, true);
        }

        void paintEmpty (juce::Graphics& g) override
        {
            g.setColour (Palette::dim);
            g.setFont (Theme::font (14.0f, true));
            auto r = getLocalBounds().withSizeKeepingCentre (getWidth() - 20, 60);
            g.drawText ("No presets match", r.removeFromTop (24), juce::Justification::centred, true);
            g.setFont (Theme::font (12.0f));
            g.drawText ("Clear the search or genre tags, or pick \"All\".", r, juce::Justification::centred, true);
        }

        void rowMouseDown (int row, const juce::MouseEvent& e, juce::Rectangle<int> b) override
        {
            if (starArea (b).expanded (5.0f).contains (e.position))
            {
                if (auto* entry = entryAt (row))
                    manager.toggleFavourite (entry->key);
                return;
            }
            if (onSelect != nullptr)
                onSelect (row);
        }

        void rowDoubleClicked (int row) override
        {
            if (onLoad != nullptr)
                onLoad (row);
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            SmoothList::mouseMove (e);
            repaint();   // star hover
        }

        int selectedRow = -1;
        std::function<void (int)> onSelect, onLoad;

    private:
        PresetManager& manager;
        const std::vector<int>& filtered;
    };

    //==============================================================================
    class DetailsPane : public juce::Component
    {
    public:
        explicit DetailsPane (PresetManager& m) : manager (m)
        {
            addAndMakeVisible (loadButton);
            addAndMakeVisible (favButton);
            addChildComponent (deleteButton);
            favButton.onClick = [this] { if (key.isNotEmpty()) manager.toggleFavourite (key); };
        }

        void setKey (const juce::String& k)
        {
            key = k;
            update();
        }

        void update()
        {
            auto* e = manager.findEntry (key);
            loadButton.setEnabled (e != nullptr);
            favButton.setEnabled (e != nullptr);
            deleteButton.setVisible (e != nullptr && ! e->factory);
            const bool fav = e != nullptr && manager.isFavourite (key);
            favButton.setButtonText (fav ? "Favourite" : "Add to favourites");
            favButton.setToggleState (fav, juce::dontSendNotification);
            const bool cur = e != nullptr && key == manager.getCurrentKey();
            loadButton.setButtonText (cur ? (manager.isModified() ? "Reload (discard edits)" : "Reload") : "Load");
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto* e = manager.findEntry (key);
            auto r = getLocalBounds().reduced (16, 14);
            if (e == nullptr)
            {
                g.setColour (Palette::dim);
                g.setFont (Theme::font (12.5f));
                g.drawFittedText ("Select a preset to see what it does.", r, juce::Justification::centred, 3, 1.0f);
                return;
            }

            r.removeFromBottom (buttonsHeight);
            const bool cur = key == manager.getCurrentKey();

            // kind + state badges
            auto badgeRow = r.removeFromTop (18).toFloat();
            g.setFont (Theme::font (10.0f, true));
            g.setColour (Palette::dim);
            const juce::String kind = e->factory ? "FACTORY PRESET" : "USER PRESET";
            g.drawText (kind, badgeRow, juce::Justification::centredLeft, false);
            float bx = badgeRow.getX() + textWidth (Theme::font (10.0f, true), kind) + 8.0f;
            if (cur)
            {
                const auto t = juce::String (manager.isModified() ? "LOADED, EDITED" : "LOADED");
                const float w = pillWidth (t, 9.0f);
                drawPill (g, { bx, badgeRow.getY() + 1.0f, w, 16.0f }, t, manager.isModified() ? colours::modified : colours::loaded, true, 9.0f);
            }
            r.removeFromTop (6);

            g.setColour (Palette::text);
            g.setFont (Theme::font (18.0f, true));
            g.drawFittedText (e->name, r.removeFromTop (48), juce::Justification::topLeft, 2, 0.9f);

            g.setColour (categoryColour (e->category));
            g.setFont (Theme::font (11.5f, true));
            g.drawText (e->category.toUpperCase(), r.removeFromTop (18), juce::Justification::centredLeft, true);
            r.removeFromTop (8);

            // genre pills, wrapped
            if (! e->genres.isEmpty())
            {
                float x = (float) r.getX(), y = (float) r.getY();
                for (auto& t : e->genres)
                {
                    const float w = pillWidth (t);
                    if (x > (float) r.getX() && x + w > (float) r.getRight())
                    {
                        x = (float) r.getX();
                        y += 22.0f;
                    }
                    drawPill (g, { x, y, w, 18.0f }, t, colours::select);
                    x += w + 5.0f;
                }
                r.removeFromTop ((int) (y - (float) r.getY()) + 18 + 12);
            }

            if (e->description.isNotEmpty())
            {
                g.setColour (Palette::text.withAlpha (0.85f));
                g.setFont (Theme::font (12.5f));
                const int lines = juce::jmax (1, (r.getHeight() - 24) / 17);
                g.drawFittedText (e->description, r.removeFromTop (juce::jmin (r.getHeight() - 24, lines * 17)),
                                  juce::Justification::topLeft, lines, 1.0f);
            }

            if (e->author.isNotEmpty())
            {
                g.setColour (Palette::dim);
                g.setFont (Theme::font (11.0f));
                g.drawText ("by " + e->author, r.removeFromBottom (18), juce::Justification::centredLeft, true);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16, 14).removeFromBottom (buttonsHeight);
            loadButton.setBounds (r.removeFromTop (34));
            r.removeFromTop (8);
            auto row = r.removeFromTop (28);
            if (deleteButton.isVisible())
            {
                deleteButton.setBounds (row.removeFromRight (76));
                row.removeFromRight (8);
            }
            favButton.setBounds (row);
        }

        FlatButton loadButton { "Load", FlatButton::primary };
        FlatButton favButton { "Add to favourites" };
        FlatButton deleteButton { "Delete", FlatButton::danger };
        juce::String key;

    private:
        static constexpr int buttonsHeight = 34 + 8 + 28;
        PresetManager& manager;
    };
}

//==============================================================================
struct PresetBrowser::Impl : public PresetManager::Listener
{
    Impl (PresetBrowser& o, PresetManager& m, HelpTarget* h)
        : owner (o), manager (m), help (h), categories (m), list (m, filtered), details (m)
    {
        styleEditor (search);
        search.setTextToShowWhenEmpty ("Search name, sound, genre...", Palette::dim);
        search.setIndents (28, 5);
        search.onTextChange = [this] { rebuild (true); };
        search.onNavigationKey = [this] (const juce::KeyPress& k) { return handleKey (k, true); };
        search.onReturnKey = [this] { loadSelected(); };
        search.onEscapeKey = [this] { escape(); };

        closeOnLoad.setToggleState (manager.getCloseBrowserOnLoad(), juce::dontSendNotification);
        closeOnLoad.onClick = [this] { manager.setCloseBrowserOnLoad (closeOnLoad.getToggleState()); };
        closeButton.onClick = [this] { close(); };

        categories.onChange = [this] { rebuild (true); };
        genres.onChange = [this] { rebuild (true); owner.resized(); };

        list.onSelect = [this] (int row) { select (row, false); };
        list.onLoad = [this] (int row) { select (row, false); loadSelected(); };

        details.loadButton.onClick = [this] { loadSelected(); };
        details.deleteButton.onClick = [this] { confirmDelete(); };

        for (juce::Component* c : { (juce::Component*) &search, (juce::Component*) &closeOnLoad,
                                    (juce::Component*) &closeButton, (juce::Component*) &categories,
                                    (juce::Component*) &genres, (juce::Component*) &list, (juce::Component*) &details })
            owner.addAndMakeVisible (c);

        addHover (hovers, search, "browser.search", help);
        addHover (hovers, categories, "browser.categories", help);
        addHover (hovers, genres, "browser.genres", help);
        addHover (hovers, list, "browser.list", help);

        manager.addListener (this);
    }

    ~Impl() override { manager.removeListener (this); }

    void presetChanged() override
    {
        details.update();
        list.repaint();
    }

    void presetListChanged() override { rebuild (false); }

    //==============================================================================
    void restoreFromManager()
    {
        const auto& f = manager.getFilter();
        categories.setSelectedQuietly (f.group);
        genres.setSelected (f.genres);
        search.setText (f.search, false);
        closeOnLoad.setToggleState (manager.getCloseBrowserOnLoad(), juce::dontSendNotification);
    }

    // Applies the UI's filter, keeps the selection where possible
    void rebuild (bool filterChanged)
    {
        PresetManager::Filter f;
        f.group = categories.selected;
        f.genres = genres.selected();
        f.search = search.getText();
        manager.setFilter (f);

        filtered = manager.filteredIndices();
        categories.updateCounts();

        int row = rowOfKey (selectedKey);
        if (row < 0 && ! filtered.empty())
            row = juce::jmax (0, rowOfKey (manager.getCurrentKey()));
        list.contentChanged();
        if (filterChanged)
            list.scrollToTop();
        select (row, true);
        owner.repaint (headerArea);
    }

    int rowOfKey (const juce::String& key) const
    {
        const int idx = manager.indexOfKey (key);
        for (size_t i = 0; i < filtered.size(); ++i)
            if (filtered[i] == idx)
                return (int) i;
        return -1;
    }

    void select (int row, bool quiet)
    {
        row = filtered.empty() ? -1 : juce::jlimit (0, (int) filtered.size() - 1, row);
        list.selectedRow = row;
        if (auto* e = list.entryAt (row))
            selectedKey = e->key;
        else if (! quiet)
            selectedKey.clear();
        details.setKey (row >= 0 ? selectedKey : juce::String());
        details.resized();
        if (row >= 0)
            list.scrollToRow (row);
        list.repaint();
    }

    void loadSelected()
    {
        if (auto* e = list.entryAt (list.selectedRow))
        {
            manager.loadKey (e->key);
            if (manager.getCloseBrowserOnLoad())
                close();
        }
    }

    void confirmDelete()
    {
        auto* e = manager.findEntry (selectedKey);
        if (e == nullptr || e->factory)
            return;
        const auto key = e->key;
        modal.open (owner, std::make_unique<ConfirmDialog> (
            "Delete \"" + e->name + "\"?",
            "The preset file is moved to the recycle bin. If it is loaded, the sound stays as it is.",
            "Delete",
            [this, key] { manager.deleteUserPreset (key); },
            [this] { modal.close(); }));
    }

    void escape()
    {
        if (search.getText().isNotEmpty())
            search.setText ({}, true);
        else
            close();
    }

    void close()
    {
        if (owner.onClose != nullptr)
            owner.onClose();
    }

    // Navigation keys shared by the browser and the search field
    bool handleKey (const juce::KeyPress& k, bool fromSearch)
    {
        const int n = (int) filtered.size();
        const int cur = list.selectedRow;
        auto move = [&] (int row) { if (n > 0) select (juce::jlimit (0, n - 1, row), false); return true; };

        if (k == juce::KeyPress::upKey)       return move (cur < 0 ? 0 : cur - 1);
        if (k == juce::KeyPress::downKey)     return move (cur + 1);
        if (k == juce::KeyPress::pageUpKey)   return move (cur - list.visibleRows());
        if (k == juce::KeyPress::pageDownKey) return move (cur + list.visibleRows());
        if (fromSearch)
            return false;   // the editor handles the rest (Return / Esc have their own callbacks)

        if (k == juce::KeyPress::homeKey)     return move (0);
        if (k == juce::KeyPress::endKey)      return move (n - 1);
        if (k == juce::KeyPress::returnKey)   { loadSelected(); return true; }
        if (k == juce::KeyPress::escapeKey)   { escape(); return true; }

        // typing anywhere in the browser goes to the search field
        const auto c = k.getTextCharacter();
        if (c >= ' ' && ! k.getModifiers().isCtrlDown() && ! k.getModifiers().isAltDown())
        {
            search.grabKeyboardFocus();
            search.moveCaretToEnd();
            search.insertTextAtCaret (juce::String::charToString (c));
            return true;
        }
        return false;
    }

    //==============================================================================
    void layout()
    {
        const auto bounds = owner.getLocalBounds();
        const int margin = bounds.getWidth() > 1000 && bounds.getHeight() > 640 ? 28 : 10;
        panel = bounds.reduced (margin);
        auto r = panel.reduced (16, 14);

        headerArea = r.removeFromTop (34);
        auto header = headerArea;
        header.removeFromLeft (190);   // title, drawn in paint()
        closeButton.setBounds (header.removeFromRight (34));
        header.removeFromRight (10);
        closeOnLoad.setBounds (header.removeFromRight (closeOnLoad.idealWidth()));
        header.removeFromRight (16);
        search.setBounds (header.removeFromLeft (juce::jmin (380, header.getWidth())));
        r.removeFromTop (12);

        footerArea = r.removeFromBottom (18);
        r.removeFromBottom (8);

        const bool narrow = r.getWidth() < 640;
        categoriesFrame = r.removeFromLeft (narrow ? 150 : 196);
        r.removeFromLeft (12);
        const bool showDetails = r.getWidth() >= 620;
        details.setVisible (showDetails);
        if (showDetails)
        {
            detailsFrame = r.removeFromRight (juce::jlimit (230, 300, r.getWidth() / 3));
            r.removeFromRight (12);
            details.setBounds (detailsFrame);
        }
        else
        {
            detailsFrame = {};
        }

        const int gh = genres.heightFor (r.getWidth());
        genres.setBounds (r.removeFromTop (gh));
        r.removeFromTop (10);
        listFrame = r;

        categories.setBounds (categoriesFrame.reduced (0, 6));
        list.setBounds (listFrame.reduced (0, 4));
    }

    void paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));

        const auto p = panel.toFloat();
        juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 24, {}).drawForRectangle (g, panel);
        g.setColour (Palette::panel);
        g.fillRoundedRectangle (p, 12.0f);
        g.setColour (Palette::panelEdge.brighter (0.15f));
        g.drawRoundedRectangle (p.reduced (0.5f), 12.0f, 1.0f);

        // title + counts
        auto title = headerArea.withWidth (180);
        g.setColour (Palette::text);
        g.setFont (Theme::font (17.0f, true));
        g.drawText ("Presets", title.removeFromTop (20), juce::Justification::centredLeft, false);
        g.setColour (Palette::dim);
        g.setFont (Theme::font (11.0f));
        const int total = (int) manager.entries().size();
        g.drawText (juce::String ((int) filtered.size()) + " of " + juce::String (total) + " shown",
                    title, juce::Justification::centredLeft, false);

        for (auto frame : { categoriesFrame, listFrame, detailsFrame })
            if (! frame.isEmpty())
            {
                g.setColour (Palette::viewBg);
                g.fillRoundedRectangle (frame.toFloat(), 8.0f);
                g.setColour (Palette::panelEdge);
                g.drawRoundedRectangle (frame.toFloat().reduced (0.5f), 8.0f, 1.0f);
            }

        g.setColour (Palette::dim.withAlpha (0.8f));
        g.setFont (Theme::font (11.0f));
        auto footer = footerArea;
        g.drawText ("Up / Down  select      Enter or double-click  load      click a star  favourite      Esc  close",
                    footer.removeFromLeft (juce::jmin (600, footer.getWidth())), juce::Justification::centredLeft, true);
        if (footer.getWidth() > 160)
            g.drawText ("User presets: " + manager.getPresetsFolder().getFullPathName(), footer,
                        juce::Justification::centredRight, true);
    }

    PresetBrowser& owner;
    PresetManager& manager;
    HelpTarget* help;

    std::vector<int> filtered;
    juce::String selectedKey;

    SearchBox search;
    Chip closeOnLoad { "Close on load", colours::loaded };
    IconButton closeButton { IconButton::close };
    CategoryList categories;
    GenreBar genres;
    PresetList list;
    DetailsPane details;
    HoverList hovers;
    ModalHost modal;

    juce::Rectangle<int> panel, headerArea, footerArea, categoriesFrame, listFrame, detailsFrame;
};

PresetBrowser::PresetBrowser (PresetManager& m, HelpTarget* h) : impl (std::make_unique<Impl> (*this, m, h))
{
    setWantsKeyboardFocus (true);
    impl->restoreFromManager();
    impl->selectedKey = m.getCurrentKey();
    impl->rebuild (false);
}

PresetBrowser::~PresetBrowser() = default;

void PresetBrowser::paint (juce::Graphics& g) { impl->paint (g); }
void PresetBrowser::resized() { impl->layout(); }

bool PresetBrowser::keyPressed (const juce::KeyPress& k) { return impl->handleKey (k, false); }

void PresetBrowser::mouseDown (const juce::MouseEvent& e)
{
    if (! impl->panel.contains (e.getPosition()))
        impl->close();
}

void PresetBrowser::visibilityChanged()
{
    if (isVisible())
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PresetBrowser> (this)]
        {
            if (safe != nullptr && safe->isShowing())
            {
                safe->impl->search.grabKeyboardFocus();
                safe->impl->list.centreRow (safe->impl->list.selectedRow);
            }
        });
}

void PresetBrowser::refresh()
{
    impl->manager.rescanUserPresets();   // also rebuilds through presetListChanged
    impl->restoreFromManager();
    impl->selectedKey = impl->manager.getCurrentKey();
    impl->rebuild (false);
    impl->list.centreRow (impl->list.selectedRow);
    resized();
    repaint();
}
