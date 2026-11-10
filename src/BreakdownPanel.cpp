#include "BreakdownPanel.h"
#include "Text.h"

#include <algorithm>
#include <cmath>

namespace mfx::ui
{

namespace
{
    constexpr int rowHeight = 18;
    constexpr int sectionHeight = 24;
    constexpr int topRowHeight = 30;
    constexpr int insetX = 8;
    constexpr int tagWidth = 32;
    constexpr int samplesWidth = 64;
    constexpr int msWidth = 68;
    constexpr int chipWidth = 78;
    constexpr int gap = 8;
    constexpr int maxBarWidth = 150;
    constexpr int minBarWidth = 40;
    constexpr int minLabelWidth = 96;

    struct Colours4
    {
        Colour text, dim, accent, onAccent, ground;
    };

    Colours4 coloursOf (Component& c)
    {
        const auto text = c.findColour (Label::textColourId);
        return { text, text.withMultipliedAlpha (0.62f), c.findColour (PopupMenu::highlightedBackgroundColourId),
                 c.findColour (PopupMenu::highlightedTextColourId), c.findColour (ResizableWindow::backgroundColourId) };
    }

    String formatMs (double ms)
    {
        return String (ms, 2) + " ms";
    }

    String formatPercent (double percent)
    {
        return String (percent, percent < 10.0 ? 2 : 1) + " %";
    }

    // The part of the row that lines up from one row to the next.
    struct Columns
    {
        Rectangle<int> tag, label, samples, ms, bar, chip;
    };

    Columns layoutColumns (Rectangle<int> row)
    {
        Columns c;
        row.removeFromLeft (insetX);
        row.removeFromRight (insetX);

        c.tag = row.removeFromLeft (tagWidth);
        row.removeFromLeft (gap);
        c.chip = row.removeFromRight (chipWidth);

        // The bar is the first thing to go when the window is narrow; the numbers and the name stay.
        const auto room = row.getWidth() - minLabelWidth - gap - samplesWidth - 6 - msWidth - gap;
        const auto barWidth = room >= minBarWidth ? jmin (maxBarWidth, room) : 0;

        c.bar = row.removeFromRight (barWidth);

        if (barWidth > 0)
            row.removeFromRight (gap);

        c.ms = row.removeFromRight (msWidth);
        row.removeFromRight (6);
        c.samples = row.removeFromRight (samplesWidth);
        row.removeFromRight (gap);
        c.label = row;
        return c;
    }

    void paintBar (Graphics& g, Rectangle<int> area, double fraction, Colour fill, Colour track, bool hatched)
    {
        if (area.isEmpty())
            return;

        const auto bar = area.withSizeKeepingCentre (area.getWidth(), 8).toFloat();
        g.setColour (track);
        g.drawRect (bar, 1.0f);

        const auto filled = bar.reduced (1.0f).withWidth (jmax (0.0f, (bar.getWidth() - 2.0f) * (float) jlimit (0.0, 1.0, fraction)));

        if (filled.getWidth() <= 0.0f)
            return;

        g.setColour (fill);

        if (! hatched)
        {
            g.fillRect (filled);
            return;
        }

        // A measured value is drawn as stripes, a declared one as a solid bar: they are told apart without colour.
        Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (filled.toNearestInt());

        for (auto x = filled.getX() - filled.getHeight(); x < filled.getRight(); x += 3.0f)
            g.drawLine (x, filled.getBottom(), x + filled.getHeight(), filled.getY(), 1.0f);
    }

    void paintChip (Graphics& g, Rectangle<int> area, const String& text, const FontOptions& font, Colour fill, Colour ink)
    {
        const auto chip = area.withSizeKeepingCentre (jmin (area.getWidth(), chipWidth - 4), rowHeight - 4).toFloat();
        g.setColour (fill);
        g.fillRect (chip);
        drawText (g, text, chip, Justification::centred, font, ink);
    }

