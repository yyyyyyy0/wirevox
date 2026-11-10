#pragma once

namespace mfx
{

// How MIN LATENCY judges a setting. `fastest` finds the smallest buffer that is clean on a quiet machine
// and then lowers the IO cycle usage as far as it stays clean. `headroom` watches every setting under a
// synthetic background load, asks for the callback to use at most half of its buffer, and leaves the IO
// cycle usage at the device default (a lower one shrinks the processing budget), so that the result keeps
// working when a game or a plugin editor takes CPU.
enum class TuneMode { headroom, fastest };

// What the window shows: only the total and what needs attention, or every number.
enum class ViewMode { simple, detail };

// What the window remembers between runs, besides the audio and the chain.
struct Preferences
{
    TuneMode tune = TuneMode::headroom;
    ViewMode view = ViewMode::simple;
};

} // namespace mfx
