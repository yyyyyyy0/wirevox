#include "Themes.h"

// TERMINAL: green-phosphor CRT. Black, monospaced, square corners, text frames and text buttons
// that invert on hover and press.
namespace mfx::ui
{
namespace
{
    struct Palette
    {
        static inline const Colour black    { 0xff000000 };
        static inline const Colour green    { 0xff3dff70 };
        static inline const Colour bright   { 0xffb9ffcb };
        static inline const Colour dim      { 0xff22a346 };
        static inline const Colour amber    { 0xffffb000 };
        static inline const Colour amberDim { 0xffb07a00 };
        static inline const Colour red      { 0xffff5555 };
    };

    FontOptions mono (float height, bool bold = false)
    {
        return FontOptions ("Menlo", height, bold ? Font::bold : Font::plain);
    }

    String utf8 (const char* text)
    {
        return String::fromUTF8 (text);
    }

    void drawFrame (Graphics& g, Rectangle<float> r, Colour colour, bool doubled)
    {
        g.setColour (colour);
        g.drawRect (r.reduced (0.5f), 1.0f);

        if (doubled)
            g.drawRect (r.reduced (3.5f), 1.0f);
    }

    // Frame whose top edge runs through the middle of `title`, with the title text set into it.
    void drawTitledFrame (Graphics& g, Rectangle<int> title, Rectangle<int> frame, const String& text, bool doubled)
    {
        auto box = frame.toFloat().withTop ((float) title.getCentreY());
        drawFrame (g, box, Palette::dim, doubled);

        const auto label = utf8 ("┤ ") + text + utf8 (" ├");
        const auto font = mono (11.0f, true);
        const auto width = textWidth (font, label) + 6.0f;
        const auto area = Rectangle<float> ((float) title.getX() + 12.0f, (float) title.getY(), width, (float) title.getHeight());

        g.setColour (Palette::black);
        g.fillRect (area);
        drawText (g, label, area, Justification::centred, font, Palette::green);
    }
}

class TerminalTheme final : public Theme
{
public:
    TerminalTheme()
    {
        metrics.margin = 14;
        metrics.rowInsetX = 12;
        metrics.numberWidth = 28;
        metrics.indicatorWidth = 32;
        metrics.scrollbarWidth = 12;
        metrics.buttons = { 46, 46, 46, 46, 86 };

        applyScheme ({ Palette::black, Palette::green, Palette::dim, Palette::black, Colours::transparentBlack,
                       Colours::transparentBlack, Palette::green, Palette::black, Palette::green, Palette::black });
        setColour (ScrollBar::thumbColourId, Palette::dim);
        setColour (ComboBox::outlineColourId, Palette::dim);
        setDefaultSansSerifTypefaceName ("Menlo");
    }

    const ThemeMetrics& getMetrics() const override { return metrics; }
    FontOptions getUiFont (float height, bool bold) const override { return mono (height, bold); }
    int getPressedTextShift() const override { return 0; }

    String decorateButtonText (const String& text, bool toggledOn) const override
    {
        return toggledOn ? "[x " + text + "]" : "[ " + text + " ]";
    }

    String iconFrameText() const override { return "[   ]"; }

    Colour getButtonTextColour (const Button&, const ControlState& state) const override
    {
        if (! state.enabled)
            return Palette::dim;

        return state.hover || state.pressed() ? Palette::black : Palette::green;
    }

    //==============================================================================
    void paintWindow (Graphics& g, Rectangle<int> bounds) override
    {
        g.fillAll (Palette::black);

        const auto r = bounds.toFloat();
        g.setGradientFill (ColourGradient (Palette::green.withAlpha (0.07f), r.getCentre(), Colours::transparentBlack,
                                           r.getTopLeft(), true));
        g.fillRect (r);

        drawFrame (g, r.reduced (3.0f), Palette::dim, true);
    }

    bool hasOverlay() const override { return true; }

    void paintOverlay (Graphics& g, Rectangle<int> bounds) override
    {
        drawScanlines (g, bounds.toFloat(), 3.0f, 0.2f);

        const auto r = bounds.toFloat();
        g.setGradientFill (ColourGradient (Colours::transparentBlack, r.getCentre(), Colours::black.withAlpha (0.45f),
                                           r.getTopLeft(), true));
        g.fillRect (r);
    }

    void paintLogo (Graphics& g, Rectangle<int> strip, const String& productName, const String& subtitle) override
    {
        auto rule = strip.removeFromBottom (6).toFloat();
        g.setColour (Palette::green);
        g.fillRect (rule.getX(), rule.getY() + 1.0f, rule.getWidth(), 1.0f);
        g.fillRect (rule.getX(), rule.getY() + 4.0f, rule.getWidth(), 1.0f);

        drawGlowText (g, productName, strip.toFloat(), Justification::centredLeft, mono (34.0f, true), Palette::bright, 0.3f);
        drawText (g, "> " + subtitle + "_", strip.translated (0, 12).toFloat(), Justification::centredRight,
                  mono (11.5f, true), Palette::dim);
    }

    void paintPanel (Graphics& g, Rectangle<int> title, Rectangle<int> frame, const String& text, PanelKind kind) override
    {
        drawTitledFrame (g, title, frame, text, kind == PanelKind::list);
    }

