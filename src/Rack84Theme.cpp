#include "Themes.h"

// RACK '84: charcoal brushed-metal rack gear with rack screws, cream silkscreen, round LEDs,
// chunky bevelled keys and a green-phosphor LCD.
namespace mfx::ui
{
namespace
{
    struct Palette
    {
        static inline const Colour plateTop        { 0xff3b3d41 };
        static inline const Colour plateBottom     { 0xff26282b };
        static inline const Colour plateHighlight  { 0xff7a7d82 };
        static inline const Colour plateShadow     { 0xff0c0d0e };
        static inline const Colour rackInterior    { 0xff141517 };
        static inline const Colour moduleTop       { 0xff484b50 };
        static inline const Colour moduleBottom    { 0xff323438 };
        static inline const Colour buttonFace      { 0xff55585d };
        static inline const Colour buttonFaceOn    { 0xff3a3c40 };
        static inline const Colour fieldBackground { 0xff16171a };
        static inline const Colour cream           { 0xffeadfc2 };
        static inline const Colour creamDim        { 0xffb0a88f };
        static inline const Colour orange          { 0xffe8742a };
        static inline const Colour ledGreen        { 0xff3dff71 };
        static inline const Colour ledAmber        { 0xffffb224 };
        static inline const Colour ledRed          { 0xffff3d33 };
        static inline const Colour failureText     { 0xffff9a90 };
        static inline const Colour segmentLit      { 0xffff8a3a };
        static inline const Colour lcdBackground   { 0xff0a130b };
        static inline const Colour lcdText         { 0xff86f07c };
        static inline const Colour lcdBright       { 0xffc2ffb8 };
        static inline const Colour lcdDim          { 0xff5cb556 };
        static inline const Colour stripBackground { 0xff1a0f06 };
        static inline const Colour stripText       { 0xffff9d45 };
        static inline const Colour stripDim        { 0xffb88252 };
    };

    constexpr float corner = 3.0f;
    constexpr float lcdBezel = 6.0f;
    constexpr float lcdPadding = 12.0f;
    constexpr float lcdHeaderHeight = 50.0f;

    FontOptions silkscreen (float height)
    {
        return FontOptions ("Helvetica Neue", height, Font::bold).withKerningFactor (0.18f);
    }

    FontOptions mono (float height, bool bold = false)
    {
        return FontOptions ("Menlo", height, bold ? Font::bold : Font::plain);
    }

    void paintBevelBox (Graphics& g, Rectangle<float> r, float radius, Colour faceTop, Colour faceBottom, bool sunken)
    {
        g.setGradientFill (ColourGradient (faceTop, r.getX(), r.getY(), faceBottom, r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, radius);

        const auto light = Palette::plateHighlight.withAlpha (0.75f);
        const auto dark = Palette::plateShadow.withAlpha (0.9f);

        g.setGradientFill (ColourGradient (sunken ? dark : light, r.getTopLeft(),
                                           sunken ? light : dark, r.getBottomRight(), false));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    }

    void paintLed (Graphics& g, Rectangle<float> area, SlotState state)
    {
        const auto colour = state == SlotState::active ? Palette::ledGreen
                          : state == SlotState::bypassed ? Palette::ledAmber
                                                         : Palette::ledRed;

        g.setGradientFill (ColourGradient (colour.withAlpha (0.35f), area.getCentre(), colour.withAlpha (0.0f),
                                           area.getCentre().translated (area.getWidth() * 1.1f, 0.0f), true));
        g.fillEllipse (area.expanded (area.getWidth() * 0.9f));

        g.setColour (Palette::plateShadow);
        g.fillEllipse (area);
        g.setColour (Palette::plateHighlight.withAlpha (0.5f));
        g.drawEllipse (area.expanded (0.5f), 1.0f);

        const auto lens = area.reduced (area.getWidth() * 0.14f);
        g.setGradientFill (ColourGradient (colour.brighter (0.7f), lens.getX() + lens.getWidth() * 0.35f, lens.getY() + lens.getHeight() * 0.3f,
                                           colour.darker (0.35f), lens.getRight(), lens.getBottom(), true));
        g.fillEllipse (lens);

        g.setColour (Colours::white.withAlpha (0.7f));
        g.fillEllipse (lens.getX() + lens.getWidth() * 0.2f, lens.getY() + lens.getHeight() * 0.14f,
                       lens.getWidth() * 0.28f, lens.getHeight() * 0.2f);
    }

    void paintScrew (Graphics& g, Point<float> centre, float radius, float slotAngle)
    {
        const auto head = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);

        g.setColour (Colours::black.withAlpha (0.55f));
        g.fillEllipse (head.translated (1.0f, 1.2f));

        g.setGradientFill (ColourGradient (Colour (0xffd2d4d8), head.getX() + radius * 0.6f, head.getY() + radius * 0.5f,
                                           Colour (0xff62656a), head.getRight(), head.getBottom(), true));
        g.fillEllipse (head);
        g.setColour (Colours::black.withAlpha (0.6f));
        g.drawEllipse (head, 0.8f);

        const auto reach = radius * 0.78f;
        const Point<float> offset (std::cos (slotAngle) * reach, std::sin (slotAngle) * reach);

        g.setColour (Colour (0xff222325));
        g.drawLine ({ centre - offset, centre + offset }, 2.0f);
        g.setColour (Colours::white.withAlpha (0.35f));
        g.drawLine ({ centre - offset + Point<float> (0.0f, 1.4f), centre + offset + Point<float> (0.0f, 1.4f) }, 0.8f);
    }