    // A small tag that says where a number comes from: 申告 (declared by a device) or 実測 (measured).
    void paintSourceTag (Graphics& g, Rectangle<int> area, bool measured, bool known, const FontOptions& font, const Colours4& colours)
    {
        const auto tag = area.withSizeKeepingCentre (area.getWidth() - 2, rowHeight - 4).toFloat();

        if (measured && known)
        {
            g.setColour (colours.text);
            g.fillRect (tag);
            drawText (g, ja ("実測"), tag, Justification::centred, font, colours.ground);
        }
        else
        {
            g.setColour (measured ? colours.dim.withMultipliedAlpha (0.8f) : colours.dim);
            g.drawRect (tag, 1.0f);
            drawText (g, measured ? ja ("実測") : ja ("申告"), tag, Justification::centred, font, colours.dim);
        }
    }
}

//==============================================================================
class BreakdownPanel::Content final : public Component
{
public:
    void setView (const BreakdownView& newView)
    {
        view = newView;
        repaint();
    }

    int getPreferredHeight() const
    {
        int height = 4;

        if (! view.deviceOpen)
            return height + rowHeight * 2 + 4;

        height += sectionHeight;
        height += (int) view.latency.rows.size() * rowHeight;
        height += rowHeight + 6;                         // total
        height += noteLines().size() * rowHeight;
        height += sectionHeight;
        height += (int) jmax (1, (int) view.loads.size()) * rowHeight;
        return height + 6;
    }

    void paint (Graphics& g) override
    {
        auto* theme = findTheme (*this);

        if (theme == nullptr)
            return;

        const auto colours = coloursOf (*this);
        const auto font = theme->getUiFont (12.0f, false);
        const auto bold = theme->getUiFont (12.0f, true);
        const auto small = theme->getUiFont (10.5f, true);
        auto area = getLocalBounds();
        area.removeFromTop (4);

        if (! view.deviceOpen)
        {
            drawText (g, ja ("オーディオデバイスが開いていません。左のパネルで入力・出力デバイスを選ぶと、遅延の内訳が出ます。"),
                      area.removeFromTop (rowHeight * 2).reduced (insetX, 0).toFloat(), Justification::topLeft, font, colours.dim);
            return;
        }

        paintSection (g, area.removeFromTop (sectionHeight), ja ("遅延の内訳（入力の取り込みから出力デバイスまで）"), bold, colours);

        double largestMs = 0.0;

        for (auto& row : view.latency.rows)
            if (row.known)
                largestMs = jmax (largestMs, row.ms);

        for (auto& row : view.latency.rows)
            paintLatencyRow (g, area.removeFromTop (rowHeight), row, largestMs, font, bold, small, colours);

        paintTotal (g, area.removeFromTop (rowHeight + 6), bold, colours);

        for (auto& note : noteLines())
            drawText (g, fitText (small, note, (float) getWidth() - 2.0f * insetX), area.removeFromTop (rowHeight).reduced (insetX, 0).toFloat(),
                      Justification::centredLeft, small, colours.dim);

        paintSection (g, area.removeFromTop (sectionHeight),
                      ja ("プラグインの処理時間（1 バッファ = ") + formatMs (view.bufferMs) + ja (" に対する割合）"), bold, colours);

        if (view.loads.empty())
            drawText (g, ja ("プラグインがありません"), area.removeFromTop (rowHeight).reduced (insetX, 0).toFloat(), Justification::centredLeft, font, colours.dim);

        for (auto& load : view.loads)
            paintLoadRow (g, area.removeFromTop (rowHeight), load, font, bold, small, colours);
    }

private:
    StringArray noteLines() const
    {
        StringArray notes;

        if (view.excess.has_value())
        {
            auto text = ja ("実測: ") + view.excess->outputDevice + " / " + String (view.excess->sampleRate, 0) + " Hz / " + String (view.excess->runs)
                      + ja (" 回の中央値");

            if (view.excess->measuredAt.toMilliseconds() > 0)
                text << ja ("（") << view.excess->measuredAt.formatted ("%Y-%m-%d %H:%M") << ja ("）");

            notes.add (text);
        }
        else
        {
            notes.add (ja ("Loopback などの仮想デバイスは遅延を 0 と申告します。右上のボタンで、足している分を測れます"));
        }

        if (view.cycleUsage < 0.999)
            notes.add (ja ("IO サイクル使用率 ") + String (view.cycleUsage, 2) + ja ("：出力バッファは ") + String (view.cycleUsage, 2) + ja (" 倍で数えています"));

        return notes;
    }

