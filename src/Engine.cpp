#include "Engine.h"

#include "Settings.h"
#include "Text.h"

#include <cmath>

namespace mfx
{

String describeRateMismatch (double runningRate, double inputRate, double outputRate)
{
    const auto differs = [runningRate] (double rate) { return rate > 0.0 && runningRate > 0.0 && std::abs (rate - runningRate) > 0.5; };
    StringArray parts;

    if (differs (inputRate))
        parts.add (ja ("入力機器の本来のレートは ") + String (inputRate, 0) + ja (" Hz で、アプリは ") + String (runningRate, 0) + ja (" Hz で動いています"));

    if (differs (outputRate))
        parts.add (ja ("出力デバイスのレートは ") + String (outputRate, 0) + ja (" Hz で、アプリは ") + String (runningRate, 0) + ja (" Hz で動いています"));

    return parts.isEmpty() ? String() : parts.joinIntoString (" / ") + ja ("。レート変換が入り、遅延と負荷が増えます");
}

std::optional<float> validCycleUsage (double value)
{
    if (std::isnan (value) || value <= 0.0 || value > 1.0)
        return std::nullopt;

    return (float) value;
}

Engine::Engine()
{
    addDefaultFormatsToManager (formats);
}

Engine::~Engine()
{
    if (started)
    {
        deviceManager.removeAudioCallback (&callback);
        player.setProcessor (nullptr);
    }

    overloads.reset();
    deviceManager.closeAudioDevice();
}

void Engine::openDevices (const XmlElement* deviceSetup)
{
    const XmlElement noDevice ("DEVICESETUP");
    deviceManager.initialise (1, 2, deviceSetup != nullptr ? deviceSetup : &noDevice, false);
}

void Engine::openFromSettings (const XmlElement* settingsRoot)
{
    openDevices (settingsRoot != nullptr ? settingsRoot->getChildByName ("DEVICESETUP") : nullptr);

    if (settingsRoot != nullptr && settingsRoot->hasAttribute ("cycleUsage"))
        setExperiments ({ validCycleUsage (settingsRoot->getDoubleAttribute ("cycleUsage", 0.0)) });

    if (settingsRoot != nullptr)
        excess = readExcess (*settingsRoot);
}

void Engine::start()
{
    player.setProcessor (&chain.getGraph());
    deviceManager.addAudioCallback (&callback);
    chain.rewire();
    started = true;
}

void Engine::refreshHal()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        overloads.reset();
        aggregate = kAudioObjectUnknown;
        seenStartCount = 0;
        deviceRates = {};
        return;
    }

    // A measurement belongs to one output device at one rate. A device is only judged while it is open:
    // one that is missing at launch (Loopback not running yet) must not cost the saved result.
    if (excess.has_value())
        if (const auto setup = deviceManager.getAudioDeviceSetup();
            ! excessApplies (*excess, setup.outputDeviceName, device->getCurrentSampleRate()))
            excess.reset();

    // Looking the aggregate up walks every device on the system, so it only happens after a device start.
    const auto startCount = callback.getStartCount();

    if (startCount == seenStartCount && overloads != nullptr)
        return;

    seenStartCount = startCount;
    aggregate = hal::findJuceAggregate();
    const auto setup = deviceManager.getAudioDeviceSetup();
    deviceRates = { nominalRateOf (setup.inputDeviceName, true), nominalRateOf (setup.outputDeviceName, false) };
    overloads.reset();

    if (aggregate != kAudioObjectUnknown)
    {
        auto watched = hal::subDevices (aggregate);
        watched.insert (watched.begin(), aggregate);
        overloads = std::make_unique<hal::OverloadListener> (std::move (watched));
        applyExperiments();
    }
}

void Engine::setExperiments (const Experiments& next)
{
    experiments = next;

    if (experiments.ioCycleUsage.has_value())
        experiments.ioCycleUsage = validCycleUsage ((double) *experiments.ioCycleUsage);

    applyExperiments();
}

double Engine::getCycleUsage() const
{
    float usage = 0.0f;

    if (aggregate != kAudioObjectUnknown && hal::readIoCycleUsage (aggregate, usage) && validCycleUsage ((double) usage).has_value())
        return (double) usage;

    return (double) experiments.ioCycleUsage.value_or (1.0f);
}

void Engine::setExcess (const ExcessRecord& record)
{
    excess = isUsable (record) ? std::optional<ExcessRecord> (record) : std::nullopt;
}

std::optional<ExcessRecord> Engine::getExcess() const
{
    if (! excess.has_value())
        return std::nullopt;

    auto* device = deviceManager.getCurrentAudioDevice();

    if (device != nullptr && ! excessApplies (*excess, deviceManager.getAudioDeviceSetup().outputDeviceName, device->getCurrentSampleRate()))
        return std::nullopt;

    return excess;
}

void Engine::applyExperiments()
{
    if (aggregate == kAudioObjectUnknown)
        return;

    // JUCE keeps the aggregate across a buffer size change, so an unset switch has to put the device's
    // own default (1.0) back rather than inherit the previous experiment.
    float current = 0.0f;
    const auto wanted = experiments.ioCycleUsage.value_or (1.0f);

    if (hal::readIoCycleUsage (aggregate, current) && std::abs (current - wanted) > 1.0e-4f)
        hal::writeIoCycleUsage (aggregate, wanted);
}

int Engine::setBufferSize (int frames)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.bufferSize = frames;
    return openWithSetup (setup);
}

int Engine::openWithSetup (const AudioDeviceManager::AudioDeviceSetup& setup, bool recordAsChosen)
{
    const auto failed = deviceManager.setAudioDeviceSetup (setup, recordAsChosen).isNotEmpty();
    refreshHal();

    auto* device = deviceManager.getCurrentAudioDevice();
    return failed || device == nullptr ? 0 : device->getCurrentBufferSizeSamples();
}

AudioSettings Engine::getPersistedAudio() const
{
    AudioSettings result;

    if (held.has_value())
    {
        if (held->device != nullptr)
            result.device = std::make_unique<XmlElement> (*held->device);

        result.cycleUsage = held->cycleUsage;
        return result;
    }

    result.device = deviceManager.createStateXml();

    if (result.device != nullptr)
        if (auto* device = deviceManager.getCurrentAudioDevice())
            result.device->setAttribute ("audioDeviceBufferSize", device->getCurrentBufferSizeSamples());

    result.cycleUsage = experiments.ioCycleUsage;
    return result;
}

void Engine::holdPersistedAudio()
{
    if (held.has_value())
        return;

    auto now = getPersistedAudio();
    held = Held { std::move (now.device), now.cycleUsage };
}

void Engine::releasePersistedAudio()
{
    held.reset();
}

double Engine::nominalRateOf (const String& deviceName, bool input)
{
    const auto id = hal::findDevice (deviceName, input, ! input);
    return id != kAudioObjectUnknown ? hal::readNumbers (id).nominalRate : 0.0;
}

ScopedSetup::ScopedSetup (Engine& e)
    : engine (e), saved (e.deviceManager.getAudioDeviceSetup()),
      hadDevice (e.deviceManager.getCurrentAudioDevice() != nullptr), experiments (e.getExperiments())
{
}

ScopedSetup::~ScopedSetup()
{
    engine.callback.setInjecting (false);
    engine.chain.setForceBypassAll (false);
    engine.setExperiments (experiments);

    if (hadDevice)
        engine.openWithSetup (saved);
}

} // namespace mfx