    // Segment bits: a top, b upper right, c lower right, d bottom, e lower left, f upper left, g middle.
    constexpr int segmentMasks[10] = { 0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
                                       0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111 };

    Path horizontalBar (float x0, float x1, float y, float t)
    {
        Path p;
        p.startNewSubPath (x0, y);
        p.lineTo (x0 + t * 0.5f, y - t * 0.5f);
        p.lineTo (x1 - t * 0.5f, y - t * 0.5f);
        p.lineTo (x1, y);
        p.lineTo (x1 - t * 0.5f, y + t * 0.5f);
        p.lineTo (x0 + t * 0.5f, y + t * 0.5f);
        p.closeSubPath();
        return p;
    }

    Path verticalBar (float x, float y0, float y1, float t)
    {
        Path p;
        p.startNewSubPath (x, y0);
        p.lineTo (x + t * 0.5f, y0 + t * 0.5f);
        p.lineTo (x + t * 0.5f, y1 - t * 0.5f);
        p.lineTo (x, y1);
        p.lineTo (x - t * 0.5f, y1 - t * 0.5f);
        p.lineTo (x - t * 0.5f, y0 + t * 0.5f);
        p.closeSubPath();
        return p;
    }

    void paintDigit (Graphics& g, Rectangle<float> r, int digit)
    {
        const auto t = r.getWidth() * 0.2f;
        const auto inset = t * 0.65f;
        const auto left = r.getX() + t * 0.5f, right = r.getRight() - t * 0.5f;
        const auto top = r.getY() + t * 0.5f, bottom = r.getBottom() - t * 0.5f, middle = r.getCentreY();

        const Path segments[7] = {
            horizontalBar (left + inset, right - inset, top, t),
            verticalBar (right, top + inset, middle - inset * 0.3f, t),
            verticalBar (right, middle + inset * 0.3f, bottom - inset, t),
            horizontalBar (left + inset, right - inset, bottom, t),
            verticalBar (left, middle + inset * 0.3f, bottom - inset, t),
            verticalBar (left, top + inset, middle - inset * 0.3f, t),
            horizontalBar (left + inset, right - inset, middle, t) };

        for (int i = 0; i < 7; ++i)
        {
            const auto lit = (segmentMasks[digit] >> i & 1) != 0;
            g.setColour (lit ? Palette::segmentLit : Palette::segmentLit.withAlpha (0.1f));
            g.fillPath (segments[i]);
        }
    }

    // Two seven-segment digits on a dark cell; unlit segments stay faintly visible.
    void paintSlotNumber (Graphics& g, Rectangle<float> cell, int number)
    {
        paintBevelBox (g, cell, 2.0f, Palette::lcdBackground.darker (0.5f), Palette::stripBackground, true);

        number = jlimit (0, 99, number);
        auto inner = cell.reduced (5.0f, 4.0f);
        const auto digitWidth = (inner.getWidth() - 3.0f) * 0.5f;

        paintDigit (g, inner.removeFromLeft (digitWidth), number / 10);
        inner.removeFromLeft (3.0f);
        paintDigit (g, inner, number % 10);
    }

