#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BassSampleData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
const char* findBundledResource (const juce::String& fileName, int& size)
{
    // Resolve original filenames instead of guessing JUCE's C++ identifier rules.
    const auto basename = fileName.fromLastOccurrenceOf ("/", false, false);
    for (int i = 0; i < BassSamples::namedResourceListSize; ++i)
    {
        if (basename == BassSamples::originalFilenames[i])
            return BassSamples::getNamedResource (BassSamples::namedResourceList[i], size);
    }

    size = 0;
    return nullptr;
}
}

juce::AudioProcessorValueTreeState::ParameterLayout SculptBassProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout parameters;
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "morph", 1 }, "Morph",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("%")));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    return parameters;
}

SculptBassProcessor::SculptBassProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SCULPT_BASS_STATE", createLayout())
{
    morphParameter = apvts.getRawParameterValue ("morph");
    outputParameter = apvts.getRawParameterValue ("output");
    loadBundledSamples();
}

void SculptBassProcessor::loadBundledSamples()
{
    // All decoding and allocation happens before playback. The engine's bank is
    // immutable afterwards; the audio callback never loads files or takes a lock.
    int manifestSize = 0;
    const auto* manifestData = findBundledResource ("manifest.json", manifestSize);
    if (manifestData == nullptr || manifestSize <= 0)
    {
        sampleMessage = "Libreria campioni mancante";
        return;
    }

    juce::var manifest;
    const auto parsed = juce::JSON::parse (
        juce::String::fromUTF8 (manifestData, manifestSize), manifest);
    const auto sampleFiles = manifest.getProperty ("sampleFiles", {});
    const auto* entries = sampleFiles.getArray();
    if (parsed.failed() || entries == nullptr || entries->isEmpty())
    {
        sampleMessage = "Manifest della libreria non valido";
        return;
    }

    juce::WavAudioFormat wav;
    std::vector<sculptbass::Sample> samples;
    samples.reserve (static_cast<std::size_t> (entries->size()));
    int largestRoundRobin = 0;

    for (const auto& entry : *entries)
    {
        if (! entry.hasProperty ("file") || ! entry.hasProperty ("rootNote")
            || ! entry.hasProperty ("velocityLayer") || ! entry.hasProperty ("roundRobin"))
        {
            sampleMessage = "Mappatura campioni incompleta";
            return;
        }

        const auto filename = entry["file"].toString();
        int dataSize = 0;
        const auto* data = findBundledResource (filename, dataSize);
        if (data == nullptr || dataSize <= 0)
        {
            sampleMessage = "Campione mancante: " + filename;
            return;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::MemoryInputStream (
                data, static_cast<std::size_t> (dataSize), false), true));
        if (reader == nullptr || reader->lengthInSamples <= 0
            || reader->lengthInSamples > 20000000 || reader->numChannels == 0
            || ! std::isfinite (reader->sampleRate) || reader->sampleRate <= 0.0)
        {
            sampleMessage = "Campione non valido: " + filename;
            return;
        }

        const int length = static_cast<int> (reader->lengthInSamples);
        juce::AudioBuffer<float> decoded (static_cast<int> (reader->numChannels), length);
        if (! reader->read (&decoded, 0, length, 0, true, true))
        {
            sampleMessage = "Lettura campione fallita: " + filename;
            return;
        }

        sculptbass::Sample sample;
        sample.rootNote = static_cast<int> (entry["rootNote"]);
        sample.velocityLayer = static_cast<int> (entry["velocityLayer"]);
        sample.roundRobin = static_cast<int> (entry["roundRobin"]);
        sample.sampleRate = reader->sampleRate;
        if (sample.rootNote < 0 || sample.rootNote > 127
            || sample.velocityLayer < 0 || sample.velocityLayer > 127
            || sample.roundRobin < 0 || sample.roundRobin > 127)
        {
            sampleMessage = "Valori di mappatura non validi: " + filename;
            return;
        }

        sample.audio.resize (static_cast<std::size_t> (length));
        const auto* left = decoded.getReadPointer (0);
        const auto* right = decoded.getNumChannels() > 1 ? decoded.getReadPointer (1) : left;
        for (int i = 0; i < length; ++i)
            sample.audio[static_cast<std::size_t> (i)] = 0.5f * (left[i] + right[i]);

        largestRoundRobin = std::max (largestRoundRobin, sample.roundRobin + 1);
        samples.push_back (std::move (sample));
    }

    engine.setSamples (std::move (samples));
    samplesReady = engine.hasSamples();
    roundRobinCount = largestRoundRobin;
    sampleMessage = samplesReady ? juce::String() : juce::String ("Libreria campioni vuota");
}

