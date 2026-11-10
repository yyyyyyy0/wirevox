# WIREVOX

[日本語](README.md)

This is the English version of the main README (Japanese). The app's UI is currently Japanese only, so UI labels are given as the Japanese label followed by an English gloss in parentheses on first use.

WIREVOX is a macOS app that runs your microphone through a chain of VST3 plugins (voice changers, EQ, noise removal, and so on) and sends the result to a virtual audio device. It is for people who want effects on a call or streaming microphone but find that a normal DAW adds too much latency. In the call app, you select the virtual device as the microphone.

Microphone → VST3 plugins → virtual device (BlackHole, Loopback, etc.) → call or streaming app

## What it does

- Low latency. Capture and output are handled in a single audio callback, and a small buffer keeps latency down.
- Shows where the latency comes from. You can see which part is largest: the input device, the buffer, each plugin, the output device, or the amount the virtual device adds. It also shows the processing time of each plugin.
- MIN LATENCY tunes itself. It searches for the smallest latency that does not drop out. There are two modes: 「余裕あり」 ("with headroom", for games and streaming) and 「最速」 ("fastest", for quiet machines).
- Monitor output. You can hear the processed voice yourself on a separate device, apart from the output that goes to the other party on the call.
- Switches between a simple view and a detail view.
- Four themes.
- Settings are saved automatically and restored on the next launch.

## Requirements

- macOS (tested only on Apple Silicon Macs)
- Command Line Tools or Xcode
- CMake 3.22 or later
- A virtual audio device (BlackHole, Loopback, etc., used as the output)
- The VST3 plugins you want to use

## Build

JUCE (a C++ library for audio apps) is a git submodule (`JUCE/`, tag 9.0.5). The build does not use the network.