    Image makeBrushedTexture (int width, int height)
    {
        Image image (Image::ARGB, jmax (1, width), jmax (1, height), true);
        Graphics g (image);

        // Fixed seeds: the grain must not change from one repaint to the next.
        for (int y = 0; y < height; ++y)
        {
            Random random (y * 7919 + 17);

            g.setColour ((random.nextBool() ? Colours::white : Colours::black).withAlpha (random.nextFloat() * 0.07f));
            g.fillRect (0, y, width, 1);

            for (int streak = 0; streak < 4; ++streak)
            {
                g.setColour ((random.nextBool() ? Colours::white : Colours::black).withAlpha (0.03f + random.nextFloat() * 0.05f));
                g.fillRect (random.nextInt (jmax (1, width)), y, 20 + random.nextInt (220), 1);
            }
        }

        return image;
    }
}

class Rack84Theme final : public Theme
{
public:
    Rack84Theme()
    {
        setDefaultSansSerifTypefaceName ("Helvetica Neue");
        applyScheme ({ Palette::plateBottom, Palette::cream, Palette::creamDim, Palette::fieldBackground,
                       Palette::buttonFace, Palette::buttonFaceOn, Palette::orange, Colour (0xff15120e),
                       Palette::cream, Palette::ledAmber });
        setColour (ComboBox::arrowColourId, Palette::ledAmber);
    }

    const ThemeMetrics& getMetrics() const override { return metrics; }

    FontOptions getUiFont (float height, bool bold) const override
    {
        return FontOptions ("Helvetica Neue", height, bold ? Font::bold : Font::plain);
    }

    //==============================================================================
    void paintWindow (Graphics& g, Rectangle<int> bounds) override
    {
        if (brushed.getWidth() != bounds.getWidth() || brushed.getHeight() != bounds.getHeight())
            brushed = makeBrushedTexture (bounds.getWidth(), bounds.getHeight());

        const auto r = bounds.toFloat();

        g.setGradientFill (ColourGradient (Palette::plateTop, 0.0f, r.getY(), Palette::plateBottom, 0.0f, r.getBottom(), false));
        g.fillRect (r);
        g.drawImageAt (brushed, bounds.getX(), bounds.getY());

        // Thin lighter bevel on the outer edge, and a groove that frames the working area.
        g.setColour (Palette::plateHighlight);
        g.fillRect (r.getX(), r.getY(), r.getWidth(), 1.0f);
        g.fillRect (r.getX(), r.getY(), 1.0f, r.getHeight());
        g.setColour (Palette::plateShadow);
        g.fillRect (r.getX(), r.getBottom() - 1.0f, r.getWidth(), 1.0f);
        g.fillRect (r.getRight() - 1.0f, r.getY(), 1.0f, r.getHeight());

        const auto groove = r.reduced ((float) metrics.margin - 4.0f);
        g.setColour (Palette::plateShadow.withAlpha (0.8f));
        g.drawRoundedRectangle (groove, 4.0f, 1.0f);
        g.setColour (Palette::plateHighlight.withAlpha (0.25f));
        g.drawRoundedRectangle (groove.translated (1.0f, 1.0f), 4.0f, 1.0f);

        const auto inset = (float) metrics.margin * 0.5f;
        paintScrew (g, { r.getX() + inset, r.getY() + inset }, 6.0f, 0.35f);
        paintScrew (g, { r.getRight() - inset, r.getY() + inset }, 6.0f, 1.2f);
        paintScrew (g, { r.getX() + inset, r.getBottom() - inset }, 6.0f, 2.4f);
        paintScrew (g, { r.getRight() - inset, r.getBottom() - inset }, 6.0f, 0.8f);
    }

    void paintLogo (Graphics& g, Rectangle<int> strip, const String& productName, const String& subtitle) override
    {
        auto stripes = strip.removeFromBottom (11).toFloat();

        g.setColour (Palette::orange);
        g.fillRect (stripes.getX(), stripes.getY(), stripes.getWidth(), 3.0f);
        g.setColour (Palette::cream.withAlpha (0.85f));
        g.fillRect (stripes.getX(), stripes.getY() + 5.0f, stripes.getWidth(), 1.5f);
        g.setColour (Palette::orange.withAlpha (0.6f));
        g.fillRect (stripes.getX(), stripes.getY() + 8.0f, stripes.getWidth(), 1.0f);

        const auto logo = FontOptions ("Futura", 34.0f, Font::bold).withKerningFactor (0.14f);
        drawText (g, productName, strip.translated (1, 2).toFloat(), Justification::centredLeft, logo, Colours::black.withAlpha (0.5f));
        drawText (g, productName, strip.toFloat(), Justification::centredLeft, logo, Palette::cream);
        drawText (g, subtitle, strip.translated (0, 13).toFloat(), Justification::centredRight, silkscreen (11.5f), Palette::creamDim);
    }