bool SculptBassProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SculptBassProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    // Install restored targets before prepare() snaps its smoothing state.
    // The first note therefore starts at the saved timbre and output level.
    engine.setMorph (morphParameter->load (std::memory_order_relaxed) * 0.01f);
    engine.setOutputGain (juce::Decibels::decibelsToGain (
        outputParameter->load (std::memory_order_relaxed), -60.0f));
    engine.prepare (sampleRate);
    meterLevel.store (0.0f, std::memory_order_relaxed);
    roundRobinStep.store (-1, std::memory_order_relaxed);
}

void SculptBassProcessor::releaseResources()
{
    engine.reset();
    meterLevel.store (0.0f, std::memory_order_relaxed);
    roundRobinStep.store (-1, std::memory_order_relaxed);
}

void SculptBassProcessor::handleMidi (const juce::MidiMessage& message)
{
    if (message.isNoteOn())
        engine.noteOn (message.getNoteNumber(), message.getFloatVelocity(), message.getChannel());
    else if (message.isNoteOff())
        engine.noteOff (message.getNoteNumber(), message.getChannel());
    else if (message.isAllSoundOff())
        engine.allSoundOff();
    else if (message.isAllNotesOff())
        engine.allNotesOff();
    else if (message.isPitchWheel())
    {
        constexpr float rangeInSemitones = 2.0f;
        const int value = message.getPitchWheelValue() - 8192;
        engine.pitchBend (rangeInSemitones * static_cast<float> (value)
                          / (value >= 0 ? 8191.0f : 8192.0f));
    }
    else if (message.isController() && message.getControllerNumber() == 121)
        engine.pitchBend (0.0f);
}

void SculptBassProcessor::renderSegment (juce::AudioBuffer<float>& audio, int start, int length)
{
    if (length > 0)
        engine.render (audio.getWritePointer (0, start), audio.getWritePointer (1, start), length);
}

void SculptBassProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();
    if (audio.getNumChannels() < 2 || ! samplesReady)
    {
        midi.clear();
        meterLevel.store (0.0f, std::memory_order_relaxed);
        return;
    }

    engine.setMorph (morphParameter->load (std::memory_order_relaxed) * 0.01f);
    engine.setOutputGain (juce::Decibels::decibelsToGain (
        outputParameter->load (std::memory_order_relaxed), -60.0f));

    int cursor = 0;
    const int blockLength = audio.getNumSamples();
    for (const auto metadata : midi)
    {
        const int position = juce::jlimit (cursor, blockLength, metadata.samplePosition);
        renderSegment (audio, cursor, position - cursor);
        // Channel messages fit in MidiMessage's inline storage. Ignore SysEx
        // before constructing a message so it cannot allocate on the audio thread.
        if (metadata.numBytes <= 3)
            handleMidi (metadata.getMessage());
        cursor = position;
    }
    renderSegment (audio, cursor, blockLength - cursor);
    midi.clear();

    const float peak = blockLength > 0 ? audio.getMagnitude (0, blockLength) : 0.0f;
    meterLevel.store (peak, std::memory_order_relaxed);
    roundRobinStep.store (engine.lastRoundRobin(), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* SculptBassProcessor::createEditor()
{
    return new SculptBassEditor (*this);
}

void SculptBassProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    state.setProperty ("schemaVersion", 1, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void SculptBassProcessor::setStateInformation (const void* data, int size)
{
    if (const auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SculptBassProcessor();
}