    void paintSection (Graphics& g, Rectangle<int> area, const String& text, const FontOptions& bold, const Colours4& colours) const
    {
        drawText (g, fitText (bold, text, (float) area.getWidth() - 2.0f * insetX), area.reduced (insetX, 0).withTrimmedTop (4).toFloat(),
                  Justification::centredLeft, bold, colours.text);
        g.setColour (colours.dim.withMultipliedAlpha (0.5f));
        g.fillRect (area.getX() + insetX, area.getBottom() - 1, area.getWidth() - 2 * insetX, 1);
    }

    void paintLatencyRow (Graphics& g, Rectangle<int> row, const BreakdownRow& r, double largestMs, const FontOptions& font,
                          const FontOptions& bold, const FontOptions& small, const Colours4& colours) const
    {
        const auto c = layoutColumns (row);
        const auto showsValue = r.known;

        if (r.isLargest)
        {
            g.setColour (colours.accent.withAlpha (0.16f));
            g.fillRect (row);
        }

        paintSourceTag (g, c.tag, r.measured, r.known, small, colours);

        const auto labelFont = r.isLargest ? bold : font;
        const auto ink = showsValue ? colours.text : colours.dim;
        drawText (g, fitText (labelFont, r.label, (float) c.label.getWidth()), c.label.toFloat(), Justification::centredLeft, labelFont, r.bypassed ? colours.dim : ink);

        if (showsValue)
        {
            drawText (g, String (r.samples) + " smp", c.samples.toFloat(), Justification::centredRight, labelFont, ink);
            drawText (g, formatMs (r.ms), c.ms.toFloat(), Justification::centredRight, labelFont, ink);
            paintBar (g, c.bar, largestMs > 0.0 ? r.ms / largestMs : 0.0, r.isLargest ? colours.accent : colours.text.withMultipliedAlpha (0.55f),
                      colours.dim.withMultipliedAlpha (0.6f), r.measured);
        }
        else
        {
            drawText (g, "--", c.samples.toFloat(), Justification::centredRight, font, colours.dim);
            drawText (g, ja ("未計測"), c.ms.toFloat(), Justification::centredRight, font, colours.dim);
            paintBar (g, c.bar, 0.0, colours.dim, colours.dim.withMultipliedAlpha (0.4f), true);
        }

        if (r.isLargest)
            paintChip (g, c.chip, ja ("ここが最大"), small, colours.accent, colours.onAccent);
    }

    void paintTotal (Graphics& g, Rectangle<int> row, const FontOptions& bold, const Colours4& colours) const
    {
        g.setColour (colours.text.withMultipliedAlpha (0.8f));
        g.fillRect (row.getX() + insetX, row.getY() + 2, row.getWidth() - 2 * insetX, 1);
        row.removeFromTop (6);

        const auto c = layoutColumns (row);
        const auto excessKnown = ! view.latency.rows.empty() && view.latency.rows.back().known;
        const auto totalSamples = view.latency.declaredSamples + (excessKnown ? view.latency.rows.back().samples : 0);

        drawText (g, excessKnown ? ja ("合計") : ja ("合計（出力デバイスの超過分を除く）"),
                  Rectangle<int> (c.tag.getX(), row.getY(), c.label.getRight() - c.tag.getX(), row.getHeight()).toFloat(),
                  Justification::centredLeft, bold, colours.text);
        drawText (g, String (totalSamples) + " smp", c.samples.toFloat(), Justification::centredRight, bold, colours.text);
        drawText (g, formatMs (view.latency.totalMs), c.ms.toFloat(), Justification::centredRight, bold, colours.text);
    }

