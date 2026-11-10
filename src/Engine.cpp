#include "Engine.h"

#include "Settings.h"
#include "Text.h"

#include <cmath>

namespace mfx
{

String describeRateMismatch (double runningRate, double inputRate, double outputRate, double monitorRate)
{
    const auto differs = [runningRate] (double rate) { return rate > 0.0 && runningRate > 0.0 && std::abs (rate - runningRate) > 0.5; };
    StringArray parts;

    if (differs (inputRate))
        parts.add (ja ("入力機器の本来のレートは ") + String (inputRate, 0) + ja (" Hz で、アプリは ") + String (runningRate, 0) + ja (" Hz で動いています"));

    if (differs (outputRate))
        parts.add (ja ("出力デバイスのレートは ") + String (outputRate, 0) + ja (" Hz で、アプリは ") + String (runningRate, 0) + ja (" Hz で動いています"));

    if (differs (monitorRate))
        parts.add (ja ("モニター機器のレートは ") + String (monitorRate, 0) + ja (" Hz で、アプリは ") + String (runningRate, 0) + ja (" Hz で動いています"));

    return parts.isEmpty() ? String() : parts.joinIntoString (" / ") + ja ("。レート変換が入り、遅延と負荷が増えます");
}

std::optional<float> validCycleUsage (double value)
{
    if (std::isnan (value) || value <= 0.0 || value > 1.0)
        return std::nullopt;

    return (float) value;
}

Engine::Engine (bool useCoreAudio)
{
    addDefaultFormatsToManager (formats);

    if (useCoreAudio)
        useDeviceType (std::unique_ptr<AudioIODeviceType> (AudioIODeviceType::createAudioIODeviceType_CoreAudio()), makeHalMonitorBackend());
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

void Engine::useDeviceType (std::unique_ptr<AudioIODeviceType> inner, std::unique_ptr<MonitorBackend> backend)
{
    jassert (monitorType == nullptr);

    auto type = std::make_unique<MonitorDeviceType> (std::move (inner), std::move (backend));
    monitorType = type.get();
    deviceManager.addAudioDeviceType (std::move (type));
}

void Engine::openDevices (const XmlElement* deviceSetup)
{
    const XmlElement noDevice ("DEVICESETUP");
    const auto setup = deviceSetup != nullptr ? deviceSetup : &noDevice;
    deviceManager.initialise (1, 2, setup, false);

    // A device that failed to open with the monitor is gone; the same setup is tried again without it.
    if (fallBackFromOpenFailure())
        deviceManager.initialise (1, 2, setup, false);
}

// True when the last open failed while the monitor was in use. The monitor is then turned off with the error
// as the reason, so the caller can open the main output alone.
bool Engine::fallBackFromOpenFailure()
{
    if (monitorType == nullptr)
        return false;

    const auto error = monitorType->takeOpenFailure();

    if (error.isEmpty())
        return false;

    monitorType->abandonMonitor (ja ("モニターを使えません（デバイスを開けませんでした: ") + error + ja ("）。メイン出力だけで続けます"));
    return true;
}

void Engine::openFromSettings (const XmlElement* settingsRoot)
{
    // The plan is in place before the first device is created, which is where it is acted on.
    if (settingsRoot != nullptr && monitorType != nullptr)
        monitorType->setPlan (readMonitor (*settingsRoot));

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
        mainOutput = monitorOutput = kAudioObjectUnknown;
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
    mainOutput = monitorOutput = kAudioObjectUnknown;

    if (isMonitorActive())
    {
        mainOutput = hal::findDevice (setup.outputDeviceName, false, true);
        monitorOutput = hal::findDevice (monitorType->getPlan().device, false, true);
    }

    deviceRates = { nominalRateOf (setup.inputDeviceName, true), nominalRateOf (setup.outputDeviceName, false),
                    monitorOutput != kAudioObjectUnknown ? hal::readNumbers (monitorOutput).nominalRate : 0.0 };
    overloads.reset();

    if (aggregate != kAudioObjectUnknown)
    {
        auto watched = hal::subDevices (aggregate);
        watched.insert (watched.begin(), aggregate);
        overloads = std::make_unique<hal::OverloadListener> (std::move (watched));
        applyExperiments();
    }
}

hal::Numbers Engine::getAggregateNumbers() const
{
    if (aggregate == kAudioObjectUnknown)
        return {};

    auto numbers = hal::readNumbers (aggregate);

    if (mainOutput != kAudioObjectUnknown)
    {
        const auto own = hal::readNumbers (mainOutput);
        numbers.outputLatency = own.outputLatency;
        numbers.outputSafety = own.outputSafety;
        numbers.outputStreamLatency = own.outputStreamLatency;
        numbers.outputChannels = own.outputChannels;
    }

    return numbers;
}

hal::Numbers Engine::getMonitorNumbers() const
{
    return monitorOutput != kAudioObjectUnknown ? hal::readNumbers (monitorOutput) : hal::Numbers {};
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
    // A monitor that is wanted but was dropped (a size it could not open) needs a new device object to be
    // tried again, and one that is running but no longer wanted needs one to be removed.
    if (monitorNeedsReopen())
        deviceManager.closeAudioDevice();

    auto failed = deviceManager.setAudioDeviceSetup (setup, recordAsChosen).isNotEmpty();

    if (fallBackFromOpenFailure())
        failed = deviceManager.setAudioDeviceSetup (setup, recordAsChosen).isNotEmpty();

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

//==============================================================================
MonitorPlan Engine::getMonitorPlan() const
{
    return monitorType != nullptr ? monitorType->getPlan() : MonitorPlan {};
}

bool Engine::isMonitorActive() const
{
    return monitorType != nullptr && monitorType->isActive() && deviceManager.getCurrentAudioDevice() != nullptr;
}

String Engine::takeMonitorProblem()
{
    return monitorType != nullptr ? monitorType->takeProblem() : String();
}

StringArray Engine::getMonitorCandidates() const
{
    return monitorType != nullptr ? monitorType->getUsableOutputNames() : StringArray();
}

int Engine::countMonitorChannels (const String& deviceName) const
{
    return monitorType != nullptr ? monitorType->countOutputChannels (deviceName) : 0;
}

bool Engine::monitorNeedsReopen() const
{
    if (monitorType == nullptr || deviceManager.getCurrentAudioDevice() == nullptr)
        return false;

    const auto plan = monitorType->getPlan();
    return (plan.enabled && ! monitorType->isActive() && ! monitorType->isWaiting()) || (! plan.enabled && monitorType->isActive());
}

void Engine::setMonitorPlan (const MonitorPlan& plan, bool reopen)
{
    if (monitorType == nullptr)
        return;

    const auto before = monitorType->getPlan();
    const auto wasActive = monitorType->isActive();
    monitorType->setPlan (plan);

    if (reopen && deviceManager.getCurrentAudioDevice() != nullptr && (plan.enabled != wasActive || (plan.enabled && plan != before)))
        reopenDevice();
}

bool Engine::checkMonitor()
{
    if (monitorType == nullptr || ! monitorType->isActive() || monitorType->isMonitorDevicePresent())
        return false;

    monitorType->suspendMonitor (ja ("モニター機器が見つからなくなりました。メイン出力だけで続けます。機器が戻ったら自動で試します"));
    reopenDevice();
    return true;
}

bool Engine::retryMonitor()
{
    if (monitorType == nullptr || deviceManager.getCurrentAudioDevice() == nullptr || ! monitorType->consumeRetry())
        return false;

    reopenDevice();
    return true;
}

bool Engine::serviceMonitor()
{
    const auto recovered = recoverMonitorFailure();
    const auto lost = checkMonitor();
    const auto retried = retryMonitor();
    return recovered || lost || retried;
}

bool Engine::recoverMonitorFailure()
{
    if (monitorType == nullptr || deviceManager.getCurrentAudioDevice() != nullptr || ! fallBackFromOpenFailure())
        return false;

    // JUCE forgot the device names when the open failed; its saved state still has them.
    if (const auto state = deviceManager.createStateXml())
        deviceManager.initialise (1, 2, state.get(), false);

    refreshHal();
    return true;
}

// A different device layout needs a new device object, which JUCE only makes when the device is closed first
// (the same setup on an open device is a no-op).
void Engine::reopenDevice()
{
    const auto setup = deviceManager.getAudioDeviceSetup();
    deviceManager.closeAudioDevice();
    openWithSetup (setup);
}

//==============================================================================
ScopedSetup::ScopedSetup (Engine& e)
    : engine (e), saved (e.deviceManager.getAudioDeviceSetup()),
      hadDevice (e.deviceManager.getCurrentAudioDevice() != nullptr), experiments (e.getExperiments()), monitor (e.getMonitorPlan())
{
}

ScopedSetup::~ScopedSetup()
{
    engine.callback.setInjecting (false);
    engine.chain.setForceBypassAll (false);
    engine.setExperiments (experiments);

    // One reopen for both: openWithSetup makes a new device when the restored plan needs one.
    engine.setMonitorPlan (monitor, ! hadDevice);

    if (hadDevice)
        engine.openWithSetup (saved);
}

} // namespace mfx
