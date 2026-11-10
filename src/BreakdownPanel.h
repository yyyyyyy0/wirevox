#pragma once

#include "Breakdown.h"
#include "ProcessTime.h"
#include "Text.h"
#include "Theme.h"

#include <optional>
#include <vector>

namespace mfx::ui
{
using namespace juce;

// The processing time of one slot, as the panel lists it.
struct SlotLoadView
{
    int slot = 0;
    String name;
    bool bypassed = false;
    ProcessStats stats;
    bool heaviest = false;
};

// Everything the panel shows. The numbers are worked out elsewhere (Breakdown.cpp, ProcessTime.cpp);
// the panel only lays them out.
struct BreakdownView
{
    bool deviceOpen = false;
    double sampleRate = 0.0;
    double cycleUsage = 1.0;
    double bufferMs = 0.0;                      // one buffer, which the processing times are shares of
    Breakdown latency;
    std::optional<ExcessRecord> excess;         // when and how the measured row was taken
    std::vector<SlotLoadView> loads;
};

// The "遅延の内訳" panel: every part of the latency with its samples and milliseconds, the largest
// one marked, what the output device declares told apart from what was measured, and below it how much
// of a buffer each plugin takes. Paints with the colours and fonts of the current theme, so it works in
// all of them; the frame around it is the theme's own panel. The rows scroll when the panel is short.
class BreakdownPanel final : public Component
{
public:
    BreakdownPanel();
    ~BreakdownPanel() override;

    void setView (const BreakdownView&);

    // Height of the whole panel when nothing has to scroll.
    int getPreferredHeight (int width) const;

    // The button starts a measurement of the output device; the owner reacts to onMeasure and says
    // whether it may be pressed and what it reads.
    TextButton measureButton { mfx::ja ("仮想デバイスを計測") };
    std::function<void()> onMeasure;
    void setMeasuring (bool running, bool allowed);

    void resized() override;
    void lookAndFeelChanged() override;

private:
    class Content;

    Label hint;
    Viewport viewport;
    std::unique_ptr<Content> content;
};

} // namespace mfx::ui