    void paintLoadRow (Graphics& g, Rectangle<int> row, const SlotLoadView& load, const FontOptions& font, const FontOptions& bold,
                       const FontOptions& small, const Colours4& colours) const
    {
        const auto c = layoutColumns (row);

        if (load.heaviest)
        {
            g.setColour (colours.accent.withAlpha (0.16f));
            g.fillRect (row);
        }

        const auto labelFont = load.heaviest ? bold : font;
        const auto label = ja ("プラグイン ") + String (load.slot + 1) + ": " + load.name;
        const auto live = load.stats.valid && ! load.bypassed;
        const auto ink = live ? colours.text : colours.dim;

        const auto text = Rectangle<int> (c.tag.getX(), row.getY(), c.label.getRight() - c.tag.getX(), row.getHeight());
        drawText (g, fitText (labelFont, label, (float) text.getWidth()), text.toFloat(), Justification::centredLeft, labelFont, ink);

        // The two columns are the mean and the worst block; both are shares of one buffer.
        if (live)
        {
            drawText (g, ja ("平均 ") + formatPercent (load.stats.meanPercent), c.samples.withWidth (c.samples.getWidth() + 30).withX (c.samples.getX() - 30).toFloat(),
                      Justification::centredRight, labelFont, ink);
            drawText (g, ja ("最大 ") + formatPercent (load.stats.maxPercent), c.ms.withWidth (c.ms.getWidth() + 22).withX (c.ms.getX() - 22).toFloat(),
                      Justification::centredRight, labelFont, ink);
            paintBar (g, c.bar, load.stats.maxPercent / 100.0, load.heaviest ? colours.accent : colours.text.withMultipliedAlpha (0.55f),
                      colours.dim.withMultipliedAlpha (0.6f), false);
        }
        else
        {
            drawText (g, load.bypassed ? ja ("バイパス中") : ja ("--"), c.ms.withWidth (c.ms.getWidth() + 22).withX (c.ms.getX() - 22).toFloat(),
                      Justification::centredRight, font, colours.dim);
        }

        if (load.heaviest)
            paintChip (g, c.chip, ja ("ここが重い"), small, colours.accent, colours.onAccent);
    }

    BreakdownView view;
};

//==============================================================================
BreakdownPanel::BreakdownPanel()
    : content (std::make_unique<Content>())
{
    measureButton.onClick = [this] { if (onMeasure) onMeasure(); };
    measureButton.setTooltip (ja ("出力デバイス（Loopback など）が足している遅延を、試験音を流して測ります。数秒かかり、その間マイクの音は止まります"));
    addAndMakeVisible (measureButton);

    hint.setText (ja ("計測中はマイクの音が止まり、出力に試験音が流れます"), dontSendNotification);
    hint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (hint);

    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

BreakdownPanel::~BreakdownPanel() = default;

void BreakdownPanel::setView (const BreakdownView& view)
{
    content->setView (view);
    content->setSize (jmax (1, viewport.getMaximumVisibleWidth()), jmax (content->getPreferredHeight(), 1));
}

int BreakdownPanel::getPreferredHeight (int) const
{
    return topRowHeight + content->getPreferredHeight();
}

void BreakdownPanel::setMeasuring (bool running, bool allowed)
{
    measureButton.setButtonText (running ? ja ("中止") : ja ("仮想デバイスを計測"));
    measureButton.setEnabled (running || allowed);
}

void BreakdownPanel::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop (topRowHeight).reduced (insetX, 2);
    measureButton.setBounds (top.removeFromLeft (jmin (170, top.getWidth())).withSizeKeepingCentre (jmin (170, top.getWidth()), 26));
    top.removeFromLeft (10);
    hint.setBounds (top);

    viewport.setBounds (area);
    content->setSize (jmax (1, viewport.getMaximumVisibleWidth()), jmax (content->getPreferredHeight(), 1));
}

void BreakdownPanel::lookAndFeelChanged()
{
    if (auto* theme = findTheme (*this))
    {
        hint.setFont (theme->getUiFont (10.5f, false));
        hint.setColour (Label::textColourId, findColour (Label::textColourId).withMultipliedAlpha (0.62f));
    }

    resized();
}

} // namespace mfx::ui
