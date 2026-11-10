#include "Themes.h"

// NEON: synthwave. A purple-to-navy sky over a perspective grid, glowing outlines instead of
// filled panels, pill buttons that fill on press.
namespace mfx::ui
{
namespace
{
    struct Palette
    {
        static inline const Colour skyTop      { 0xff260646 };
        static inline const Colour skyBottom   { 0xff080a30 };
        static inline const Colour floor       { 0xff0a0624 };
        static inline const Colour panel       { 0xff0b0630 };
        static inline const Colour magenta     { 0xffff2fd0 };
        static inline const Colour cyan        { 0xff27f0ff };
        static inline const Colour yellow      { 0xffffe14d };
        static inline const Colour hotPink     { 0xffff4d85 };
        static inline const Colour text        { 0xfff4ecff };
        static inline const Colour textDim     { 0xffb7a6e8 };
        static inline const Colour onNeon      { 0xff07041a };
        static inline const Colour orange      { 0xffffb347 };
    };

    FontOptions ui (float height, bool bold = false, bool italic = false)
    {
        return FontOptions ("Helvetica Neue", height, (bold ? Font::bold : Font::plain) | (italic ? Font::italic : Font::plain));
    }

    FontOptions mono (float height, bool bold = false)
    {
        return FontOptions ("Menlo", height, bold ? Font::bold : Font::plain);
    }

    // Thin outline with a halo: two wide faint strokes under a bright one.
    void drawNeon (Graphics& g, Rectangle<float> r, float radius, Colour colour, float strength = 1.0f)
    {
        g.setColour (colour.withAlpha (0.07f * strength));
        g.drawRoundedRectangle (r, radius, 7.0f);
        g.setColour (colour.withAlpha (0.18f * strength));
        g.drawRoundedRectangle (r, radius, 3.5f);
        g.setColour (colour.brighter (0.25f));
        g.drawRoundedRectangle (r, radius, 1.2f);
    }

    void fillPanel (Graphics& g, Rectangle<float> r, float radius)
    {
        g.setColour (Palette::panel.withAlpha (0.88f));
        g.fillRoundedRectangle (r, radius);
    }

    Image makeBackground (int width, int height)
    {
        Image image (Image::ARGB, jmax (1, width), jmax (1, height), false);
        Graphics g (image);

        const auto w = (float) width, h = (float) height;
        const auto horizon = h * 0.6f;

        g.setGradientFill (ColourGradient (Palette::skyTop, 0.0f, 0.0f, Palette::skyBottom, 0.0f, horizon, false));
        g.fillRect (0.0f, 0.0f, w, horizon);

        // Sun: a half disc resting on the horizon.
        const auto radius = jmin (w, h) * 0.2f;
        g.saveState();
        g.reduceClipRegion (0, 0, width, (int) horizon);
        g.setGradientFill (ColourGradient (Palette::yellow.withAlpha (0.55f), w * 0.5f, horizon - radius,
                                           Palette::magenta.withAlpha (0.5f), w * 0.5f, horizon, false));
        g.fillEllipse (w * 0.5f - radius, horizon - radius, radius * 2.0f, radius * 2.0f);
        g.restoreState();

        g.setGradientFill (ColourGradient (Palette::floor, 0.0f, horizon, Palette::skyTop.darker (0.4f), 0.0f, h, false));
        g.fillRect (0.0f, horizon, w, h - horizon);

        const auto line = [&g] (float x0, float y0, float x1, float y1, float alpha)
        {
            g.setColour (Palette::magenta.withAlpha (0.12f * alpha));
            g.drawLine (x0, y0, x1, y1, 4.0f);
            g.setColour (Palette::magenta.withAlpha (0.65f * alpha));
            g.drawLine (x0, y0, x1, y1, 1.0f);
        };

        for (int i = -14; i <= 14; ++i)
            line (w * 0.5f, horizon, w * 0.5f + (float) i * w * 0.11f, h, 0.8f);

        for (int i = 1; i <= 14; ++i)
        {
            const auto t = std::pow ((float) i / 14.0f, 2.2f);
            const auto y = horizon + (h - horizon) * t;
            line (0.0f, y, w, y, 0.4f + 0.6f * t);
        }

        g.setColour (Palette::cyan.withAlpha (0.7f));
        g.fillRect (0.0f, horizon - 1.0f, w, 2.0f);

        return image;
    }
}

class NeonTheme final : public Theme
{
public:
    NeonTheme()
    {
        metrics.margin = 18;
        metrics.logoStripHeight = 54;
        metrics.rowInsetX = 16;
        metrics.numberWidth = 30;
        metrics.indicatorWidth = 2;
        metrics.scrollbarWidth = 12;
        metrics.buttons = { 52, 34, 34, 52, 74 };

        applyScheme ({ Palette::skyBottom, Palette::text, Palette::textDim, Palette::panel, Palette::cyan, Palette::magenta,
                       Palette::cyan, Palette::onNeon, Palette::text, Palette::onNeon });
        setColour (ComboBox::arrowColourId, Palette::cyan);
        setColour (ComboBox::outlineColourId, Palette::cyan);
        setColour (PopupMenu::backgroundColourId, Palette::panel);
        setColour (PopupMenu::highlightedBackgroundColourId, Palette::magenta);
    }

