#include "Engine.h"

#include <cmath>

namespace mfx
{

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
        inputHardwareRate = 0.0;
        return;
    }

    // Looking the aggregate up walks every device on the system, so it only happens after a device start.
    const auto startCount = callback.getStartCount();

    if (startCount == seenStartCount && overloads != nullptr)
        return;

    seenStartCount = startCount;
    aggregate = hal::findJuceAggregate();
    inputHardwareRate = nominalRateOf (deviceManager.getAudioDeviceSetup().inputDeviceName, true);
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
    applyExperiments();
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

    if (deviceManager.setAudioDeviceSetup (setup, false).isNotEmpty() || deviceManager.getCurrentAudioDevice() == nullptr)
        return 0;

    refreshHal();
    return deviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples();
}

double Engine::nominalRateOf (const String& deviceName, bool input)
{
    const auto id = hal::findDevice (deviceName, input, ! input);
    return id != kAudioObjectUnknown ? hal::readNumbers (id).nominalRate : 0.0;
}

ScopedSetup::ScopedSetup (Engine& e)
    : engine (e), saved (e.deviceManager.createStateXml()), experiments (e.getExperiments())
{
}

ScopedSetup::~ScopedSetup()
{
    engine.callback.setInjecting (false);
    engine.chain.setForceBypassAll (false);
    engine.setExperiments (experiments);

    if (saved != nullptr)
        engine.deviceManager.initialise (1, 2, saved.get(), false);
}

} // namespace mfx
