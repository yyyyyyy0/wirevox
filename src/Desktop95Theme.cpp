#include "Themes.h"

// DESKTOP '95: flat light-grey mid-90s desktop OS. 2 px raised and sunken bevels on every control,
// navy title bars, white sunken fields, a sunken status bar.
namespace mfx::ui
{
namespace
{
    struct Palette
    {
        static inline const Colour face      { 0xffc0c0c0 };
        static inline const Colour white     { 0xffffffff };
        static inline const Colour light     { 0xffdfdfdf };
        static inline const Colour shadow    { 0xff808080 };
        static inline const Colour black     { 0xff0a0a0a };
        static inline const Colour navy      { 0xff000080 };
        static inline const Colour disabled  { 0xff6a6a6a };
        static inline const Colour failure   { 0xff900000 };
        static inline const Colour warning   { 0xff7a2e00 };
        static inline const Colour green     { 0xff00b000 };
        static inline const Colour yellow    { 0xffffd800 };
        static inline const Colour red       { 0xffe00000 };
    };

    FontOptions system (float height, bool bold = false)
    {
        return FontOptions ("Arial", height, bold ? Font::bold : Font::plain);
    }

    void drawLines (Graphics& g, Rectangle<float> r, Colour topLeft, Colour bottomRight)
    {
        g.setColour (topLeft);
        g.fillRect (r.getX(), r.getY(), r.getWidth() - 1.0f, 1.0f);
        g.fillRect (r.getX(), r.getY(), 1.0f, r.getHeight() - 1.0f);
        g.setColour (bottomRight);
        g.fillRect (r.getX(), r.getBottom() - 1.0f, r.getWidth(), 1.0f);
        g.fillRect (r.getRight() - 1.0f, r.getY(), 1.0f, r.getHeight());
    }

    // 2 px bevel: outer pair, then inner pair.
    void paintBevel (Graphics& g, Rectangle<float> r, bool raised)
    {
        drawLines (g, r, raised ? Palette::white : Palette::shadow, raised ? Palette::black : Palette::white);
        drawLines (g, r.reduced (1.0f), raised ? Palette::light : Palette::black, raised ? Palette::shadow : Palette::light);
    }

    void drawTitleBar (Graphics& g, Rectangle<float> r, const String& text)
    {
        g.setColour (Palette::navy);
        g.fillRect (r);
        drawText (g, text, r.reduced (6.0f, 0.0f), Justification::centredLeft, system (12.5f, true), Palette::white);
    }
}

class Desktop95Theme final : public Theme
{
public:
    Desktop95Theme()
    {
        metrics.margin = 10;
        metrics.panelTitleHeight = 20;
        metrics.statusHeight = 30;
        metrics.rowOneLine = 44;
        metrics.rowStacked = 70;
        metrics.rowInsetX = 8;
        metrics.numberWidth = 30;
        metrics.indicatorWidth = 16;
        metrics.controlHeight = 24;
        metrics.scrollbarWidth = 16;
        metrics.buttons = { 46, 30, 30, 46, 92 };

        applyScheme ({ Palette::face, Colours::black, Palette::shadow, Palette::white, Palette::face, Palette::face,
                       Palette::navy, Palette::white, Colours::black, Colours::black });
        setColour (PopupMenu::backgroundColourId, Palette::face);
        setColour (ComboBox::arrowColourId, Colours::black);
        setColour (ComboBox::outlineColourId, Palette::shadow);
        setDefaultSansSerifTypefaceName ("Arial");
    }

    const ThemeMetrics& getMetrics() const override { return metrics; }
    FontOptions getUiFont (float height, bool bold) const override { return system (height, bold); }

    Colour getButtonTextColour (const Button&, const ControlState& state) const override
    {
        return state.enabled ? Colours::black : Palette::disabled;
    }

    //==============================================================================
    void paintWindow (Graphics& g, Rectangle<int> bounds) override
    {
        g.fillAll (Palette::face);
        paintBevel (g, bounds.toFloat(), true);
    }

    void paintLogo (Graphics& g, Rectangle<int> strip, const String& productName, const String& subtitle) override
    {
        drawTitleBar (g, strip.removeFromTop (22).toFloat(), productName + "  -  " + subtitle);
    }

    Rectangle<int> getSelectorBounds (Rectangle<int> strip) const override
    {
        return { strip.getRight() - 170, strip.getY() + 27, 170, 22 };
    }

    void paintPanel (Graphics& g, Rectangle<int> title, Rectangle<int> frame, const String& text, PanelKind kind) override
    {
        drawTitleBar (g, title.toFloat(), text);

        g.setColour (kind == PanelKind::list ? Palette::white : Palette::face);
        g.fillRect (frame);
        paintBevel (g, frame.toFloat(), false);
    }