    const ThemeMetrics& getMetrics() const override { return metrics; }
    FontOptions getUiFont (float height, bool bold) const override { return ui (height, bold); }

    Colour getButtonTextColour (const Button&, const ControlState& state) const override
    {
        if (! state.enabled)
            return Palette::textDim.withAlpha (0.55f);

        return state.pressed() ? Palette::onNeon : Palette::text;
    }

    //==============================================================================
    void paintWindow (Graphics& g, Rectangle<int> bounds) override
    {
        if (background.getWidth() != bounds.getWidth() || background.getHeight() != bounds.getHeight())
            background = makeBackground (bounds.getWidth(), bounds.getHeight());

        g.drawImageAt (background, bounds.getX(), bounds.getY());
    }

    void paintLogo (Graphics& g, Rectangle<int> strip, const String& productName, const String& subtitle) override
    {
        auto area = strip.toFloat();
        auto rule = area.removeFromBottom (6.0f);

        g.setGradientFill (ColourGradient (Palette::magenta.withAlpha (0.2f), rule.getX(), 0.0f,
                                           Palette::cyan.withAlpha (0.2f), rule.getRight(), 0.0f, false));
        g.fillRect (rule.getX(), rule.getBottom() - 6.0f, rule.getWidth(), 6.0f);
        g.setGradientFill (ColourGradient (Palette::magenta, rule.getX(), 0.0f, Palette::cyan, rule.getRight(), 0.0f, false));
        g.fillRect (rule.getX(), rule.getBottom() - 4.0f, rule.getWidth(), 2.0f);

        const auto logo = ui (36.0f, true, true).withKerningFactor (0.08f);
        drawText (g, productName, area.translated (-2.0f, 0.0f), Justification::centredLeft, logo, Palette::cyan.withAlpha (0.7f));
        drawText (g, productName, area.translated (2.0f, 0.0f), Justification::centredLeft, logo, Palette::magenta.withAlpha (0.7f));
        drawText (g, productName, area, Justification::centredLeft, logo, Colours::white);

        drawText (g, subtitle, area.translated (0.0f, 9.0f), Justification::centredRight,
                  ui (11.5f, true).withKerningFactor (0.2f), Palette::cyan);
    }

    void paintPanel (Graphics& g, Rectangle<int> title, Rectangle<int> frame, const String& text, PanelKind kind) override
    {
        const auto colour = kind == PanelKind::list ? Palette::magenta : Palette::cyan;

        drawText (g, text, title.toFloat().withTrimmedLeft (4.0f), Justification::centredLeft,
                  ui (10.5f, true).withKerningFactor (0.2f), colour.brighter (0.3f));

        fillPanel (g, frame.toFloat(), 10.0f);
        drawNeon (g, frame.toFloat().reduced (1.0f), 10.0f, colour);
    }

    void paintSlotRow (Graphics& g, const SlotRowInfo& info, const SlotRowGeometry& geometry) override
    {
        const auto r = geometry.module.toFloat();
        const auto state = info.state == SlotState::active ? Palette::cyan
                         : info.state == SlotState::bypassed ? Palette::yellow : Palette::hotPink;

        g.setColour (Palette::panel);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (Palette::magenta.withAlpha (0.45f));
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

        // Glowing state bar on the left edge.
        const auto bar = Rectangle<float> (r.getX() + 3.0f, r.getY() + 6.0f, 5.0f, r.getHeight() - 12.0f);
        g.setColour (state.withAlpha (0.16f));
        g.fillRoundedRectangle (bar.expanded (4.0f), 5.0f);
        g.setColour (state.withAlpha (0.35f));
        g.fillRoundedRectangle (bar.expanded (1.5f), 3.5f);
        g.setColour (state.brighter (0.3f));
        g.fillRoundedRectangle (bar, 2.5f);

        drawGlowText (g, String (info.number).paddedLeft ('0', 2), geometry.number.toFloat(), Justification::centred,
                      mono (16.0f, true), Palette::cyan, 0.3f);

        const auto font = ui (14.0f, true);
        auto area = geometry.name.toFloat();

        if (info.failureTag.isNotEmpty())
        {
            drawText (g, fitText (font, info.name, area.getWidth()), area.removeFromTop (area.getHeight() / 2.0f),
                      Justification::bottomLeft, font, Palette::text);
            drawText (g, info.failureTag, area, Justification::topLeft, ui (11.5f, true), Palette::hotPink);
        }
        else
        {
            drawText (g, fitText (font, info.name, area.getWidth()), area, Justification::centredLeft, font, Palette::text);
        }
    }