```sh
git clone --recursive https://github.com/yyyyyyy0/wirevox.git
cd wirevox
# If you already cloned and JUCE/ is empty, run this
git submodule update --init
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output: `build/Wirevox_artefacts/Release/WIREVOX.app`

```sh
open build/Wirevox_artefacts/Release/WIREVOX.app
```

Depending on how Command Line Tools was installed, clang may fail to find the standard C++ headers (`<algorithm>` and so on). `CMakeLists.txt` detects this and automatically switches to the libc++ headers inside the SDK.

## Usage

1. On the first launch, no device is opened. This avoids feedback from the microphone going straight to the speakers. In the left panel, choose the output (the virtual device) and the input (the microphone).
2. For the input channels, check only the channels the microphone is connected to. With one channel, the same sound is sent to both left and right. With two or more, the first goes to left (L) and the second to right (R).
3. Choose a `.vst3` from 「プラグインを追加...」 ("Add plugin...") or drop a `.vst3` onto the window. Adding scans only that file.
4. Use the 「上」 ("Up") and 「下」 ("Down") buttons to reorder, 「バイパス」 ("Bypass") to take a plugin out temporarily, and 「編集」 ("Edit") to open the plugin's own window.
5. At the lower right you see the sample rate, the buffer, the input and output device latency, the plugin latency, the total (ms), the processing load (average and maximum), the overload count (how many times processing did not finish in time), and the late callback count (how many times the audio callback was called late). To reduce latency, reduce the buffer size.
6. Press 「MIN LATENCY」 and choose 「余裕あり」 ("with headroom") or 「最速」 ("fastest") to search automatically for the smallest latency that does not drop out. If you use the app while gaming or streaming, choose 「余裕あり」.
7. To hear the processed voice on another device as well, check 「モニター出力」 ("Monitor output") at the bottom of the left panel and choose a device and a channel pair.
8. Below the chain (the list of plugins), 「LATENCY BREAKDOWN」 shows which part of the latency is largest and the processing time of each plugin. Press 「仮想デバイスを計測」 ("Measure virtual device") to also measure the latency that something like Loopback adds.

## Main features

### MIN LATENCY

This is a button in the chain panel. Pressing it opens a menu where you choose how to search. The chosen mode is remembered for next time.

- 「余裕あり（ゲーム・配信向け）」 ("With headroom (for games and streaming)"): searches while putting load on the background. The result is less likely to drop out when the CPU is busy with a game or stream. This is the default.
- 「最速（静かなとき向け）」 ("Fastest (for quiet machines)"): searches with no load. It can reach a smaller latency when nothing else is running.

While searching, the app reopens the device several times, so the sound cuts out. The whole search takes about a minute. The sample rate does not change. The result appears in the status line at the bottom. Pressing it while the game is running gives a result closer to real use.

### Latency breakdown and measurement

Below the chain on the main screen (LATENCY BREAKDOWN), the latency is always shown split by part. The largest row gets a 「ここが最大」 ("largest here") mark. Each row has a tag showing whether the value is what the device reports about itself (declared) or what was actually measured (measured).

- Pressing 「仮想デバイスを計測」 opens a confirmation dialog. For a few seconds, a test tone plays on the output device and the microphone sound stops. During a call, the other party hears the test tone. It starts only if you choose 「計測する」 ("Measure").
- The result is saved per output device and sample rate, and shown again on the next launch under the same conditions.
- Below the breakdown, the processing time of each plugin is shown. It is a percentage of one buffer's duration: the average and the maximum over the last 3 seconds. The plugin with the largest maximum gets a 「ここが重い」 ("heavy here") mark.
- While measuring, MIN LATENCY, device selection, and adding, removing, reordering and bypassing plugins are disabled.

### Monitor output

The processed voice is also played on a second device (for example, outputs 3/4 of an audio interface), separate from the output that goes to the call. What the other party hears does not change.

1. Check 「モニター出力」 at the bottom of the left panel.
2. Choose the output device in 「モニター機器」 ("Monitor device").
3. Choose a left/right pair in 「ch」. Pairs start at an odd number, such as 1/2 and 3/4.

Until you check the box, nothing is created and no device is opened. The Audio MIDI Setup settings are never touched.

### Simple view and detail view

The button to the right of the logo and to the left of the theme selector switches the whole screen (it does not affect the audio). The chosen view is restored on the next launch.

- Simple view: shows the total latency as a large number, the largest part on one line, and the status (「安定」 "stable" or 「音切れ N 回」 "N dropouts") on one line. A 「注意:」 ("Note:") line appears only when something needs attention (a sample rate mismatch, the monitor waiting or failing, or the heaviest plugin using 70 % or more of the buffer time). The latency breakdown panel is folded away, and only the 「仮想デバイスを計測」 button remains.
- Detail view: shows everything, including the breakdown and per-plugin processing time.

On the first launch with no settings file, the app starts in simple view. The button label is 「詳細を表示」 ("Show details") in simple view and 「かんたん表示」 ("Simple view") in detail view.

### Themes

The selector at the upper right changes the look (it applies immediately to the whole app). The chosen theme is restored on the next launch.

| id | Name | Look |
| --- | --- | --- |
| `rack84` | RACK '84 | Charcoal brushed metal, rack screws, round LEDs, a green LCD |
| `terminal` | TERMINAL | A green phosphor CRT, monospace, text buttons such as `[ 編集 ]`, status shown as `ON` / `BYP` / `ERR` |
| `desktop95` | DESKTOP '95 | A grey 90s desktop, navy title bars, checkboxes |
| `neon` | NEON | Synthwave, a horizon grid, glowing outlines, pill-shaped buttons |

## Troubleshooting

- The sound crackles or cuts out: raise the buffer size. You can also run 「MIN LATENCY」 with 「余裕あり」 again. Lowering the sample rate can also reduce the load.
- A sample rate warning appears: the input device or the output device is running at a different rate from the app. In that case sample rate conversion is added in between, which increases latency and load. Matching the device's rate to the app can fix it (details in the technical notes under "How sample rates are handled").
- The monitor shows 「待機中」 ("waiting"): the monitor device or the main output device is not found right now. Check the connection and the power. When the device appears, the app retries once automatically.
- The input is silent: macOS asks for microphone access on the first launch. If you did not allow it, the input stays silent. Allow the app's microphone access in System Settings.

## Technical notes

Everything below is detail about how things work and how the settings are stored. You do not need it just to use the app. Each topic opens when you click it.

<details>
<summary>How sample rates are handled</summary>

- To reduce latency, reduce the buffer size.
- When JUCE opens a device, it writes the sample rate from the settings file (`audioDeviceRate`) to the device as `kAudioDevicePropertyNominalSampleRate`. So it cannot be said that the app never changes the rate.
- A virtual device may follow the app's rate and get rewritten (with a Loopback virtual device, there was an example where it changed from 44100 to 48000).
- When the input device cannot run at the rate in the settings (for example, the input is 192 kHz and the app is 44.1 kHz), the device's rate stays as it is, and the aggregate device that JUCE creates internally (a single device made by combining several devices) does the rate conversion (drift correction).
- The app cannot stop this write. So, for both the input device and the output device, when the original rate right after opening differs from the app's running rate, the status line shows a warning (rate conversion is added, which increases latency and load).
- `--measure` and the like print the rates both before and after opening the devices, for input and output separately, and report `CHANGED` if one moved.

</details>

<details>
<summary>How latency is measured (how <code>--measure</code> works)</summary>

The app measures "from putting a test tone on the input to hearing it on the input side of the output device". No cable is needed.

```sh
APP=build/Wirevox_artefacts/Release/WIREVOX.app/Contents/MacOS/WIREVOX
"$APP" --measure --settings <settings file>                 # measure 10 times and print median, min, max, correlation quality
"$APP" --measure --settings <settings file> --with-chain    # measure through the real plugins (subtracts the declared plugin latency)
"$APP" --measure-sweep --settings <settings file>           # try every buffer and switch combination and find the fastest stable one
"$APP" --tune --settings <settings file>                    # the same search as MIN LATENCY, without a window (default: fastest)
"$APP" --tune --headroom --settings <settings file>         # search with headroom (with background load). --fastest states fastest explicitly
```

Settings file and how it is handled at run time:

- The settings file can be a copy of the real settings (`~/Library/Application Support/WIREVOX/settings.xml`).
- Without `--settings`, `--use-default-settings` is required.
- In both cases, the command refuses to run if another WIREVOX process is running (even with a copy, it reopens the same devices and would interrupt the sound of the running one). Add `--force` only when you know the running WIREVOX is not using those devices.
- The default of `--tune` is still fastest. The app's menu defaults to with-headroom, but existing scripts and the numbers in this document were produced with this search, so it was not changed.
- `--headroom` and `--fastest` cannot be given together.
- The result is also written to `measure-<date and time>.txt` in the same folder as the settings file.
- During the measurement, a short -20 dBFS test tone plays on the output device, and the input is replaced by the test tone for that time (the microphone sound does not pass through).
- The original rates of the input device and the output device are read before and after, and a change is reported.

What is measured:

- Test tone: a 50 ms linear sweep (300 Hz to 0.4 x the sample rate). Because its energy stays inside the band, the correlation peak stays narrow (about 30 microseconds) even after passing through rate conversion, and it stands 30 dB or more above the noise because of its time-bandwidth product.
- Start point: the "time the first frame of the input buffer was captured from the hardware" (the HAL's `inInputTime`; the HAL is macOS's audio driver layer). The `hostTimeNs` that JUCE passes to the callback is `inOutputTime` (the time the output is handed to the hardware) when both input and output exist, not the input time (`JUCE/modules/juce_audio_devices/native/juce_CoreAudio_mac.cpp`, lines 1609 to 1615). So a second IOProc that only records timestamps is attached to the aggregate device, and the input time is obtained by matching it with the output time of the same cycle.
- End point: the input side of the output device is recorded with a raw IOProc, and the time is the `inInputTime` of each buffer plus the position of the start of the test tone (cross-correlation with fractional-sample interpolation). The recording side's input latency and safety offset (a margin the device adds for safety) are added as well. The length of the recording buffer does not affect the result (the result is the same when the recording buffer is changed to 64, 128, 512 or 1024; try it with `--capture-buffer`).
- The three result values:
  - `latency`: from capture at the input until the signal is available at the output device.
  - `declared`: the value that the HAL times of the host's own cycle promise over the same span. It matches input + output latency as shown by JUCE.
  - `excess`: the difference. The amount the output device adds.
- Noise estimate: the time resolution is 41.7 ns (one mach tick), and the HAL makes the times from sample positions, so on a virtual device the spread is almost 0 (the measured standard deviation is 0.01 ms or less). With a real input device, jitter in the HAL times may be included. Runs with poor correlation quality (closer to 1 means the test tone arrived intact) or a poor sidelobe ratio are marked "REJECTED" and left out of the tally.
- What cannot be measured: the test tone is injected after the input side of the aggregate device, so the latency of the input device itself (for example, inside an audio interface) and the latency added internally by input-side drift correction (rate conversion) are not included. These stay as the HAL's declared values (the latency the device reports about itself).

</details>

<details>
<summary>Measurement example</summary>

This is an example with 44.1 kHz and a 64-sample buffer, from an audio interface to a Loopback virtual device. Different devices and settings give different values.

| Span | Value |
| --- | --- |
| The host itself (HAL declared; 64 + 64 + safety 5 + 4 = 137 samples) | 3.107 ms |
| Added by the virtual device | 17.415 ms (constant at 768 samples) |
| Total | 20.522 ms |

In this example, the virtual device's 768 samples did not depend on the host's buffer size or the recording side's buffer size, and the device itself declared a latency of 0. So the host side can sometimes reduce only part of the total. To find out which part is largest in your own setup, use 「仮想デバイスを計測」 or `--measure`.

The input and output device latency shown on screen includes one buffer's worth. The latency inside a virtual device such as Loopback is not included, because the device does not declare it.

</details>

<details>
<summary>MIN LATENCY search details</summary>

Pressing the button opens a small menu where you choose how to search. The search starts with the chosen mode, and that mode is the checked item in the menu from then on (the `tuneMode` attribute on the root of the settings file, `headroom` or `fastest`; when it is missing or has an unknown value, it is `headroom`).

| Mode | Suited for | Dropout criterion | IO cycle usage |
| --- | --- | --- | --- |
| With headroom (`headroom`, default) | Games, streaming, calls | Checked while loading the background; the maximum load must be 50 % of the buffer time or less | Left alone (stays at the default 1.0; if it was lowered earlier, it is put back to the default) |
| Fastest (`fastest`) | A quiet machine with nothing else running | With no load; the maximum load must be 85 % or less (exactly the conventional search described below) | Searched by lowering it to 0.75, 0.5, 0.35, 0.25 |

The smallest buffer found on a quiet machine will drop out once a game or a plugin's window starts using the CPU (192 kHz, buffer 256, cycle usage 0.75 was an example where the time allowed for the callback was only about 1 ms). The with-headroom mode exists to avoid that. Lowering the usage also shrinks the time allowed for the callback, so the with-headroom mode skips the usage steps and searches only the buffer. Pressing it while the game is running gives a result closer to real use (the result text says so too).

#### Fastest

- Starting from the current buffer size, it lowers the buffer one step at a time, watching for dropouts for 3 seconds each. If it is unstable from the start, it raises the buffer.
- A dropout is judged as an overload, a late callback, or a maximum load above 85 %. While monitoring, a buffer that the aggregate device cannot open also counts as "not stable".
- When it finds the smallest stable buffer, it checks again for 12 seconds. If a dropout appears, it raises the buffer one step and checks again (because a plugin whose load comes in bursts is missed in a few seconds).
- Next it lowers the IO cycle usage to 0.75, 0.5, 0.35, 0.25, watches in the same way, and finishes with a 12-second check (it is rejected if the maximum load exceeds 60 % of the usage).
- The whole search takes about a minute, and the device is reopened several times, so the sound cuts out. The sample rate is not changed. The result appears in the status line.

#### With headroom

The way the buffer is searched (lowering, raising, the 12-second check) is the same as fastest. There are three differences (switched by `TuneMode` in `LatencyTuner`; the fastest processing is unchanged).

1. During the 3-second and 12-second checks, a synthetic load is put on the background (`src/BackgroundLoad.*`).
   - The worker threads run at normal priority (not real-time), and their number is the physical core count minus 1 (at least 1, counted with `juce::SystemStats`).
   - Each thread keeps cycling through a buffer of a few MB, competing for CPU and memory bandwidth.
   - It is kept stopped until the device has been reopened and settled (the settle before each cycle's check), and started after that.
   - When the search ends, is cancelled, is cut short because the device changed, cannot reopen the device, or the window is closed, the load is always stopped and joined (it is also stopped in the destructor).
2. The condition for counting as stable is stricter: the maximum load must be 50 % of the buffer time or less (zero overloads and zero late callbacks is the same as before).
3. The IO cycle usage is left at the default, and the usage steps are skipped. If the usage was lowered before the start, it is put back to the default, and on cancel or failure it is restored to the original value.

The status line during the run shows 「（背景の負荷を掛けています）」 ("applying background load"). The result includes the mode name, as in 「最小遅延（余裕あり）: バッファ N / サイクル使用率は既定のまま」 ("Minimum latency (with headroom): buffer N / cycle usage left at the default"). The background load is not a substitute for a real game. As a rough guide, it only creates a situation where processes compete for spare CPU.

</details>

<details>
<summary>IO cycle usage</summary>

The IO cycle usage (`kAudioDevicePropertyIOCycleUsage`) is a value from 0 to 1 for the aggregate device that says "how much of one cycle the callback uses".

- Making it smaller makes the HAL move the output time earlier, and the declared latency becomes `buffer x (1 + usage) + safety` (at the default 1.0, `buffer x 2 + safety`).
- The time at which the callback actually starts does not change; instead, the time allowed for the callback shrinks to `usage x buffer`.
- On the Mac used for measuring, going from 1.0 to 0.25 at 64 samples lowered the latency by 1.09 ms. With a chain of heavy plugins, overloads appear.

</details>

<details>
<summary>How the latency breakdown is calculated, and how the virtual device is measured</summary>

This is the breakdown table (LATENCY BREAKDOWN). The values are updated whenever the device or the chain changes (they are re-read every 250 ms). When the window is not tall enough, the inside of the panel scrolls.

| Row | Contents | Source |
| --- | --- | --- |
| 入力機器の申告値 (Input device, declared) | Input side latency + safety + stream | Declared (HAL) |
| 入力バッファ (Input buffer) | One buffer | Declared |
| プラグイン N (Plugin N) | That slot's `getLatencySamples()`. 0 while bypassed (it is out of the signal path) | Declared |
| 出力バッファ + safety (Output buffer + safety) | Buffer x IO cycle usage + the output side's safety | Declared |
| 出力デバイスの申告値 (Output device, declared) | Output side latency + stream | Declared |
| 出力デバイスの実測超過分 (Output device, measured excess) | The amount the output device adds without declaring it (next section). Shown as 「未計測」 ("not measured") if it has not been measured | Measured |

Each row shows samples, ms and a bar. The largest row gets a 「ここが最大」 mark and a colored band. Declared rows have an outline-only tag (declared) and a filled bar; measured rows have a filled tag (measured) and a hatched bar, so they can be told apart without color. While the excess is not measured, the total says 「超過分を除く」 ("excluding the excess"). In all four themes (RACK '84 / TERMINAL / DESKTOP '95 / NEON), the panel frame comes from the theme, and the text and mark colors use the theme's palette (text color and selection color).

How it is calculated:

- The calculation lives in one place: `declareHost()` (adds up the HAL numbers) and `computeBreakdown()` (turns them into rows; a pure function that does not depend on the screen) in `src/Breakdown.cpp`. The numbers come from `Engine::getAggregateNumbers()` and `Engine::getCycleUsage()`.
- It matches the `declared` value of `--measure` (the value derived from HAL times) over the same span when the IO cycle usage is the default 1.0 (at 44.1 kHz and 64 samples, 64 + 64 + 5 + 4 = 137 samples = 3.107 ms; `--selftest-timing` checks this).
- `--measure` also prints the value from the same function on the `HAL numbers declare ...` line.
- When the usage is lowered, the output buffer is counted at usage times the buffer (see the "IO cycle usage" topic).
- The large number on the LCD and the "total" use the same result, so they match the panel's total. If the excess has been measured, both include it.

#### Measuring the virtual device

「仮想デバイスを計測」 does the same measurement as `--measure` (`Measure.cpp`) from the app screen. `measure::Session` advances the measurement step by step. As with `LatencyTuner`, a timer on the screen calls `tick()` every 250 ms, so the message thread is not blocked. Only the correlation computation on the recording runs on a separate thread. `--measure` uses the same `Session` through `runSession()`, which runs it to the end while spinning the message loop.

- Pressing it opens a confirmation dialog. For the duration of the measurement (a few seconds), a test tone plays on the output device, the microphone sound stops, and during a call the other party hears the test tone. It starts only if you choose 「計測する」.
- During measurement, MIN LATENCY, device selection, and adding, removing, reordering and bypassing plugins are disabled. While MIN LATENCY is running the measure button is disabled, and while measuring MIN LATENCY is disabled, so they never run at the same time. The button becomes 「中止」 ("Cancel").
- During the test tone, the chain is taken out of the signal path (so that plugins do not change the test tone). Whether it ends by finishing, failing, being cancelled, or the window being closed, `ScopedSetup` restores the device settings, the IO cycle usage, the full bypass of the chain, and the test tone injection. If the device changes during measurement, it is cancelled.
- The test tone is sent 6 times, and if 3 or more runs were reliable, the median of `excess` becomes the result. If there are not enough, it is not adopted and the status line says so. The reason for a failure also appears in the status line.

The result is saved on the root of the settings file, together with the output device name and sample rate.

    <LATENCY_EXCESS outputDevice="Loopback Audio" sampleRate="44100" excessMs="17.415" runs="6" measuredAt="2026-11-10T14:23:00.000+09:00"/>

- When it was measured is shown below the breakdown.
- On the next launch, it is shown if the output device and the rate are the same.
- If the device or the rate changes, it goes back to "not measured" and is also removed from the settings file (changing back does not bring the old value back).
- If the device could not be opened at startup, the saved value is kept, because the device cannot be judged.
- An old settings file without this element, and an element with a corrupt value (NaN, a negative number, 0 runs, and so on), are both read as not measured.

#### How per-plugin processing time is measured

Below the breakdown, the time each slot's `processBlock` takes is shown as a percentage of one buffer's duration (64 samples at 44.1 kHz is 1.45 ms). It is the average over all blocks in the last 3 seconds, and the longest single block (the maximum). The slot with the largest maximum gets a 「ここが重い」 mark. What prevents lowering the buffer is the single slowest block, so the slot is chosen by the maximum rather than the average, and by the larger average when the maximums are equal.

- The method: before and after each slot in the signal path, two small pass-through processors that do not touch the buffer (`TimingProbe` in `Chain.cpp`) are placed, and the time between them is read.
- On the audio thread there are no locks, allocations or logging. It only reads the clock and adds to a few atomics (`ProcessTimer`, `ProcessTime.h`). The message thread collects the values every 250 ms.
- The way `rewire()` connects things (input → slots in the path → output) does not change.
- For a plugin with no input (a generator), a plugin with no output, or when there is no input signal, the graph cannot decide the order of the small processors before and after, so they are not wrapped, and that slot shows 「--」 (not measured).
- A bypassed slot, and everything during forced bypass, has not only its processor stopped but its small processors removed, so latency 0 and CPU 0 hold as in a full bypass. The small processors add no latency.
- In `--selftest-processtime`, the cost per plugin (including the two small processors and the graph node) was about 0.005 % of a 64-sample buffer.
- When the buffer size or the rate changes, the meaning of the percentage changes, so the measurements taken until then are discarded.

</details>

<details>
<summary>How the monitor output works</summary>

These are the monitor output items and how they behave when set.

| Item | Contents |
| --- | --- |
| モニター出力 (Monitor output, checkbox) | Until it is turned on, no aggregate device is created and no device is opened (off by default) |
| モニター機器 (Monitor device) | A list of devices that have outputs. The same device as the main output can also be chosen |
| ch | A left/right pair. Choose a pair that starts at an odd number, such as 1/2, 3/4, ... (the saved value is the 0-based first channel, so 3/4 is 2) |

What the other party on the call hears (the main output) does not change. The monitor receives a copy of the signal that goes to the main output (after the plugins, or the bypassed signal while bypassing).

#### Why an aggregate device is created

JUCE's CoreAudio device opens one input and one output together as a single aggregate device, and uses the output device's clock. Only one output can be passed, so when the monitor is used, WIREVOX creates an aggregate device of "main output + monitor device" through the HAL (`AudioHardwareCreateAggregateDevice`) and hands it to JUCE as the output device (`src/Monitor.cpp`; the HAL part is `PrivateAggregate` in `src/Hal.cpp`).

- The clock is the main output (`kAudioAggregateDeviceClockDeviceKey` and the main sub-device). Drift correction (high quality) is applied to the monitor device. The HAL does all the drift handling.
- In the callback, only the main output's channels are passed to the plugin chain, and the chain's first and second channels written out (the first channel to both sides if the main output has one channel) are copied to the monitor pair (`copyToMonitor`). There are no allocations or locks. If none of the main channels is active, the monitor is silent.
- If the monitor device is the same device as the main output, no aggregate device is created. It is treated as two more channels of the same device. If that pair is included in the main output's channels, it cannot be opened (so that the channel selection JUCE saves is not narrowed silently); the monitor is turned off again and the reason is shown. In the channel list, that pair appears as 「[モニター]」 ("[monitor]").
- JUCE's `AudioDeviceManager` is never shown the aggregate device. `MonitorDeviceType` stands in front of the CoreAudio device type and, while the monitor is off, returns the CoreAudio devices unchanged. While it is on, it returns `MonitorDevice`, which presents the device name, the channel list, the active channels and the latency declaration as those of the main output.
- The device panel, the saved DEVICESETUP, `LATENCY_EXCESS` (keyed by output device name and rate), the check in MIN LATENCY that "the device has not changed", and the virtual device measurement all use the main output's name as JUCE returns it, so the aggregate device's name appears nowhere.
- JUCE does not open the aggregate device it is handed; it rearranges the sub-devices inside and builds one more aggregate device of its own (`juce_CoreAudio_mac.cpp`, lines 1192 to 1200 and 1218 to 1264, `AggregateDeviceDescription`). So WIREVOX's aggregate device is needed only while JUCE's device is alive, and is destroyed when the device is closed, when the monitor is switched off, when the main output is changed, and when the app exits (the destructor of `MonitorDevice`; the JUCE side is cleaned up first, then this is destroyed).

#### The aggregate device is private

It is created with `kAudioAggregateDeviceIsPrivateKey` set to 1 (JUCE's own aggregate device is the same, and is made public only when logging is enabled, `juce_CoreAudio_mac.cpp` line 1303). The basis for judging that JUCE's `AudioDeviceManager` can still open it by name even though it is private is as follows.

- `CoreAudioIODeviceType::scanForDevices()` (lines 2321 to 2354) reads the `kAudioHardwarePropertyDevices` list (lines 966 to 969) and excludes only names that start with `_JucePrivateAggregateDevice_`. Aggregate devices with other names are not hidden.
- A private aggregate device is visible to the process that created it. This app's `findJuceAggregate()` (`Hal.cpp`) finds JUCE's private aggregate device in the same list, and the measurement in `--measure` relies on it. The HAL header describes a private aggregate device as "only for the process that created it, and it does not persist across launches".
- If it were public, it would show up in Audio MIDI Setup and in other apps, and leftovers would remain after a crash. Deleting by name is dangerous, so it would have to be recognized by a UID prefix; with a private device the HAL removes it when the process ends, so no such cleanup is needed.

Name and UID:

- The name is `WIREVOX monitor <pid>.<sequence number>`. The HAL's list is updated slightly after destruction, so before creating the next one the app waits up to 200 ms for the previous one to disappear from the list, so that two devices with the same name are never created.
- The UID is `local.nil.wirevox.monitor.<pid>.<UUID>`.
- It is not shown in the device list (it is excluded by the name prefix).
- This aggregate device is only for the process that created it (private), and the HAL removes it when the process ends, so no code to clean up leftovers is included.
- After creating it, the app checks whether JUCE has put it in its list (whether the name can be opened). If it is opened by name when it is not in the list, JUCE creates an input-only device with no output.

#### Saving and loading

It is a child element of the root of the settings file.

    <MONITOR enabled="1" device="My Interface" firstChannel="2"/>

- It is not written if nothing was ever chosen. An old settings file without this element is read as monitor off.
- It is also off when the device name is empty, when the channel is not a string of digits only (a negative number, a decimal, empty, or more than 4 digits), or when `enabled` is not true.
- When the device is not found, the selection is kept and the monitor waits. When the channel goes beyond the device's range (including a pair that starts at the last channel), the monitor is turned off again at open time and the reason is shown in the status line.
- In both cases DEVICESETUP and the chain are not touched, and the next save keeps `enabled="0"` with the device name and channel left in place.

#### When it fails

- When the device is "just not there right now" (the main output Loopback does not exist yet at startup, the monitor device is powered off, or it was unplugged while running), the saved selection (`enabled="1"`) is not erased. The status line shows that it is waiting and the reason, the app runs on the main output only, and it retries once when the name in the device list actually changes and the device appears (it does not look twice at the same list).
- A device that is in the list but cannot be resolved in the HAL (several with the same name, aggregate devices, and so on) is not treated as waiting; the reason is shown, the monitor is turned off, and the device is left out of the candidates.
- Things that happen while MIN LATENCY or a measurement is running are picked up when it ends and by the 250 ms timer.
- The saved value becomes `enabled="0"` only when the selection itself cannot hold (the channel goes beyond the device's range, the pair overlaps the main output, the aggregate device cannot be created or opened) and when the user turned it off.
- When an error is returned after the device was opened (reopening, startup, cleanup after a measurement, JUCE's own reopening), the monitor is also turned off, the reason (with the error text) is shown once, and the device is reopened with the main output only.
- In every one of these cases (the aggregate device cannot be created, JUCE does not list it, it cannot be opened, the monitor device or channel is missing), the device is opened with the main output only, the monitor goes back to off, and the reason is shown in the status line (no aggregate device is left behind). If the monitor device is unplugged while running, the check every 250 ms finds it and reopens with the main output only in the same way.

#### Effect on the breakdown and measurement

- The breakdown rows still describe the main output's path. When the monitor device is in the aggregate device, the HAL's output-side numbers (latency, safety, stream latency) can grow because of the monitor device's values. So while the monitor is running, `Engine::getAggregateNumbers()` replaces the output side with the main output device's own values (the input side and the buffer stay as the aggregate device's).
- The monitor's path adds just one line to the LCD (「モニター経路（申告値。デバイス間のドリフト補正は含みません）」, "Monitor path (declared values; drift correction between devices is not included)"). The input side and the plugins are the same as in the breakdown, and the output side is the monitor device's own values (buffer x usage + safety + latency + stream latency, `declareMonitorPath`). The latency added by drift correction is not included, because nobody declares it.
- 「仮想デバイスを計測」 (`measure::Session`), when the monitor is on, takes the monitor off and reopens the device only for the duration of the measurement, and `ScopedSetup` puts it back afterward (even after a failure or cancel). This is because a second output in the aggregate device changes the HAL times and the test tone's path, and the main output's excess can no longer be measured. `--measure` and the like do not use the monitor (they do not read the monitor from the settings and open with it off).

#### `--probe-monitor`

On a real machine, this checks that the aggregate device can be created and used, without playing any sound.

```sh
APP=build/Wirevox_artefacts/Release/WIREVOX.app/Contents/MacOS/WIREVOX
"$APP" --probe-monitor "BlackHole 2ch" "My Interface"
```

- It creates the aggregate device with the same function the app uses.
- It prints the nominal rates of the devices involved (the main output and the monitor device) before creating, after creating, and after destroying (with `CHANGED` if one changed).
- It prints the aggregate device's clock UID, each sub-device's range of output channels, and the mapping from the aggregate device's channel numbers to the sub-devices' channels.
- It also prints whether the device appears in this process's HAL list and in JUCE's device scan, and whether it disappeared from the list after being destroyed, then destroys it.
- It does not start IO and does not write any rate (if the HAL changes one itself, `CHANGED` shows it).
- After destroying, the HAL list updates asynchronously, so it watches the list for up to 1 second for the device to disappear, and prints the OSStatus of `AudioHardwareDestroyAggregateDevice` and the rates after the wait.
- Exit codes: 0 means no problem, 2 means a device was not found, 3 means it could not be created, 4 means a rate moved, and 5 means it did not appear in the list (or remained after being destroyed).

</details>

<details>
<summary>How bypass works</summary>

A bypassed slot is taken completely out of the signal path (the graph connections are removed, and the plugin's processing is also stopped).

- Latency becomes 0 and CPU becomes 0, and this is reflected in the latency shown on screen.
- The plugin instance and its state remain. When bypass is turned off, the plugin is `reset()` before it returns to the path (so that no old sound remains).
- A plugin leaving the path is stopped after the graph has been rebuilt and has switched to the new processing order. If it were stopped first, the graph would clear the stopped node's buffer, and the sound would have gaps during the switch.
- `--selftest-chain` switches 400 times while keeping the processing thread running. With the order "stop, then rebuild", around 7000 blocks were silent; with "rebuild, then stop", it was 0 to a few blocks. The few blocks are blocks that had started running just before switching to the new processing order.
- The earlier "bypass built into the plugin" still left latency and processing in place while bypassed.

</details>

<details>
<summary>Simple view and theme implementation</summary>

Simple view:

- The view is saved in the `view` attribute on the root of the settings file (`simple` or `detail`) and restored on the next launch.
- What the simple view (`simple`) shows, in detail:
  - The total latency (a large number; the same value as the LCD, including the measured excess if there is one. While it is not measured, 「※ 超過分を除く」, "excluding the excess", is added).
  - The largest part on one line (「いちばん大きいのは: 出力デバイス（17.4 ms）」, "The largest is: output device (17.4 ms)").
  - The status on one line (「安定」 or 「音切れ N 回」, with the maximum load). Dropouts are the sum of overloads and late callbacks, counted since the last reset.
  - A 「注意:」 line only when something needs attention (a sample rate mismatch, the reason the monitor is waiting or failed, or the heaviest plugin using 70 % or more of the buffer time).
  - The latency breakdown and per-plugin processing time panels are folded away, leaving only the 「仮想デバイスを計測」 button (the frame's name is MEASURE). The chain list uses the freed height.
  - The device panel, monitor output, MIN LATENCY and adding plugins are unchanged.
- The detail view (`detail`) is the screen as it has always been.
- When the settings file has no `view` (the app has been in use since before this feature), it starts in the detail view and looks unchanged. A first launch with no settings file yet starts in the simple view. An unknown value means the detail view.
- The wording is decided in `summarise()` in `src/Breakdown.cpp` (a pure function that does not depend on the screen), and `--selftest-breakdown` checks it.

Themes:

- The chosen theme is saved in the `theme` attribute on the root of the settings file. An unknown value becomes `rack84`.
- A theme is one file implementing `Theme` in `src/Theme.h` (a LookAndFeel that also holds the palette, fonts, dimensions and drawing). You can add one by declaring its factory function in `src/Themes.h` and adding one line to the table in `src/ThemeRegistry.cpp` (and one line to the source list in `CMakeLists.txt`).

</details>

<details>
<summary>Settings file and carrying over from old names</summary>

The settings (devices, chain, each plugin's state, bypass, the virtual device measurement result, the monitor output choice, the view and the MIN LATENCY mode) are saved in `~/Library/Application Support/WIREVOX/settings.xml` and restored on the next launch.

- When the new file does not exist yet, the app looks for settings under the old names, newest first, and makes a copy of the first one it finds, once at startup, to carry them over.
- The places searched are `~/Library/Application Support/VOXRACK/settings.xml` for the old name VOXRACK, and then `~/Library/Application Support/mic-fx-host/settings.xml` for the old name mic-fx-host (the old folders are not touched).
- With `--settings <file>`, that file is used as the settings and nothing is carried over (for checking the look, or when you do not want to touch the real settings). Only in this case can a second WIREVOX be started, but even through `..` or a symbolic link, if the file it points to is the real settings, a second one cannot be started.
- A plugin that cannot be loaded stays as a row in red text, and its state is not lost.
- When the settings file is corrupt and cannot be read, a copy is left in the same folder as `settings.unreadable.xml` before it is overwritten.
- If the whole app crashed while loading a plugin, the next launch does not load the chain and leaves the original settings in `settings.crashed.xml`.

The product name is decided by one setting, `MFX_PRODUCT_NAME` in `CMakeLists.txt` (the `.app` name, the window title, the settings folder, and the bundle ID `local.nil.wirevox`). Because the bundle ID changed, when moving from the old names (VOXRACK, mic-fx-host), macOS asks again for microphone permission. macOS asks about microphone access on the first launch. If it is not allowed, the input is silent.

</details>

<details>
<summary>Command line</summary>

These are the commands that use the path to the executable (`APP`). The details are in each topic.

- `--list-devices`: prints the input and output device names and exits.
- `--measure`, `--measure-sweep`, `--tune`: see the topic on how latency is measured. They are combined with `--settings <file>`, `--use-default-settings`, `--force`, `--with-chain`, `--headroom`, `--fastest` and `--capture-buffer`.
- `--probe-monitor`: see the topic on how the monitor output works.
- `--selftest-*`: see the topic on tests below.

```sh
APP=build/Wirevox_artefacts/Release/WIREVOX.app/Contents/MacOS/WIREVOX
"$APP" --list-devices                          # print the input and output device names and exit
```

</details>

<details>
<summary>Tests (ctest and <code>--selftest-*</code>)</summary>

```sh
APP=build/Wirevox_artefacts/Release/WIREVOX.app/Contents/MacOS/WIREVOX