    void paintSlotRow (Graphics& g, const SlotRowInfo& info, const SlotRowGeometry& geometry) override
    {
        drawFrame (g, geometry.module.toFloat(), Palette::dim, false);

        drawGlowText (g, String (info.number).paddedLeft ('0', 2), geometry.number.toFloat(), Justification::centred,
                      mono (15.0f, true), Palette::bright, 0.25f);

        auto tag = geometry.indicator.toFloat();
        const auto text = info.state == SlotState::active ? "ON " : info.state == SlotState::bypassed ? "BYP" : "ERR";

        if (info.state == SlotState::failed)
        {
            g.setColour (Palette::red);
            g.fillRect (tag.withSizeKeepingCentre (tag.getWidth(), 18.0f));
            drawText (g, text, tag, Justification::centred, mono (12.0f, true), Palette::black);
        }
        else
        {
            drawText (g, text, tag, Justification::centred, mono (12.0f, true),
                      info.state == SlotState::active ? Palette::green : Palette::amber);
        }

        const auto font = mono (13.5f, true);
        auto area = geometry.name.toFloat();

        if (info.failureTag.isNotEmpty())
        {
            drawGlowText (g, fitText (font, info.name, area.getWidth()), area.removeFromTop (area.getHeight() / 2.0f),
                          Justification::bottomLeft, font, Palette::bright, 0.25f);
            drawText (g, info.failureTag, area, Justification::topLeft, mono (11.0f, true), Palette::red);
        }
        else
        {
            drawGlowText (g, fitText (font, info.name, area.getWidth()), area, Justification::centredLeft, font, Palette::bright, 0.25f);
        }
    }

    //==============================================================================
    int measureReadout (int width, const ReadoutContent& content) const override
    {
        const BodyText body (content.body, mono (12.5f), Palette::green, 2.0f, (float) width - 2.0f * padding);
        return (int) std::ceil (2.0f * padding + headerHeight + body.getHeight());
    }

    void paintReadout (Graphics& g, Rectangle<int> bounds, const ReadoutContent& content) const override
    {
        drawFrame (g, bounds.toFloat(), Palette::dim, true);

        auto inner = bounds.toFloat().reduced (padding);
        auto header = inner.removeFromTop (headerHeight);

        drawText (g, "> " + content.caption, header.removeFromTop (14.0f), Justification::centredLeft, mono (11.0f, true), Palette::dim);

        const auto bigFont = mono (36.0f, true);
        const auto bigWidth = textWidth (bigFont, content.bigValue);
        drawGlowText (g, content.bigValue, header.withWidth (bigWidth + 4.0f), Justification::centredLeft, bigFont, Palette::bright, 0.3f);

        const auto unitFont = mono (18.0f, true);
        const auto unitWidth = textWidth (unitFont, content.unit) + 4.0f;
        drawText (g, content.unit, header.withTrimmedLeft (bigWidth + 8.0f), Justification::centredLeft, unitFont, Palette::green);

        // Block cursor after the unit.
        g.setColour (Palette::green);
        g.fillRect (header.getX() + bigWidth + 8.0f + unitWidth + 6.0f, header.getCentreY() - 12.0f, 14.0f, 24.0f);

        BodyText (content.body, mono (12.5f), Palette::green, 2.0f, inner.getWidth()).draw (g, inner);
    }

    void paintStatus (Graphics& g, Rectangle<int> bounds, const String& text) const override
    {
        drawFrame (g, bounds.toFloat(), Palette::amberDim, false);

        auto inner = bounds.toFloat().reduced (8.0f, 2.0f);
        drawText (g, "STATUS>", inner.removeFromLeft (64.0f), Justification::centredLeft, mono (11.0f, true), Palette::amberDim);

        AttributedString attributed;
        attributed.setJustification (Justification::centredLeft);
        attributed.setLineSpacing (1.0f);
        attributed.append (text, mono (12.0f), Palette::amber);
        attributed.draw (g, inner);
    }

    //==============================================================================
    void paintControlFace (Graphics& g, Rectangle<float> r, Colour face, const ControlState& state) override
    {
        if (state.enabled && (state.hover || state.pressed()))
        {
            g.setColour (Palette::green);
            g.fillRect (r);
        }
        else if (face.getAlpha() > 0)
        {
            g.setColour (face);
            g.fillRect (r);
        }
    }

    void paintFieldFace (Graphics& g, Rectangle<float> r, bool enabled) override
    {
        g.setColour (Palette::black);
        g.fillRect (r);
        g.setColour (enabled ? Palette::dim : Palette::dim.withAlpha (0.5f));
        g.drawRect (r.reduced (0.5f), 1.0f);
    }

    void paintTickBox (Graphics& g, Rectangle<float> area, bool ticked, bool enabled, bool) override
    {
        const auto box = area.withSizeKeepingCentre (jmin (area.getWidth(), area.getHeight()), jmin (area.getWidth(), area.getHeight()));
        g.setColour (enabled ? Palette::green : Palette::dim);
        g.drawRect (box.reduced (0.5f), 1.0f);

        if (ticked)
            g.fillRect (box.reduced (4.0f));
    }

    void paintComboArrow (Graphics& g, Rectangle<float> key, Colour colour, bool enabled) override
    {
        Path arrow;
        arrow.addTriangle (key.getCentreX() - 4.0f, key.getCentreY() - 2.0f, key.getCentreX() + 4.0f, key.getCentreY() - 2.0f,
                           key.getCentreX(), key.getCentreY() + 3.0f);
        g.setColour (colour.withMultipliedAlpha (enabled ? 1.0f : 0.4f));
        g.fillPath (arrow);
    }

private:
    static constexpr float padding = 14.0f;
    static constexpr float headerHeight = 58.0f;

    ThemeMetrics metrics;
};

std::unique_ptr<Theme> createTerminalTheme()
{
    return std::make_unique<TerminalTheme>();
}

} // namespace mfx::ui