    //==============================================================================
    int measureReadout (int width, const ReadoutContent& content) const override
    {
        const BodyText body (content.body, mono (12.0f), bodyColour, 2.0f, (float) width - 2.0f * padding);
        return (int) std::ceil (2.0f * padding + headerHeight + body.getHeight());
    }

    void paintReadout (Graphics& g, Rectangle<int> bounds, const ReadoutContent& content) const override
    {
        const auto r = bounds.toFloat();
        fillPanel (g, r, 10.0f);
        drawNeon (g, r.reduced (1.0f), 10.0f, Palette::cyan);

        auto inner = r.reduced (padding);
        auto header = inner.removeFromTop (headerHeight);

        drawText (g, content.caption, header.removeFromTop (14.0f), Justification::centredLeft,
                  mono (10.5f, true), Palette::magenta.brighter (0.4f));

        const auto bigFont = ui (40.0f, true, true);
        const auto bigWidth = textWidth (bigFont, content.bigValue);
        auto big = header.withWidth (bigWidth + 8.0f);

        drawText (g, content.bigValue, big.translated (-2.0f, 0.0f), Justification::centredLeft, bigFont, Palette::cyan.withAlpha (0.55f));
        drawText (g, content.bigValue, big.translated (2.0f, 0.0f), Justification::centredLeft, bigFont, Palette::magenta.withAlpha (0.55f));
        drawText (g, content.bigValue, big, Justification::centredLeft, bigFont, Colours::white);
        drawText (g, content.unit, header.withTrimmedLeft (bigWidth + 12.0f).withTrimmedTop (10.0f), Justification::centredLeft,
                  ui (18.0f, true, true), Palette::cyan);

        BodyText (content.body, mono (12.0f), bodyColour, 2.0f, inner.getWidth()).draw (g, inner);
    }

    void paintStatus (Graphics& g, Rectangle<int> bounds, const String& text) const override
    {
        const auto r = bounds.toFloat();
        fillPanel (g, r, 8.0f);
        drawNeon (g, r.reduced (1.0f), 8.0f, Palette::orange, 0.8f);

        AttributedString attributed;
        attributed.setJustification (Justification::centredLeft);
        attributed.setLineSpacing (1.0f);
        attributed.append (text, ui (12.0f, true), Palette::orange);
        attributed.draw (g, r.reduced (14.0f, 2.0f));
    }

    //==============================================================================
    void paintControlFace (Graphics& g, Rectangle<float> r, Colour face, const ControlState& state) override
    {
        const auto radius = r.getHeight() * 0.5f;
        const auto colour = state.enabled ? face : face.withAlpha (0.35f);

        g.setColour (Palette::panel.withAlpha (0.7f));
        g.fillRoundedRectangle (r, radius);

        if (state.pressed())
        {
            g.setColour (colour.withAlpha (0.28f));
            g.fillRoundedRectangle (r.expanded (2.0f), radius + 2.0f);
            g.setColour (colour);
            g.fillRoundedRectangle (r, radius);
        }
        else
        {
            if (state.hover && state.enabled)
            {
                g.setColour (colour.withAlpha (0.2f));
                g.fillRoundedRectangle (r, radius);
            }

            drawNeon (g, r.reduced (0.5f), radius, colour, state.enabled ? 0.8f : 0.0f);
        }
    }

    void paintFieldFace (Graphics& g, Rectangle<float> r, bool enabled) override
    {
        g.setColour (Palette::panel);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Palette::cyan.withAlpha (enabled ? 0.65f : 0.3f));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }

    void paintTickBox (Graphics& g, Rectangle<float> area, bool ticked, bool enabled, bool) override
    {
        const auto size = jmin (area.getWidth(), area.getHeight());
        const auto box = area.withSizeKeepingCentre (size, size).reduced (1.0f);

        g.setColour (Palette::panel);
        g.fillRoundedRectangle (box, 3.0f);
        drawNeon (g, box, 3.0f, enabled ? Palette::cyan : Palette::textDim, 0.6f);

        if (ticked)
        {
            g.setColour (Palette::magenta.withAlpha (0.3f));
            g.fillRoundedRectangle (box.reduced (2.0f).expanded (1.5f), 3.0f);
            g.setColour (Palette::magenta);
            g.fillRoundedRectangle (box.reduced (box.getWidth() * 0.28f), 2.0f);
        }
    }

private:
    static constexpr float padding = 14.0f;
    static constexpr float headerHeight = 58.0f;
    static inline const Colour bodyColour { 0xffd6f6ff };

    ThemeMetrics metrics;
    Image background;
};

std::unique_ptr<Theme> createNeonTheme()
{
    return std::make_unique<NeonTheme>();
}

} // namespace mfx::ui