"$APP" --list-devices                          # print the input and output device names and exit
"$APP" --selftest "/Library/Audio/Plug-Ins/VST3/TDR Nova.vst3"
"$APP" --selftest-settings                     # check, in a temporary folder, carrying settings over from the old folders (VOXRACK, mic-fx-host)
"$APP" --selftest-correlate                     # does the latency estimate agree down to fractional samples
"$APP" --selftest-chain                         # latency, CPU and state of a bypassed slot, and no gaps in the sound while switching
"$APP" --selftest-sweep                         # picking the fastest from the exhaustive search
"$APP" --selftest-timing                        # check the latency calculation with synthetic HAL times; check the microphone does not leak during test tone injection
"$APP" --selftest-engine                        # with a fake device layer: MIN LATENCY (fastest and with headroom), saved settings, restoring settings, and being killed midway
"$APP" --selftest-breakdown                     # the breakdown calculation, the largest row, saving and restoring the measured excess (back to not measured when the device or rate changes), the simple view wording, saving the view and search mode
"$APP" --selftest-processtime                   # per-plugin processing time aggregation, the "heavy here" decision, and that the small pass-through processors do not break the path
"$APP" --selftest-monitor                       # saving and restoring the monitor output settings, channel assignment, copying to the monitor pair, and falling back to the main output only on failure
(cd build && ctest)                            # run all of the above together (the --selftest plugin cases only when the plugin is installed)
```

The plugins that ctest uses can be changed with `-DMFX_TEST_PLUGINS="path1;path2"`.

What each test covers:

- `--selftest`: uses neither a window nor an audio device. It loads the plugin through the same `Chain` and graph as the GUI and processes 3 seconds in blocks of 48 kHz and 64 samples. It checks the plugin name, the reported latency, the output peak, that the output is not silent, that a mono input reaches both left and right when bypassed, and that the chain comes back the same after being turned into XML and restored. Exit code 0 on success, non-zero on a load failure and so on.
- `--selftest-settings`: only inside a temporary folder, it checks that the old settings (VOXRACK first, then mic-fx-host) are copied to the new location, that VOXRACK wins if both exist, that the old files remain, and that an existing new file is not overwritten.
- `--selftest-engine` and `--selftest-timing`: neither opens a device.
  - The former plugs in a fake device layer (`src/FakeAudioDevice.h`) in place of CoreAudio, and checks MIN LATENCY completing, being cancelled, failing and the device switching, the settings written in the meantime, the restoring by `ScopedSetup`, and a child process being killed midway through the search.
  - The latter uses synthetic times for a 48 kHz, 64-sample host and the recording-side buffer, and checks the latency calculation (matching of cycles, the sign of the recording side's lower bound, the one-buffer offset).
- The with-headroom part of `--selftest-engine`: it uses a fake of the replaceable background load component (`BackgroundLoad`), which does not actually use the CPU. For the same apparent load (62.5 % at 64 samples), it checks the following.
  - Fastest stays at 64, and with headroom goes up to 128.
  - With headroom never sets the usage and puts the previous value back to the default.
  - The load starts after things have settled, and stops on completion, cancel, device switch, a reopen failure, a case where nothing is stable, and in the destructor.
  - Fastest does not start the load.
  - For the real load, it only checks start, stop, stopping twice, and destroying it while running.
- `--selftest-breakdown`: with a fake device layer, it checks the row sums (137 samples = 3.107 ms, total 20.522 ms), 0 while bypassed, the decision of the largest row, saving and restoring settings, that it becomes not measured when the device or rate differs, that old settings files and corrupt values can be read, and that nothing changes when a measurement cannot be started. The simple view wording (how the largest row is phrased, stable and dropouts, the three notes, the note added when the excess is unknown, no device) and reading and writing `view` and `tuneMode` are in the same test.
- `--selftest-processtime`: it checks that nothing is lost even when another thread writes while reading, the calculation of the span checked and the percentage, how the heaviest slot is chosen, measurement on a real chain and graph (bypass, move, delete, a mono plugin, a plugin with latency), and the cost of the small processors.
- The core of the measurement that needs the HAL (the test tone and the recording) can only be checked on a real machine.
- `--selftest-monitor`: with a fake device layer (`FakeMonitorBackend` in place of the HAL), it checks the following for the monitor output.
  - Saving and restoring the settings (old files without the element and corrupt values are off).
  - Channel assignment (the main output with 0, 1 and 2 channels, an aggregate device where the monitor device's channels come first, the same device, and a pair that does not fit).
  - Copying in the callback (with an empty chain, a plugin, a bypassed slot, or the forced bypass during measurement, the monitor equals the main output).
  - While off, nothing is created and JUCE's device is not wrapped.
  - While on, the device name, channel list, saved DEVICESETUP, `LATENCY_EXCESS` and MIN LATENCY stay as the main output's name, and the aggregate device's name is nowhere in the settings file.
  - No aggregate device is left behind by turning on, off, switching, and exiting.
  - On a failure (no device, a channel out of range, a one-channel device, a refused creation, not appearing in the list), it goes back to the main output only.
  - If a device is unplugged while running, and a measurement takes the monitor off and puts it back.
  - Creating the aggregate device in the real HAL, JUCE opening it, the channel mapping, the rate not changing, and the monitor sound can be checked only on a real machine (`--probe-monitor` checks the first three without sound).

</details>

<details>
<summary>Design decisions (results of measurement-based tuning, the lower bound, out of scope)</summary>

#### Results of measurement-based tuning

| Change | Result |
| --- | --- |
| Measuring overloads and load | Adopted. JUCE 9.0.5 has `getXRunCount()` but never sets `running` to true, so it always returns -1 (`juce_CoreAudio_mac.cpp`, lines 1377 and 1391). Replaced with our own HAL listener and callback time measurement |
| Lower limit of the buffer size | Not possible. The HAL also accepts values such as 15, but JUCE detects a callback size different from the declared one (line 1587), restarts the device, and returns to a setting rounded to a value in the list (`requestBufferSize`, lines 1833 to 1836; `AudioDeviceManager`, line 928). Only powers of two can be chosen |
| IO cycle usage | Adopted (the second stage of MIN LATENCY). Up to 1.1 ms lower on a real machine, only when the load is light |
| Taking a bypassed slot out of the path | Adopted. For TDR Nova, a 184-sample latency at 48 kHz becomes 0 |
| Review of the computation path | No extra buffer latency. The input has only JUCE's one copy and the graph's two copies for input and output (`juce_AudioProcessorPlayer.cpp` lines 61 to 104, `AudioInOp` at line 995 and `AudioOutOp` at line 1009 of `juce_AudioProcessorGraph.cpp`). The graph's latency compensation does not take effect in a serial chain |
| Denormal handling (FTZ) | Not adopted. No difference in either average or maximum load |
| Audio workgroups | Not possible. `AudioProcessorGraph` does not pass `audioWorkgroupContextChanged` to the child plugins, and VST3 has no such API |

#### The lower bound, and going lower

With this design (the app handles input and output at the same time in one IOProc of the aggregate device), the host-side lower bound is

    input device latency + safety + 1 buffer + output safety + latency   (IO cycle usage → 0)
    = 0 + 5 + 64 + 4 + 0 = 73 samples = 1.66 ms (at 44.1 kHz and 64 samples)

(at usage 1.0 it is 137 samples = 3.107 ms).

- When the input and output rates differ, the amount added by drift correction (high-quality rate conversion) is added on top, but it could not be measured with this method.
- Making the buffer smaller lowers the bound, but then it runs into the plugins' maximum processing time. For example, some plugins had moments that took about 2 ms against a 32-sample (0.73 ms) buffer.
- To go lower than this, the app would need to be a virtual microphone driver that keeps its own clock without going through Loopback (this requires installing into the system), to remove the aggregate device and Loopback's 768 samples. This repository does not build that.

#### Out of scope

A menu bar resident mode, bulk plugin scanning, a preset UI, notarization (Apple's review and signing of the app) and AU hosting are not included (VST3 only).

</details>

## License

WIREVOX's own source code is under the MIT license (`LICENSE`).

However, WIREVOX links against JUCE (`JUCE/`, AGPLv3 or JUCE's commercial license). If you distribute the built executable, you must either comply with AGPLv3 or hold a JUCE commercial license. This repository does not distribute binaries. For your own use of a build you made yourself, check JUCE's license.
