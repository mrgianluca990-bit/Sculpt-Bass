#include "../Source/BassEngine.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace {
std::atomic<bool> trackAllocations{false};
std::atomic<std::size_t> allocationCount{0};
// Keep the deliberately malloc-backed replacement allocation hooks out of
// GCC's inliner; otherwise it reports a false mismatched-new/delete warning.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void releaseTestAllocation(void* p) noexcept { std::free(p); }
}

void* operator new(std::size_t size) {
    if (trackAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { releaseTestAllocation(p); }
void operator delete[](void* p) noexcept { releaseTestAllocation(p); }
void operator delete(void* p, std::size_t) noexcept { releaseTestAllocation(p); }
void operator delete[](void* p, std::size_t) noexcept { releaseTestAllocation(p); }

namespace {
constexpr double sampleRate = 48000.0;
constexpr double pi = 3.14159265358979323846;
using sculptbass::BassEngine;
using sculptbass::Sample;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

double frequency(int note) { return 440.0 * std::exp2((note - 69.0) / 12.0); }

// Synthetic signals are isolated test fixtures, never product sound content.
Sample fixture(int root = 40, int layer = 0, int rr = 0,
               double sourceRate = sampleRate, double seconds = 3.0) {
    Sample sample;
    sample.sampleRate = sourceRate;
    sample.rootNote = root;
    sample.velocityLayer = layer;
    sample.roundRobin = rr;
    sample.audio.resize(static_cast<std::size_t>(sourceRate * seconds));
    const float level = layer == 0 ? 0.24f : 0.48f;
    for (std::size_t i = 0; i < sample.audio.size(); ++i)
        sample.audio[i] = level * static_cast<float>(std::sin(2.0 * pi * frequency(root) * i / sourceRate));
    return sample;
}

std::vector<Sample> bank() {
    std::vector<Sample> samples;
    for (int layer = 0; layer < 2; ++layer)
        for (int rr = 0; rr < 4; ++rr) samples.push_back(fixture(40, layer, rr));
    return samples;
}

std::vector<float> render(BassEngine& engine, int count) {
    std::vector<float> left(static_cast<std::size_t>(count), 9.0f);
    std::vector<float> right(static_cast<std::size_t>(count), -9.0f);
    engine.render(left.data(), right.data(), count);
    require(left == right, "Output must be dual-mono and overwrite both channels");
    for (float x : left) require(std::isfinite(x), "Output contains a non-finite sample");
    return left;
}

double rms(const std::vector<float>& signal, std::size_t start = 0) {
    double sum = 0.0;
    for (std::size_t i = start; i < signal.size(); ++i) sum += signal[i] * signal[i];
    return std::sqrt(sum / std::max<std::size_t>(1, signal.size() - start));
}

double rmsWindow(const std::vector<float>& signal, std::size_t start, std::size_t end) {
    double sum = 0.0;
    end = std::min(end, signal.size());
    for (std::size_t i = start; i < end; ++i) sum += signal[i] * signal[i];
    return std::sqrt(sum / std::max<std::size_t>(1, end - start));
}

double spectralAmplitude(const std::vector<float>& signal, double hz, std::size_t start) {
    double re = 0.0, im = 0.0, weight = 0.0;
    const std::size_t n = signal.size() - start;
    for (std::size_t i = 0; i < n; ++i) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * pi * i / std::max<std::size_t>(1, n - 1));
        const double phase = 2.0 * pi * hz * i / sampleRate;
        re += signal[start + i] * window * std::cos(phase);
        im -= signal[start + i] * window * std::sin(phase);
        weight += window;
    }
    return 2.0 * std::hypot(re, im) / weight;
}

double measuredFrequency(const std::vector<float>& signal, std::size_t start = 0) {
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (std::size_t i = std::max<std::size_t>(1, start); i < signal.size(); ++i) {
        if (signal[i - 1] <= 0.0f && signal[i] > 0.0f) {
            const double at = static_cast<double>(i - 1)
                - signal[i - 1] / static_cast<double>(signal[i] - signal[i - 1]);
            if (first < 0.0) first = at;
            last = at;
            ++crossings;
        }
    }
    return crossings > 1 ? sampleRate * (crossings - 1) / (last - first) : 0.0;
}

void testEmptyAndSelection() {
    BassEngine engine;
    engine.noteOn(40, 1.0f);
    require(!engine.hasSamples() && rms(render(engine, 512)) == 0.0,
        "An empty instrument must remain silent without a placeholder");

    std::vector<Sample> samples;
    for (int root : {40, 52}) for (int layer = 0; layer < 2; ++layer)
        for (int rr = 0; rr < 4; ++rr)
            samples.push_back(fixture(root, layer, root * 100 + layer * 10 + rr, sampleRate, 0.1));
    engine.setSamples(std::move(samples));
    engine.prepare(sampleRate);
    for (int i = 0; i < 9; ++i) {
        engine.noteOn(41, 79.0f / 127.0f);
        require(engine.lastRoundRobin() == 4000 + i % 4, "Low-velocity round robin sequence is incorrect");
        engine.allSoundOff();
    }
    engine.noteOn(41, 80.0f / 127.0f);
    require(engine.lastRoundRobin() == 4010, "Velocity 80 must select the upper layer independently");
    engine.noteOn(51, 0.5f);
    require(engine.lastRoundRobin() == 5200, "The nearest recorded root must be selected");
    engine.reset();
    engine.noteOn(41, 0.5f);
    require(engine.lastRoundRobin() == 4000, "Reset must restore deterministic round robin order");
}

void testAnchorsAndTuning() {
    std::array<std::vector<float>, 5> anchors;
    for (int anchor = 0; anchor < 5; ++anchor) {
        BassEngine engine;
        engine.setSamples(bank());
        engine.setMorph(static_cast<float>(anchor) * 0.25f);
        engine.prepare(sampleRate);
        engine.noteOn(40, 0.9f);
        anchors[static_cast<std::size_t>(anchor)] = render(engine, 24000);
        require(engine.peak() < 0.90f, "Nominal-level anchor has insufficient headroom");
        require(rms(anchors[static_cast<std::size_t>(anchor)], 4800) > 0.015, "An anchor is inaudible");
    }
    std::array<double, 5> harmonic{};
    for (int i = 0; i < 5; ++i)
        harmonic[static_cast<std::size_t>(i)] = spectralAmplitude(anchors[i], frequency(40) * 3.0, 6000)
            / spectralAmplitude(anchors[i], frequency(40), 6000);
    require(harmonic[1] < 0.015, "The aggressive-clean anchor must remain clean");
    require(harmonic[2] > 0.10 && harmonic[2] > harmonic[1] + 0.08,
        "Midpoint must produce material distortion rather than a level-only change");
    const double synthSecond = spectralAmplitude(anchors[3], frequency(40) * 2.0, 6000)
        / spectralAmplitude(anchors[3], frequency(40), 6000);
    const double synthNinth = spectralAmplitude(anchors[3], frequency(40) * 9.0, 6000)
        / spectralAmplitude(anchors[3], frequency(40), 6000);
    require(synthSecond > 0.20 && synthNinth > 0.04,
        "The dance synth needs a saturated spectrum extending beyond the former eight partials");
    require(harmonic[4] > 0.40 && harmonic[4] < 0.55,
        "The 808 needs controlled aggression while retaining a dominant fundamental");
    require(std::abs(measuredFrequency(anchors[4], 6000) - frequency(40)) < 0.10,
        "808 fundamental is not MIDI-tuned after its short attack envelope");
    require(rms(anchors[4], 6000) > rms(anchors[2], 6000),
        "The full 808 anchor must not disappear below the distorted bass");
    for (int anchor : {3, 4}) {
        const auto& signal = anchors[static_cast<std::size_t>(anchor)];
        require(rmsWindow(signal, 0, 1440) > rmsWindow(signal, 4800, 12000) * 1.30,
            "The electronic attack must have a clear contrast with the sustained body");
    }
    const double ratio = rms(anchors[2], 6000) / rms(anchors[1], 6000);
    require(ratio > 0.6 && ratio < 1.8, "Distortion compensation has a large loudness jump");

    // A high-velocity event must use its matching real soft take at the natural
    // end, then open a little at 25%, without changing the selected RR.
    Sample bright = fixture(40, 1, 0);
    Sample soft = fixture(40, 0, 0);
    for (std::size_t i = 0; i < bright.audio.size(); ++i)
        bright.audio[i] += 0.15f * static_cast<float>(std::sin(2.0 * pi * 3000.0 * i / sampleRate));
    std::array<std::vector<float>, 2> clean;
    for (int stage = 0; stage < 2; ++stage) {
        BassEngine cleanEngine;
        cleanEngine.setSamples({soft, bright});
        cleanEngine.setMorph(static_cast<float>(stage) * 0.25f);
        cleanEngine.prepare(sampleRate);
        cleanEngine.noteOn(40, 0.9f);
        clean[static_cast<std::size_t>(stage)] = render(cleanEngine, 12000);
    }
    require(spectralAmplitude(clean[0], 3000.0, 3000)
        < spectralAmplitude(clean[1], 3000.0, 3000) * 0.20,
        "The natural end must use the soft recording rather than the bright take");
    require(spectralAmplitude(clean[0], frequency(40), 3000)
        >= spectralAmplitude(clean[1], frequency(40), 3000) * 0.90,
        "Rounding must retain the bass body rather than only reducing gain");

    BassEngine engine;
    std::vector<Sample> resampled;
    resampled.push_back(fixture(40, 1, 0, 22050.0));
    engine.setSamples(std::move(resampled));
    engine.prepare(sampleRate);
    engine.noteOn(47, 0.9f);
    require(std::abs(measuredFrequency(render(engine, 18000), 2000) - frequency(47)) < 0.10,
        "Sample playback must account for both root pitch and source sample rate");
    engine.setMorph(1.0f);
    engine.pitchBend(12.0f);
    require(std::abs(measuredFrequency(render(engine, 24000), 8000) - frequency(59)) < 0.15,
        "Pitch bend must tune the 808 without a random pitch offset");
}

void testHeldStackAndPanic() {
    BassEngine engine;
    engine.setSamples(bank());
    engine.setMorph(1.0f);
    engine.prepare(sampleRate);
    engine.noteOn(40, 0.9f, 1);
    render(engine, 6000);
    engine.noteOn(47, 0.8f, 2);
    render(engine, 12000);
    engine.noteOff(47, 2);
    const auto returned = render(engine, 18000);
    require(std::abs(measuredFrequency(returned, 9000) - frequency(40)) < 0.20,
        "Last-note priority must return to a still-held key on another MIDI channel");
    engine.noteOff(40, 1);
    require(rms(render(engine, 24000), 18000) < 1.0e-7, "Releasing the final held key must stop the instrument");

    engine.noteOn(40, 1.0f);
    engine.noteOn(40, 1.0f);
    engine.noteOff(40);
    require(rms(render(engine, 8000), 4000) > 0.01, "Duplicate note-on counts must survive one note-off");
    engine.noteOff(40);
    require(rms(render(engine, 24000), 18000) < 1.0e-7, "Duplicate notes must not become stuck");

    engine.noteOn(40, 1.0f);
    render(engine, 2048);
    engine.allNotesOff();
    require(rms(render(engine, 24000), 18000) < 1.0e-7, "All Notes Off must release all held state");
    engine.noteOn(43, 1.0f, 16);
    render(engine, 2048);
    engine.allSoundOff();
    require(rms(render(engine, 256)) == 0.0, "All Sound Off must silence the very next sample");
}

void testContinuousMorphAndBlocks() {
    BassEngine engine;
    engine.setSamples(bank());
    engine.prepare(sampleRate);
    engine.noteOn(40, 0.85f);
    const int selected = engine.lastRoundRobin();
    std::array<float, 64> left{}, right{};
    float previous = 0.0f, maximumJump = 0.0f, maximumPeak = 0.0f;
    for (int block = 0; block < 1500; ++block) {
        const float progress = static_cast<float>(block) / 1499.0f;
        engine.setMorph(progress < 0.5f ? progress * 2.0f : 2.0f - progress * 2.0f);
        engine.render(left.data(), right.data(), static_cast<int>(left.size()));
        for (float x : left) {
            require(std::isfinite(x), "Morph sweep produced non-finite output");
            maximumJump = std::max(maximumJump, std::abs(x - previous));
            previous = x;
        }
        maximumPeak = std::max(maximumPeak, engine.peak());
    }
    require(engine.lastRoundRobin() == selected, "Morph must not advance the round robin");
    // Rich, bandlimited 64-partial waves legitimately have steeper edges than
    // the earlier sine/sparse synth. Compare automation against those same waves
    // held at fixed timbre, rather than treating their intended edge as a click.
    float fixedTimbreJump = 0.0f;
    for (float morph : {0.5f, 0.75f, 1.0f}) {
        BassEngine fixed;
        fixed.setSamples(bank());
        fixed.setMorph(morph);
        fixed.prepare(sampleRate);
        fixed.noteOn(40, 0.85f);
        const auto steady = render(fixed, 10000);
        for (std::size_t i = 6001; i < steady.size(); ++i)
            fixedTimbreJump = std::max(fixedTimbreJump, std::abs(steady[i] - steady[i - 1]));
    }
    require(maximumJump < fixedTimbreJump * 1.15f + 0.005f,
        "The morph introduces an edge beyond the instrument's fixed-timbre waveform");
    require(maximumPeak < 0.90f, "Morph overlap introduced a bass level spike");

    BassEngine a, b;
    a.setSamples(bank()); b.setSamples(bank());
    a.prepare(sampleRate); b.prepare(sampleRate);
    a.noteOn(43, 0.8f); b.noteOn(43, 0.8f);
    a.setMorph(0.83f); b.setMorph(0.83f);
    a.pitchBend(0.7f); b.pitchBend(0.7f);
    const auto reference = render(a, 4096);
    std::vector<float> varied(4096), rightBlock(4096);
    std::size_t offset = 0;
    for (int block : {1, 7, 64, 511, 3, 1024, 2486}) {
        b.render(varied.data() + offset, rightBlock.data() + offset, block);
        offset += static_cast<std::size_t>(block);
    }
    require(offset == reference.size(), "Test block partition is invalid");
    require(reference == varied, "Audio must be independent of the host's block partition");

    // Retrigger a sustained dance note at several phases. Its first sample must
    // stay close to the uninterrupted voice: restarting brightness must not jump
    // the harmonic coefficients while the amplitude is still nonzero.
    BassEngine held;
    held.setSamples(bank());
    held.setMorph(0.75f);
    held.prepare(sampleRate);
    held.noteOn(40, 0.85f);
    render(held, 20000);
    for (int phase = 0; phase < 6; ++phase) {
        render(held, 823);
        BassEngine retriggered = held;
        float normalLeft = 0.0f, normalRight = 0.0f;
        float newLeft = 0.0f, newRight = 0.0f;
        held.render(&normalLeft, &normalRight, 1);
        retriggered.noteOn(40, 0.85f);
        retriggered.render(&newLeft, &newRight, 1);
        require(std::abs(newLeft - normalLeft) < 0.015f,
            "A sustained synth retrigger introduced an instantaneous spectral jump");
    }
}

void testRealtimePathDoesNotAllocate() {
    BassEngine engine;
    engine.setSamples(bank());
    engine.prepare(sampleRate);
    std::array<float, 256> left{}, right{};
    allocationCount = 0;
    trackAllocations = true;
    for (int block = 0; block < 300; ++block) {
        if (block % 7 == 0) engine.noteOn(36 + block % 20, 0.75f, 1 + block % 16);
        if (block % 11 == 0) engine.noteOff(36 + (block - 4 + 20) % 20, 1 + block % 16);
        if (block % 43 == 0) engine.allNotesOff();
        if (block % 101 == 0) engine.allSoundOff();
        engine.setMorph(static_cast<float>(block % 101) / 100.0f);
        engine.setOutputGain(0.8f);
        engine.pitchBend(static_cast<float>(block % 5) - 2.0f);
        engine.render(left.data(), right.data(), static_cast<int>(left.size()));
    }
    trackAllocations = false;
    require(allocationCount == 0, "MIDI or render path allocated heap memory");
    engine.allNotesOff();
    require(rms(render(engine, 24000), 18000) < 1.0e-7, "Dense MIDI sequence left a stuck note");
}
}

int main() {
    try {
        testEmptyAndSelection();
        testAnchorsAndTuning();
        testHeldStackAndPanic();
        testContinuousMorphAndBlocks();
        testRealtimePathDoesNotAllocate();
        std::cout << "PASS: sample selection, RR, morph anchors, tuning, note lifecycle, block invariance, realtime allocation checks\n";
        return 0;
    } catch (const std::exception& error) {
        trackAllocations = false;
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