    void paintSlotRow (Graphics& g, const SlotRowInfo& info, const SlotRowGeometry& geometry) override
    {
        const auto module = geometry.module.toFloat();

        g.setColour (Palette::light);
        g.fillRect (module.getX(), module.getBottom(), module.getWidth(), 1.0f);

        auto number = geometry.number.toFloat();
        g.setColour (Palette::white);
        g.fillRect (number);
        paintBevel (g, number, false);
        drawText (g, String (info.number).paddedLeft ('0', 2), number, Justification::centred, system (13.0f, true), Colours::black);

        const auto lamp = geometry.indicator.toFloat().withSizeKeepingCentre (14.0f, 14.0f);
        g.setColour (info.state == SlotState::active ? Palette::green : info.state == SlotState::bypassed ? Palette::yellow : Palette::red);
        g.fillRect (lamp);
        paintBevel (g, lamp, false);

        const auto font = system (13.0f, true);
        auto area = geometry.name.toFloat();

        if (info.failureTag.isNotEmpty())
        {
            drawText (g, fitText (font, info.name, area.getWidth()), area.removeFromTop (area.getHeight() / 2.0f),
                      Justification::bottomLeft, font, Colours::black);
            drawText (g, info.failureTag, area, Justification::topLeft, system (11.5f, true), Palette::failure);
        }
        else
        {
            drawText (g, fitText (font, info.name, area.getWidth()), area, Justification::centredLeft, font, Colours::black);
        }
    }

    //==============================================================================
    int measureReadout (int width, const ReadoutContent& content) const override
    {
        const BodyText body (content.body, system (12.5f), Colours::black, 2.0f, (float) width - 2.0f * padding);
        return (int) std::ceil (titleHeight + 2.0f * padding + headerHeight + body.getHeight());
    }

    void paintReadout (Graphics& g, Rectangle<int> bounds, const ReadoutContent& content) const override
    {
        auto r = bounds.toFloat();
        drawTitleBar (g, r.removeFromTop (titleHeight), content.caption);

        g.setColour (Palette::white);
        g.fillRect (r);
        paintBevel (g, r, false);

        auto inner = r.reduced (padding);
        auto header = inner.removeFromTop (headerHeight);

        const auto bigFont = system (32.0f, true);
        const auto bigWidth = textWidth (bigFont, content.bigValue);
        drawText (g, content.bigValue, header.withWidth (bigWidth + 4.0f), Justification::centredLeft, bigFont, Colours::black);
        drawText (g, content.unit, header.withTrimmedLeft (bigWidth + 8.0f).withTrimmedTop (10.0f), Justification::centredLeft,
                  system (16.0f, true), Colours::black);

        BodyText (content.body, system (12.5f), Colours::black, 2.0f, inner.getWidth()).draw (g, inner);
    }

    void paintStatus (Graphics& g, Rectangle<int> bounds, const String& text) const override
    {
        auto r = bounds.toFloat();
        g.setColour (Palette::face);
        g.fillRect (r);
        drawLines (g, r.reduced (0.0f), Palette::shadow, Palette::white);

        AttributedString attributed;
        attributed.setJustification (Justification::centredLeft);
        attributed.setLineSpacing (1.0f);
        attributed.append (text, system (12.0f, true), Palette::warning);
        attributed.draw (g, r.reduced (8.0f, 2.0f));
    }

    //==============================================================================
    void paintControlFace (Graphics& g, Rectangle<float> r, Colour face, const ControlState& state) override
    {
        g.setColour (face.getAlpha() > 0 ? face : Palette::face);
        g.fillRect (r);
        paintBevel (g, r, ! state.pressed());
    }

    void paintFieldFace (Graphics& g, Rectangle<float> r, bool) override
    {
        g.setColour (Palette::white);
        g.fillRect (r);
        paintBevel (g, r, false);
    }

    void paintTickBox (Graphics& g, Rectangle<float> area, bool ticked, bool enabled, bool) override
    {
        const auto size = jmin (area.getWidth(), area.getHeight(), 14.0f);
        const auto box = area.withSizeKeepingCentre (size, size);
        paintFieldFace (g, box, enabled);

        if (ticked)
        {
            Path tick;
            tick.startNewSubPath (box.getX() + size * 0.25f, box.getCentreY());
            tick.lineTo (box.getX() + size * 0.43f, box.getBottom() - size * 0.28f);
            tick.lineTo (box.getRight() - size * 0.22f, box.getY() + size * 0.26f);
            g.setColour (enabled ? Colours::black : Palette::disabled);
            g.strokePath (tick, PathStrokeType (2.0f));
        }
    }

    void paintComboArrow (Graphics& g, Rectangle<float> key, Colour colour, bool enabled) override
    {
        Path arrow;
        arrow.addTriangle (key.getCentreX() - 4.0f, key.getCentreY() - 2.0f, key.getCentreX() + 4.0f, key.getCentreY() - 2.0f,
                           key.getCentreX(), key.getCentreY() + 2.5f);
        g.setColour (enabled ? colour : Palette::disabled);
        g.fillPath (arrow);
    }

    // A checkbox with its label instead of a key.
    void paintBypass (Graphics& g, ToggleButton& button, bool hover, bool) override
    {
        paintTickBox (g, { 2.0f, ((float) button.getHeight() - 14.0f) * 0.5f, 14.0f, 14.0f }, button.getToggleState(),
                      button.isEnabled(), hover);

        drawText (g, button.getButtonText(), button.getLocalBounds().toFloat().withTrimmedLeft (24.0f),
                  Justification::centredLeft, system (13.0f, true), Colours::black);
    }

private:
    static constexpr float padding = 10.0f;
    static constexpr float titleHeight = 20.0f;
    static constexpr float headerHeight = 44.0f;

    ThemeMetrics metrics;
};

std::unique_ptr<Theme> createDesktop95Theme()
{
    return std::make_unique<Desktop95Theme>();
}

} // namespace mfx::ui