    void paintPanel (Graphics& g, Rectangle<int> title, Rectangle<int> frame, const String& text, PanelKind) override
    {
        const auto font = silkscreen (10.5f);
        drawText (g, text, title.toFloat(), Justification::centredLeft, font, Palette::creamDim);

        const auto lineX = (float) title.getX() + textWidth (font, text) + 10.0f;
        const auto y = (float) title.getCentreY();

        g.setColour (Palette::creamDim.withAlpha (0.45f));
        g.fillRect (lineX, y - 1.0f, (float) title.getRight() - lineX, 1.0f);
        g.fillRect (lineX, y + 1.0f, (float) title.getRight() - lineX, 1.0f);

        const auto r = frame.toFloat();
        paintBevelBox (g, r, corner, Palette::rackInterior, Palette::rackInterior, true);
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.55f), r.getX(), r.getY(),
                                           Colours::transparentBlack, r.getX(), r.getY() + 10.0f, false));
        g.fillRoundedRectangle (r.reduced (1.0f), corner);
    }

    //==============================================================================
    void paintSlotRow (Graphics& g, const SlotRowInfo& info, const SlotRowGeometry& geometry) override
    {
        const auto r = geometry.module.toFloat();

        g.setColour (Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.5f), corner);
        paintBevelBox (g, r, corner, Palette::moduleTop, Palette::moduleBottom, false);

        g.setColour (Colours::white.withAlpha (0.025f));

        for (auto y = r.getY() + 3.0f; y < r.getBottom() - 2.0f; y += 3.0f)
            g.fillRect (r.getX() + 2.0f, y, r.getWidth() - 4.0f, 1.0f);

        paintScrew (g, { r.getX() + 9.0f, r.getCentreY() }, 4.0f, 0.5f);
        paintScrew (g, { r.getRight() - 9.0f, r.getCentreY() }, 4.0f, 2.2f);

        paintSlotNumber (g, geometry.number.toFloat(), info.number);
        paintLed (g, geometry.indicator.toFloat().withSizeKeepingCentre (14.0f, 14.0f), info.state);

        const auto font = getUiFont (14.0f, true);
        auto area = geometry.name.toFloat();

        if (info.failureTag.isNotEmpty())
        {
            drawText (g, fitText (font, info.name, area.getWidth()), area.removeFromTop (area.getHeight() / 2.0f),
                      Justification::bottomLeft, font, Palette::cream);
            drawText (g, info.failureTag, area, Justification::topLeft, getUiFont (11.5f, true), Palette::failureText);
        }
        else
        {
            drawText (g, fitText (font, info.name, area.getWidth()), area, Justification::centredLeft, font, Palette::cream);
        }
    }

    //==============================================================================
    int measureReadout (int width, const ReadoutContent& content) const override
    {
        const BodyText body (content.body, mono (12.0f), Palette::lcdText, 1.5f, (float) width - 2.0f * (lcdBezel + lcdPadding));
        return (int) std::ceil (2.0f * (lcdBezel + lcdPadding) + lcdHeaderHeight + body.getHeight());
    }

    void paintReadout (Graphics& g, Rectangle<int> bounds, const ReadoutContent& content) const override
    {
        paintBevelBox (g, bounds.toFloat(), 5.0f, Palette::moduleTop, Palette::moduleBottom, true);

        const auto glass = bounds.toFloat().reduced (lcdBezel);
        g.setGradientFill (ColourGradient (Palette::lcdBackground.brighter (0.08f), glass.getX(), glass.getY(),
                                           Palette::lcdBackground.darker (0.3f), glass.getX(), glass.getBottom(), false));
        g.fillRoundedRectangle (glass, 3.0f);

        {
            Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (glass.toNearestInt());

            auto inner = glass.reduced (lcdPadding);
            auto header = inner.removeFromTop (lcdHeaderHeight);

            drawText (g, content.caption, header.removeFromTop (13.0f), Justification::centredLeft, mono (10.5f, true), Palette::lcdDim);

            const auto bigFont = mono (32.0f, true);
            const auto bigWidth = textWidth (bigFont, content.bigValue);
            drawText (g, content.bigValue, header.withWidth (bigWidth + 4.0f), Justification::centredLeft, bigFont, Palette::lcdBright);
            drawText (g, content.unit, header.withTrimmedLeft (bigWidth + 8.0f), Justification::centredLeft, mono (16.0f, true), Palette::lcdText);

            BodyText (content.body, mono (12.0f), Palette::lcdText, 1.5f, inner.getWidth()).draw (g, inner);

            // Faint scanlines over the glass; kept light so the text stays legible.
            drawScanlines (g, glass, 3.0f, 0.16f);
            g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.05f), glass.getX(), glass.getY(),
                                               Colours::transparentWhite, glass.getX(), glass.getY() + glass.getHeight() * 0.4f, false));
            g.fillRect (glass);
        }

        g.setColour (Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (glass.expanded (0.5f), 3.5f, 1.2f);
    }

    void paintStatus (Graphics& g, Rectangle<int> bounds, const String& text) const override
    {
        paintBevelBox (g, bounds.toFloat(), corner, Palette::moduleTop, Palette::moduleBottom, true);

        const auto glass = bounds.toFloat().reduced (4.0f);
        g.setColour (Palette::stripBackground);
        g.fillRoundedRectangle (glass, 2.0f);

        Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (glass.toNearestInt());

        auto content = glass.reduced (8.0f, 2.0f);
        drawText (g, "STATUS", content.removeFromLeft (56.0f), Justification::centredLeft, mono (10.5f, true), Palette::stripDim);

        AttributedString attributed;
        attributed.setJustification (Justification::centredLeft);
        attributed.setLineSpacing (1.0f);
        attributed.append (text, mono (12.0f), Palette::stripText);
        attributed.draw (g, content);

        drawScanlines (g, glass, 3.0f, 0.14f);
    }

    //==============================================================================
    void paintControlFace (Graphics& g, Rectangle<float> r, Colour face, const ControlState& state) override
    {
        if (! state.enabled)
            face = face.withMultipliedBrightness (0.7f);
        else if (state.hover && ! state.pressed())
            face = face.brighter (0.1f);

        if (! state.pressed())
        {
            g.setColour (Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle (r.translated (0.0f, 1.5f), corner);
        }

        if (state.pressed())
            paintBevelBox (g, r, corner, face.darker (0.45f), face.darker (0.1f), true);
        else
            paintBevelBox (g, r, corner, face.brighter (0.2f), face.darker (0.25f), false);
    }

    void paintFieldFace (Graphics& g, Rectangle<float> r, bool) override
    {
        paintBevelBox (g, r.reduced (0.5f), corner, Palette::fieldBackground.darker (0.3f), Palette::fieldBackground, true);
    }

    void paintTickBox (Graphics& g, Rectangle<float> area, bool ticked, bool enabled, bool) override
    {
        const auto box = area.withSizeKeepingCentre (jmin (area.getWidth(), area.getHeight()), jmin (area.getWidth(), area.getHeight())).reduced (1.0f);
        paintBevelBox (g, box, 2.0f, Palette::fieldBackground.darker (0.4f), Palette::fieldBackground, true);

        if (ticked)
        {
            const auto lamp = box.reduced (box.getWidth() * 0.27f);
            const auto colour = enabled ? Palette::ledAmber : Palette::creamDim.withAlpha (0.4f);

            g.setColour (colour.withAlpha (0.3f));
            g.fillRoundedRectangle (lamp.expanded (2.0f), 2.5f);
            g.setGradientFill (ColourGradient (colour.brighter (0.5f), lamp.getX(), lamp.getY(),
                                               colour.darker (0.2f), lamp.getRight(), lamp.getBottom(), false));
            g.fillRoundedRectangle (lamp, 1.5f);
        }
    }

private:
    ThemeMetrics metrics;
    Image brushed;
};

std::unique_ptr<Theme> createRack84Theme()
{
    return std::make_unique<Rack84Theme>();
}

} // namespace mfx::ui
