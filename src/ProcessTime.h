#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mfx
{

// How long one plugin's processBlock takes. The chain puts a probe in front of and behind every slot
// that is in the signal path; the first calls begin(), the second end(), both on the audio thread.
// They take no lock, allocate nothing and log nothing: a clock read and a few relaxed atomics.
// The message thread takes what has piled up with drain().
class ProcessTimer
{
public:
    ProcessTimer();

    // Audio thread.
    void begin() noexcept;
    void end() noexcept;
    void record (int64_t nanoseconds) noexcept;

    // Everything recorded since the last drain. count, sum and maximum are exchanged one by one, so a
    // block that lands between the three reads may have its time in this window and its count in the
    // next. Nothing is lost, and over the recent windows the totals are exact.
    struct Window
    {
        int64_t count = 0, sumNs = 0, maxNs = 0;
    };

    Window drain() noexcept;

private:
    double nsPerTick = 1.0;
    std::atomic<uint64_t> startTicks { 0 };
    std::atomic<int64_t> count { 0 }, sumNs { 0 }, maxNs { 0 };
};

// What the window shows for one slot, against the length of one buffer.
struct ProcessStats
{
    bool valid = false;             // false when no block was timed in the recent windows
    int64_t blocks = 0;
    double meanPercent = 0.0, maxPercent = 0.0;
};

// The last few drained windows of one slot. Message thread only.
class ProcessWindows
{
public:
    static constexpr size_t numWindows = 12;        // 3 s at the window's 250 ms timer

    void push (const ProcessTimer::Window&) noexcept;
    void clear() noexcept;

    // Mean over every block in the recent windows, and the largest single block, each as a percentage
    // of `periodNs` (the time one buffer lasts).
    ProcessStats stats (double periodNs) const noexcept;

private:
    std::array<ProcessTimer::Window, numWindows> ring {};
    size_t next = 0;
};

// Index of the slot with the largest worst-case block (the mean breaks a tie, then the lower index),
// or -1 when none has a reading. The worst block is what overloads the callback, so it is the one
// that stops the buffer from being lowered.
int findHeaviest (const std::vector<ProcessStats>&) noexcept;

} // namespace mfx
