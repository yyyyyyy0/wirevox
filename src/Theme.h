#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>

namespace mfx::ui
{
using namespace juce;

enum class SlotState { active, bypassed, failed };
enum class PanelKind { controls, list };

struct SlotRowInfo
{
    int number = 0;
    String name;
    String failureTag;      // empty unless the plugin failed to load
    SlotState state = SlotState::active;
};

// Where ChainRow put the parts of a row; the theme paints into these rectangles.
struct SlotRowGeometry
{
    Rectangle<int> module, number, indicator, name;
};

struct ReadoutContent
{
    String caption, bigValue, unit, body;
};

struct ButtonWidths
{
    int remove, down, up, edit, bypass;

    int total() const noexcept { return remove + down + up + edit + bypass + 20; }   // 4 px gaps, 8 px before bypass
};

struct ThemeMetrics
{
    int margin = 22;                // window edge to working area
    int logoStripHeight = 52;
    int panelTitleHeight = 18;
    int statusHeight = 32;
    int rowOneLine = 50;
    int rowStacked = 76;
    int rowInsetX = 20;             // module edge to its content
    int numberWidth = 38;
    int indicatorWidth = 14;
    int controlHeight = 26;
    int scrollbarWidth = 14;
    ButtonWidths buttons { 46, 30, 30, 46, 68 };
};

struct ControlState
{
    bool enabled = true, hover = false, down = false, toggledOn = false;

    bool pressed() const noexcept { return down || toggledOn; }
};

// "Bypass" on a chain row. Themes may draw it as a key or as a checkbox; the checkboxes inside
// AudioDeviceSelectorComponent are plain ToggleButtons.
class BypassToggle final : public ToggleButton
{
public:
    using ToggleButton::ToggleButton;
};

float textWidth (const FontOptions&, const String&);

// Shortens `text` with an ellipsis until it fits `maxWidth`.
String fitText (const FontOptions&, const String&, float maxWidth);

void drawText (Graphics&, const String&, Rectangle<float>, Justification, const FontOptions&, Colour);

// The same text four times at a 1 px offset and low alpha, then once at full strength.
void drawGlowText (Graphics&, const String&, Rectangle<float>, Justification, const FontOptions&, Colour, float glowAlpha);

void drawScanlines (Graphics&, Rectangle<float>, float spacing, float alpha);

// Multi-line text laid out through CoreText (a Label sizes every line from its first font, so
// Japanese fallback glyphs overlap), split into columns when there is room.
class BodyText
{
public:
    BodyText (const String& text, const FontOptions&, Colour, float lineSpacing, float width, float columnGap = 24.0f);

    float getHeight() const noexcept { return height; }
    void draw (Graphics&, Rectangle<float> area) const;

private:
    std::vector<TextLayout> columns;
    float columnWidth = 0.0f, gap = 0.0f, height = 0.0f;
};

//==============================================================================
// A theme is a LookAndFeel that also paints the parts of the window that are not JUCE widgets.
// MainComponent only ever asks the current theme to paint; it has no theme-specific branches.
class Theme : public LookAndFeel_V4
{
public:
    // Colours handed to the generic JUCE widgets through setColour().
    struct Scheme
    {
        Colour window, text, textDim, field, face, faceOn, accent, onAccent, buttonText, buttonTextOn;
    };

    ~Theme() override = default;

    virtual const ThemeMetrics& getMetrics() const = 0;

    // Window background and chrome. paintOverlay() draws on top of the children (CRT scanlines).
    virtual void paintWindow (Graphics&, Rectangle<int> bounds) = 0;
    virtual bool hasOverlay() const { return false; }
    virtual void paintOverlay (Graphics&, Rectangle<int>) {}

    virtual void paintLogo (Graphics&, Rectangle<int> strip, const String& productName, const String& subtitle) = 0;
    virtual Rectangle<int> getSelectorBounds (Rectangle<int> strip) const;

    // `title` is the strip above `frame`; the device panel and the chain list use it.
    virtual void paintPanel (Graphics&, Rectangle<int> title, Rectangle<int> frame, const String& text, PanelKind) = 0;

    virtual void paintSlotRow (Graphics&, const SlotRowInfo&, const SlotRowGeometry&) = 0;

    virtual int measureReadout (int width, const ReadoutContent&) const = 0;
    virtual void paintReadout (Graphics&, Rectangle<int> bounds, const ReadoutContent&) const = 0;
    virtual void paintStatus (Graphics&, Rectangle<int> bounds, const String&) const = 0;

    //==============================================================================
    // Widget primitives; the JUCE overrides below are written once on top of these.
    virtual void paintControlFace (Graphics&, Rectangle<float>, Colour face, const ControlState&) = 0;
    virtual void paintFieldFace (Graphics&, Rectangle<float>, bool enabled) = 0;
    virtual void paintTickBox (Graphics&, Rectangle<float>, bool ticked, bool enabled, bool hover) = 0;
    virtual void paintComboArrow (Graphics&, Rectangle<float> key, Colour, bool enabled);
    virtual void paintBypass (Graphics&, ToggleButton&, bool hover, bool down);

    virtual FontOptions getUiFont (float height, bool bold) const = 0;
    virtual Colour getButtonTextColour (const Button&, const ControlState&) const;
    virtual String decorateButtonText (const String& text, bool toggledOn) const { ignoreUnused (toggledOn); return text; }
    virtual int getPressedTextShift() const { return 1; }

    //==============================================================================
    Font getTextButtonFont (TextButton&, int buttonHeight) override;
    void drawButtonBackground (Graphics&, Button&, const Colour&, bool highlighted, bool down) override;
    void drawButtonText (Graphics&, TextButton&, bool highlighted, bool down) override;
    void drawToggleButton (Graphics&, ToggleButton&, bool highlighted, bool down) override;
    void drawTickBox (Graphics&, Component&, float x, float y, float w, float h,
                      bool ticked, bool isEnabled, bool highlighted, bool down) override;

    void drawComboBox (Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, ComboBox&) override;
    Font getComboBoxFont (ComboBox&) override;
    void positionComboBoxText (ComboBox&, Label&) override;
    Font getPopupMenuFont() override;

    int getDefaultScrollbarWidth() override;
    void drawScrollbar (Graphics&, ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

protected:
    void applyScheme (const Scheme&);

    // Draws `text` on the key face the same way for TextButtons and the key-style bypass.
    void paintKeyLabel (Graphics&, Button&, const ControlState&);
};

// Finds the theme a component is currently drawn with; null only before one has been installed.
Theme* findTheme (Component&);

//==============================================================================
// Latency readout and status line. Both only forward to the current theme.
class ReadoutPanel final : public Component
{
public:
    void setContent (const ReadoutContent&);
    int getPreferredHeight (int width);
    void paint (Graphics&) override;

private:
    ReadoutContent content;
};

class StatusStrip final : public Component
{
public:
    void setText (const String&);
    void paint (Graphics&) override;

private:
    String text;
};

//==============================================================================
struct ThemeEntry
{
    const char* id;
    const char* displayName;
    std::unique_ptr<Theme> (*create)();
};

// Owns one instance of each theme, created on first use. The table of themes is in ThemeRegistry.cpp.
class ThemeRegistry
{
public:
    static const std::vector<ThemeEntry>& entries();
    static String getDefaultId();

    // An unknown or empty id gives the default theme's id.
    static String sanitise (const String& id);

    Theme& get (const String& id);

private:
    std::vector<std::pair<String, std::unique_ptr<Theme>>> instances;
};

} // namespace mfx::ui
