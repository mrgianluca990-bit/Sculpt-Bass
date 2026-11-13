#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace sculptbass {

struct Sample {
    std::vector<float> audio;
    double sampleRate = 44100.0;
    int rootNote = 40;
    int velocityLayer = 0;
    int roundRobin = 0;
};

// A monophonic, sample-backed bass with one timbre macro. The sample bank is
// installed before playback; all rendering and MIDI methods are allocation-free.
// Morph control: 0 = soft natural, 0.25 = open natural, 0.40 = driven bass,
// 0.65 = dance synth, 1 = 808. Timbres retain their original internal positions;
// setMorph maps the control before the existing parameter smoothing.
class BassEngine {
public:
    BassEngine() { prepare(44100.0); }

    // Non-realtime operation. Do not call concurrently with MIDI/render methods.
    // Samples remain immutable after installation. No synthetic sample fallback.
    void setSamples(std::vector<Sample> samples) {
        allSoundOff();
        samples.erase(std::remove_if(samples.begin(), samples.end(), [](const Sample& s) {
            if (s.audio.size() < 2 || !std::isfinite(s.sampleRate)
                || s.sampleRate < 1000.0 || s.sampleRate > 768000.0
                || s.rootNote < 0 || s.rootNote > 127)
                return true;
            bool nonzero = false;
            for (const float x : s.audio) {
                if (!std::isfinite(x)) return true;
                nonzero = nonzero || std::abs(x) > 1.0e-8f;
            }
            return !nonzero;
        }), samples.end());
        samples_ = std::move(samples);
        // The soft recording is a matched partner of the chosen root/take, not
        // another RR event. Both playheads start together and can morph mid-note.
        softPartners_.assign(samples_.size(), -1);
        for (std::size_t i = 0; i < samples_.size(); ++i) {
            const auto& main = samples_[i];
            if (main.velocityLayer <= 0) continue;
            for (std::size_t j = 0; j < samples_.size(); ++j) {
                const auto& soft = samples_[j];
                if (soft.velocityLayer <= 0 && soft.rootNote == main.rootNote
                    && soft.roundRobin == main.roundRobin) {
                    softPartners_[i] = static_cast<int>(j);
                    break;
                }
            }
        }
        for (auto& note : choices_) for (auto& layer : note) layer.count = 0;
        for (int note = 0; note < 128; ++note) {
            for (int layer = 0; layer < 2; ++layer) buildChoices(note, layer);
        }
        calibrateLevels();
        reset();
    }

    void prepare(double sampleRate) {
        sampleRate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 768000.0
            ? sampleRate : 44100.0;
        parameterStep_ = 1.0f - static_cast<float>(std::exp(-1.0 / (0.008 * sampleRate_)));
        bendStep_ = 1.0f - static_cast<float>(std::exp(-1.0 / (0.003 * sampleRate_)));
        velocityStep_ = 1.0f - static_cast<float>(std::exp(-1.0 / (0.004 * sampleRate_)));
        synthDecay_ = static_cast<float>(std::exp(-1.0 / (0.75 * sampleRate_)));
        synthRelease_ = static_cast<float>(std::exp(-1.0 / (0.022 * sampleRate_)));
        pitchDecay_ = static_cast<float>(std::exp(-1.0 / (0.008 * sampleRate_)));
        roundedAttackStep_ = 1.0f - static_cast<float>(std::exp(-1.0 / (0.010 * sampleRate_)));
        spectralDecay_ = static_cast<float>(std::exp(-1.0 / (0.065 * sampleRate_)));
        punchDecay_ = static_cast<float>(std::exp(-1.0 / (0.012 * sampleRate_)));
        crossfadeSamples_ = std::max(1, static_cast<int>(0.006 * sampleRate_));
        releaseSamples_ = std::max(1, static_cast<int>(0.10 * sampleRate_));
        synthAttackSamples_ = std::max(1, static_cast<int>(0.00075 * sampleRate_));
        for (std::size_t i = 0; i < glideCoefficients_.size(); ++i) {
            const double amount = static_cast<double>(i)
                / static_cast<double>(glideCoefficients_.size() - 1);
            const double seconds = 0.035 * amount;
            glideCoefficients_[i] = seconds <= 0.0 ? 0.0
                : std::exp(-1.0 / (seconds * sampleRate_));
            const double cutoff = std::min(sampleRate_ * 0.40, 1800.0 * std::pow(2.0, amount));
            toneFilterSteps_[i] = 1.0f
                - static_cast<float>(std::exp(-2.0 * pi_ * cutoff / sampleRate_));
        }
        double danceReferencePower = 0.0, danceOvertonePower = 0.0;
        for (std::size_t i = 0; i < danceHarmonics_.size(); ++i) {
            const double harmonic = static_cast<double>(i + 1);
            const double base = i == 0 ? 1.0
                : 0.65 * std::pow(harmonic, -0.85)
                    + ((i % 2) == 0 ? 0.35 * std::pow(harmonic, -0.75) : 0.0);
            const double previous = base * (harmonic > 8.0 ? 1.35 : 1.0);
            const double heldColour = 1.0 - 0.45 * static_cast<double>(i)
                / static_cast<double>(danceHarmonics_.size() - 1);
            danceReferencePower += previous * previous * heldColour * heldColour;
            // Retain the useful saw/pulse identity, but make the upper spectrum
            // recede smoothly instead of keeping a boosted, nearly flat rasp.
            const double controlled = i == 0 ? 1.0 : base * 0.82
                * std::exp(-std::max(0.0, harmonic - 6.0) / 18.0);
            danceHarmonics_[i] = static_cast<float>(controlled);
            if (i != 0) danceOvertonePower += controlled * controlled * heldColour * heldColour;

            // A deterministic, low-crest harmonic packet adds a short midrange
            // strike. It shares the oscillator clock, excludes the fundamental,
            // and never resets phase or introduces random pitch/timing.
            const double packetLevel = i == 0 ? 0.0
                : 0.13 * std::min(1.0, (harmonic - 1.0) / 8.0);
            const double packetPhase = pi_ * (harmonic - 2.0) * (harmonic - 3.0)
                / static_cast<double>(danceHarmonics_.size() - 1);
            punchSine_[i] = static_cast<float>(packetLevel * std::cos(packetPhase));
            punchCosine_[i] = static_cast<float>(packetLevel * std::sin(packetPhase));
        }
        // Put the removed sustained energy into the fundamental, preserving
        // body/RMS instead of achieving a cleaner sound by turning it down.
        danceHarmonics_[0] = static_cast<float>(std::sqrt(
            std::max(1.0, danceReferencePower - danceOvertonePower)));

        // The old 808's broad odd-harmonic tail is used only as an energy
        // reference. The audible replacement contains the first few overtones.
        static constexpr std::array<double, 15> previousSub{
            1.0, 0.18, 0.48, 0.0, 0.26, 0.0, 0.16, 0.0,
            0.11, 0.0, 0.07, 0.0, 0.04, 0.0, 0.025};
        double subReferencePower = 0.0;
        for (double amplitude : previousSub) subReferencePower += amplitude * amplitude;
        for (std::size_t i = 16; i < subHarmonics_.size(); i += 2) {
            const double previous = 0.13 * std::pow(17.0 / static_cast<double>(i + 1), 0.65);
            subReferencePower += previous * previous;
        }
        subHarmonics_.fill(0.0f);
        subHarmonics_[1] = 0.12f;
        subHarmonics_[2] = 0.17f;
        subHarmonics_[4] = 0.050f;
        subHarmonics_[6] = 0.018f;
        subHarmonics_[8] = 0.006f;
        double subOvertonePower = 0.0;
        for (float amplitude : subHarmonics_) subOvertonePower += static_cast<double>(amplitude) * amplitude;
        subHarmonics_[0] = static_cast<float>(std::sqrt(
            std::max(1.0, subReferencePower - subOvertonePower)));
        reset();
    }

    void reset() {
        allSoundOff();
        for (auto& note : rrCounters_) note.fill(0);
        lastRoundRobin_ = -1;
        bendCurrent_ = bendTarget_ = 0.0f;
        morphCurrent_ = morphTarget_;
        gainCurrent_ = gainTarget_;
        orderCounter_ = 0;
    }

    void setMorph(float value) {
        if (!std::isfinite(value)) return;
        const float knob = std::clamp(value, 0.0f, 1.0f);
        // The first quarter keeps its approved natural response. The driven
        // anchor is reached earlier and hands over fully to dance by 65%.
        if (knob <= 0.25f) morphTarget_ = knob;
        else if (knob < 0.40f)
            morphTarget_ = 0.25f + (knob - 0.25f) * (0.25f / (0.40f - 0.25f));
        else if (knob < 0.65f)
            morphTarget_ = 0.50f + (knob - 0.40f) * (0.25f / (0.65f - 0.40f));
        else if (knob < 1.0f)
            morphTarget_ = 0.75f + (knob - 0.65f) * (0.25f / (1.0f - 0.65f));
        else morphTarget_ = 1.0f;
    }

    // Linear output gain, not decibels. Unity is the calibrated nominal level.
    void setOutputGain(float gain) {
        if (std::isfinite(gain)) gainTarget_ = std::clamp(gain, 0.0f, 4.0f);
    }

    void noteOn(int note, float velocity, int channel = 1) {
        if (!validAddress(note, channel) || !std::isfinite(velocity)) return;
        if (velocity <= 0.0f) { noteOff(note, channel); return; }
        velocity = std::min(velocity, 1.0f);
        const int layer = velocity * 127.0f < 80.0f ? 0 : 1;
        const auto& choice = choices_[static_cast<std::size_t>(note)][static_cast<std::size_t>(layer)];
        if (choice.count == 0) return;
        auto& counter = rrCounters_[static_cast<std::size_t>(note)][static_cast<std::size_t>(layer)];
        const int sampleIndex = choice.indices[counter % choice.count];
        ++counter;
        lastRoundRobin_ = samples_[static_cast<std::size_t>(sampleIndex)].roundRobin;
        auto& key = held_[static_cast<std::size_t>(channel - 1)][static_cast<std::size_t>(note)];
        if (key.count < std::numeric_limits<std::uint16_t>::max()) ++key.count;
        key.order = ++orderCounter_;
        key.velocity = velocity;
        key.sampleIndex = sampleIndex;
        activate(note, channel, velocity, sampleIndex);
    }

    void noteOff(int note, int channel = 1) {
        if (!validAddress(note, channel)) return;
        auto& key = held_[static_cast<std::size_t>(channel - 1)][static_cast<std::size_t>(note)];
        if (key.count == 0) return;
        --key.count;
        if (key.count != 0 || note != activeNote_ || channel != activeChannel_) return;

        int nextNote = -1, nextChannel = -1;
        std::uint64_t newest = 0;
        for (int ch = 0; ch < 16; ++ch) {
            for (int n = 0; n < 128; ++n) {
                const auto& held = held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
                if (held.count != 0 && held.order >= newest) {
                    newest = held.order;
                    nextNote = n;
                    nextChannel = ch + 1;
                }
            }
        }
        if (nextNote >= 0) {
            const auto& next = held_[static_cast<std::size_t>(nextChannel - 1)][static_cast<std::size_t>(nextNote)];
            // Returning to a held key reuses its selected take; it is not a new RR event.
            activate(nextNote, nextChannel, next.velocity, next.sampleIndex);
        } else {
            activeNote_ = activeChannel_ = -1;
            releaseVoice(current_, releaseSamples_);
            synthGate_ = false;
        }
    }

    void pitchBend(float semitones) {
        if (std::isfinite(semitones)) bendTarget_ = std::clamp(semitones, -48.0f, 48.0f);
    }

    void allNotesOff() {
        clearHeld();
        activeNote_ = activeChannel_ = -1;
        releaseVoice(current_, releaseSamples_);
        synthGate_ = false;
    }

    void allSoundOff() {
        clearHeld();
        activeNote_ = activeChannel_ = -1;
        current_ = Voice{};
        for (auto& voice : tails_) voice = Voice{};
        synthGate_ = synthActive_ = synthLegato_ = false;
        synthEnvelope_ = synthVelocity_ = synthVelocityTarget_ = pitchEnvelope_ = 0.0f;
        spectralEnvelope_ = 0.0f;
        spectralAttackRemaining_ = 0;
        punchEnvelope_ = 0.0f;
        punchAttackRemaining_ = 0;
        synthAttackRemaining_ = 0;
        synthPhase_ = 0.0;
        synthFrequency_ = targetFrequency_ = 55.0;
        previousSample_ = previousDriven_ = 0.0f;
        toneFilter1_ = 0.0f;
        blockPeak_ = 0.0f;
    }

    // Writes, rather than adds to, the caller's section. Dual-mono stereo.
    // No allocation, lock, file access, sample mutation or bank rebuild here.
    void render(float* left, float* right, int samples) {
        blockPeak_ = 0.0f;
        for (int i = 0; i < samples; ++i) {
            smoothParameter(morphCurrent_, morphTarget_, parameterStep_);
            smoothParameter(gainCurrent_, gainTarget_, parameterStep_);
            smoothParameter(bendCurrent_, bendTarget_, bendStep_);
            const double bendRatio = std::exp2(static_cast<double>(bendCurrent_) / 12.0);

            float weight = 0.0f;
            float recorded = renderVoice(current_, bendRatio, weight);
            for (auto& tail : tails_) recorded += renderVoice(tail, bendRatio, weight);
            // A dense retrigger sequence crossfades, rather than piles up, bass voices.
            if (weight > 1.0f) recorded /= weight;
            recorded *= bankInputGain_;

            // The quiet recording provides the natural softness. Only a mild,
            // partly parallel low-pass remains; it preserves the recorded attack
            // and does not rebuild a synthetic envelope around the performance.
            const float openness = smoothstep(morphCurrent_ * 4.0f);
            const float filterStep = interpolate(toneFilterSteps_, openness);
            smoothParameter(toneFilter1_, recorded, filterStep);
            if (morphCurrent_ < 0.50f) {
                const float dryTone = 0.18f + 0.20f * openness;
                const float warm = (1.0f - dryTone) * toneFilter1_ + dryTone * recorded;
                const float returnToDrive = smoothstep((morphCurrent_ - 0.25f) * 4.0f);
                recorded = (1.0f - returnToDrive) * warm + returnToDrive * recorded;
            }

            // Distortion begins only after the separate clean-aggressive stage.
            const float driveAmount = smoothstep((morphCurrent_ - 0.30f) / 0.20f);
            // Soften only the driven-bass anchor: at 0.5 the shaper input gain
            // is 9.28x rather than 24x, while the signal stays fully in that path.
            // Recover the approved saturation smoothly by internal position 0.72;
            // the sampled path has completely faded out at internal 0.75.
            float saturationAmount = driveAmount;
            float drivenFocus = 0.0f;
            if (morphCurrent_ > 0.30f && morphCurrent_ < 0.72f) {
                drivenFocus = morphCurrent_ <= 0.50f ? driveAmount
                    : 1.0f - smoothstep((morphCurrent_ - 0.50f) / 0.22f);
                saturationAmount *= 1.0f - 0.40f * drivenFocus;
            }
            const float preGain = 1.0f + 23.0f * saturationAmount * saturationAmount;
            const float driven = recorded * preGain;
            // First-order antiderivative antialiasing of x/sqrt(1+x*x).
            // Rationalising the primitive difference avoids cancellation near x[n-1].
            const float shaped = (driven + previousDriven_)
                / (std::sqrt(1.0f + driven * driven)
                   + std::sqrt(1.0f + previousDriven_ * previousDriven_));
            const float dry = 0.5f * (recorded + previousSample_);
            // Match the shaper's new drive to the existing real-bank energy
            // calibration. This preserves body/level without changing the master.
            const float compensation = interpolate(compensation_, saturationAmount);
            // A separate post-shaper balance keeps the approved distortion
            // spectrum and envelope, but seats its anchor 3 dB lower. The trim
            // fades continuously through neighbouring control positions.
            const float drivenLevel = 1.0f + (0.70794578f - 1.0f) * drivenFocus;
            const float acoustic = (1.0f - driveAmount) * dry
                + driveAmount * shaped * compensation * drivenLevel;
            previousSample_ = recorded;
            previousDriven_ = driven;

            const float electronic = renderSynth(bendRatio);
            const float synthTransition = smoothstep((morphCurrent_ - 0.50f) * 4.0f);
            const float subTransition = smoothstep((morphCurrent_ - 0.75f) * 4.0f);
            const float electronicWeight = synthTransition * (0.92f + 0.08f * subTransition);
            // The sampled/drive path now ends at the dance anchor (65% on the
            // control), rather than retaining an 8% residue until the 808 end.
            // Keep the electronic contribution at its approved calibrated gain.
            float output = ((1.0f - synthTransition) * acoustic + electronicWeight * electronic)
                * nominalHeadroom_ * gainCurrent_;
            if (!std::isfinite(output)) output = 0.0f;
            if (left != nullptr) left[i] = output;
            if (right != nullptr) right[i] = output;
            blockPeak_ = std::max(blockPeak_, std::abs(output));
        }
    }

    bool hasSamples() const { return !samples_.empty(); }
    int lastRoundRobin() const { return lastRoundRobin_; }
    float peak() const { return blockPeak_; }

private:
    static constexpr double pi_ = 3.1415926535897932384626433832795;
    static constexpr float nominalHeadroom_ = 0.65f;
    static constexpr std::size_t maxRoundRobins_ = 64;
    static constexpr std::size_t compensationSize_ = 33;

    struct Choices {
        std::array<int, maxRoundRobins_> indices{};
        std::uint32_t count = 0;
    };
    struct HeldKey {
        std::uint16_t count = 0;
        std::uint64_t order = 0;
        float velocity = 0.0f;
        int sampleIndex = -1;
    };
    struct Voice {
        int sampleIndex = -1;
        int softSampleIndex = -1;
        double position = 0.0;
        double baseIncrement = 1.0;
        double softPosition = 0.0;
        double softBaseIncrement = 1.0;
        float velocity = 0.0f;
        float envelope = 0.0f;
        float roundedAttack = 0.0f;
        float releaseStep = 0.0f;
        int attackRemaining = 0;
        int releaseRemaining = 0;
        bool releasing = false;
    };

    static bool validAddress(int note, int channel) {
        return note >= 0 && note < 128 && channel >= 1 && channel <= 16;
    }
    static float smoothstep(float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
    static float softRecordingWeight(float morph) {
        if (morph <= 0.25f) return 1.0f - 0.10f * smoothstep(morph * 4.0f);
        return 0.90f * (1.0f - smoothstep((morph - 0.25f) * 4.0f));
    }
    static void smoothParameter(float& current, float target, float step) {
        const float difference = target - current;
        // Avoid slowly decaying denormals even in a standalone non-JUCE host.
        current = std::abs(difference) < 1.0e-7f ? target : current + step * difference;
    }
    template <typename T, std::size_t N>
    static T interpolate(const std::array<T, N>& table, float position) {
        const float index = std::clamp(position, 0.0f, 1.0f) * static_cast<float>(N - 1);
        const auto low = static_cast<std::size_t>(index);
        const auto high = std::min(low + 1, N - 1);
        return table[low] + (table[high] - table[low])
            * static_cast<T>(index - static_cast<float>(low));
    }
    static double frequencyForNote(int note) {
        return 440.0 * std::exp2((static_cast<double>(note) - 69.0) / 12.0);
    }
    static float velocityGain(float velocity) {
        return std::pow(std::clamp(velocity, 0.0f, 1.0f), 0.8f);
    }

    void clearHeld() {
        for (auto& channel : held_) for (auto& key : channel) key = HeldKey{};
    }

    void buildChoices(int note, int layer) {
        int root = -1;
        int bestDistance = 10000;
        bool hasLayer = false;
        for (const auto& sample : samples_)
            hasLayer = hasLayer || (sample.velocityLayer <= 0 ? 0 : 1) == layer;
        for (const auto& sample : samples_) {
            if (hasLayer && (sample.velocityLayer <= 0 ? 0 : 1) != layer) continue;
            const int distance = std::abs(sample.rootNote - note);
            if (distance < bestDistance || (distance == bestDistance && sample.rootNote < root)) {
                bestDistance = distance;
                root = sample.rootNote;
            }
        }
        auto& choice = choices_[static_cast<std::size_t>(note)][static_cast<std::size_t>(layer)];
        for (std::size_t i = 0; i < samples_.size() && choice.count < maxRoundRobins_; ++i) {
            const auto& sample = samples_[i];
            if (sample.rootNote != root) continue;
            if (hasLayer && (sample.velocityLayer <= 0 ? 0 : 1) != layer) continue;
            choice.indices[choice.count++] = static_cast<int>(i);
        }
        std::sort(choice.indices.begin(), choice.indices.begin() + choice.count,
            [this](int a, int b) {
                const int aRR = samples_[static_cast<std::size_t>(a)].roundRobin;
                const int bRR = samples_[static_cast<std::size_t>(b)].roundRobin;
                return aRR == bRR ? a < b : aRR < bRR;
            });
    }

    void calibrateLevels() {
        compensation_.fill(1.0f);
        bankInputGain_ = 1.0f;
        synthLevel_ = 0.22f;
        float maximum = 0.0f;
        for (const auto& sample : samples_)
            for (float x : sample.audio) maximum = std::max(maximum, std::abs(x));
        if (maximum > 0.98f) bankInputGain_ = 0.98f / maximum;

        double referenceEnergy = 0.0;
        std::array<double, compensationSize_> drivenEnergy{};
        std::size_t count = 0;
        // A bounded analysis of the opening 400 ms of each real recording.
        // One bank-wide calibration preserves the recorded velocity differences.
        for (const auto& sample : samples_) {
            const std::size_t end = std::min(sample.audio.size(),
                static_cast<std::size_t>(sample.sampleRate * 0.4));
            const std::size_t step = std::max<std::size_t>(1, end / 256);
            for (std::size_t i = 0; i < end; i += step) {
                const double x = sample.audio[i] * bankInputGain_;
                referenceEnergy += x * x;
                ++count;
                for (std::size_t j = 0; j < compensationSize_; ++j) {
                    const double amount = static_cast<double>(j) / (compensationSize_ - 1);
                    const double input = x * (1.0 + 23.0 * amount * amount);
                    drivenEnergy[j] += input * input / (1.0 + input * input);
                }
            }
        }
        if (count == 0 || referenceEnergy < 1.0e-12) return;
        for (std::size_t j = 0; j < compensationSize_; ++j) {
            compensation_[j] = static_cast<float>(std::clamp(
                std::sqrt(referenceEnergy / std::max(1.0e-18, drivenEnergy[j])), 0.01, 2.0));
        }
        // Shared real-bank reference. The electronic stages apply their own
        // calibrated gain/envelope, without lifting the instrument's master gain.
        // The bundled bank is below this ceiling and keeps its previous level.
        // It reserves peak room for the new transient with unusually hot banks;
        // the sample/drive reference and compensation remain entirely unchanged.
        synthLevel_ = static_cast<float>(std::clamp(
            std::sqrt(2.0 * referenceEnergy / static_cast<double>(count)), 0.12, 0.21));
    }

    static void releaseVoice(Voice& voice, int samples) {
        if (voice.sampleIndex < 0) return;
        if (voice.releasing && voice.releaseRemaining <= samples) return;
        voice.releasing = true;
        voice.attackRemaining = 0;
        voice.releaseRemaining = std::max(1, samples);
        voice.releaseStep = voice.envelope / static_cast<float>(voice.releaseRemaining);
    }

    void retireCurrent() {
        if (current_.sampleIndex < 0 || current_.envelope <= 0.0f) return;
        auto* destination = &tails_[0];
        for (auto& tail : tails_) {
            if (tail.sampleIndex < 0) { destination = &tail; break; }
            if (tail.envelope < destination->envelope) destination = &tail;
        }
        *destination = current_;
        releaseVoice(*destination, crossfadeSamples_);
    }

    void activate(int note, int channel, float velocity, int sampleIndex) {
        const bool legato = synthGate_ && activeNote_ >= 0 && activeNote_ != note;
        retireCurrent();
        const auto& sample = samples_[static_cast<std::size_t>(sampleIndex)];
        current_ = Voice{};
        current_.sampleIndex = sampleIndex;
        current_.baseIncrement = (sample.sampleRate / sampleRate_)
            * std::exp2((static_cast<double>(note) - sample.rootNote) / 12.0);
        current_.softSampleIndex = softPartners_[static_cast<std::size_t>(sampleIndex)];
        if (current_.softSampleIndex >= 0) {
            const auto& soft = samples_[static_cast<std::size_t>(current_.softSampleIndex)];
            current_.softBaseIncrement = (soft.sampleRate / sampleRate_)
                * std::exp2((static_cast<double>(note) - soft.rootNote) / 12.0);
        }
        current_.velocity = velocityGain(velocity);
        current_.attackRemaining = crossfadeSamples_;

        targetFrequency_ = frequencyForNote(note);
        synthVelocityTarget_ = velocityGain(velocity);
        synthLegato_ = legato;
        if (!legato) {
            synthFrequency_ = targetFrequency_;
            pitchEnvelope_ = 9.0f;
            spectralAttackRemaining_ = synthAttackSamples_;
            punchAttackRemaining_ = synthAttackSamples_;
            synthAttackRemaining_ = synthAttackSamples_;
            if (!synthActive_) {
                synthPhase_ = 0.0;
                synthEnvelope_ = 0.0f;
                synthVelocity_ = synthVelocityTarget_;
            }
        }
        synthActive_ = synthGate_ = true;
        activeNote_ = note;
        activeChannel_ = channel;
    }

    static float readRecordedSample(const Sample& sample, double position,
                                    double increment, double outputRate, float& endFade) {
        if (position >= static_cast<double>(sample.audio.size() - 1)) {
            endFade = 0.0f;
            return 0.0f;
        }
        const auto index = static_cast<std::size_t>(position);
        const float fraction = static_cast<float>(position - static_cast<double>(index));
        const float a = sample.audio[index == 0 ? 0 : index - 1];
        const float b = sample.audio[index];
        const float c = sample.audio[index + 1];
        const float d = sample.audio[std::min(index + 2, sample.audio.size() - 1)];
        const float c1 = 0.5f * (c - a);
        const float c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
        const float value = ((c3 * fraction + c2) * fraction + c1) * fraction + b;
        // Fade the immutable sample's last 3 ms even if a source file ends abruptly.
        const double remaining = (static_cast<double>(sample.audio.size() - 1) - position)
            / std::max(1.0e-9, increment);
        endFade = static_cast<float>(std::min(1.0, remaining / (0.003 * outputRate)));
        return value;
    }

    float renderVoice(Voice& voice, double bendRatio, float& totalWeight) {
        if (voice.sampleIndex < 0) return 0.0f;
        const auto& sample = samples_[static_cast<std::size_t>(voice.sampleIndex)];
        const bool mainFinished = voice.position >= static_cast<double>(sample.audio.size() - 1);
        const bool softFinished = voice.softSampleIndex < 0
            || voice.softPosition >= static_cast<double>(
                samples_[static_cast<std::size_t>(voice.softSampleIndex)].audio.size() - 1);
        if (mainFinished && softFinished) { voice = Voice{}; return 0.0f; }
        if (voice.releasing) {
            voice.envelope = std::max(0.0f, voice.envelope - voice.releaseStep);
            if (--voice.releaseRemaining <= 0) { voice = Voice{}; return 0.0f; }
        } else if (voice.attackRemaining > 0) {
            voice.envelope += (1.0f - voice.envelope) / static_cast<float>(voice.attackRemaining);
            --voice.attackRemaining;
        }
        const double increment = voice.baseIncrement * bendRatio;
        float endFade = 0.0f;
        const float value = readRecordedSample(sample, voice.position, increment, sampleRate_, endFade);
        voice.position += increment;
        float softValue = 0.0f, softEndFade = 0.0f;
        if (voice.softSampleIndex >= 0) {
            const auto& soft = samples_[static_cast<std::size_t>(voice.softSampleIndex)];
            const double softIncrement = voice.softBaseIncrement * bendRatio;
            softValue = readRecordedSample(soft, voice.softPosition, softIncrement, sampleRate_, softEndFade);
            voice.softPosition += softIncrement;
        }
        smoothParameter(voice.roundedAttack, 1.0f, roundedAttackStep_);
        const float openness = smoothstep(morphCurrent_ * 4.0f);
        const float approvedDriveAttack = voice.roundedAttack
            + openness * (1.0f - voice.roundedAttack) * 1.15f;
        // The two clean anchors use the recorded attack without the old 10 ms
        // shaping or transient boost. The approved drive path is exact at 0.5.
        if (morphCurrent_ < 0.50f) {
            const float returnToDrive = smoothstep((morphCurrent_ - 0.25f) * 4.0f);
            const float attackShape = 1.0f + returnToDrive * (approvedDriveAttack - 1.0f);
            const float softWeight = softRecordingWeight(morphCurrent_);
            if (voice.softSampleIndex >= 0) {
                const float fadeWeight = (1.0f - softWeight) * endFade + softWeight * softEndFade;
                totalWeight += voice.envelope * fadeWeight;
                // One modest, common correction for the softer performance;
                // individual takes and bank-wide drive calibration are untouched.
                const float recorded = (1.0f - softWeight) * value * endFade
                    + softWeight * softValue * softEndFade * 1.22f;
                return recorded * voice.velocity * voice.envelope * attackShape;
            }
            const float weight = voice.envelope * endFade;
            totalWeight += weight;
            const float softGain = sample.velocityLayer <= 0 ? 1.0f + 0.22f * softWeight : 1.0f;
            return value * voice.velocity * weight * attackShape * softGain;
        }
        const float weight = voice.envelope * endFade;
        totalWeight += weight;
        const float attackShape = approvedDriveAttack;
        return value * voice.velocity * weight * attackShape;
    }

    float renderSynth(double bendRatio) {
        if (!synthActive_) return 0.0f;
        if (synthGate_) {
            if (synthAttackRemaining_ > 0) {
                synthEnvelope_ += (1.0f - synthEnvelope_) / static_cast<float>(synthAttackRemaining_);
                --synthAttackRemaining_;
            } else {
                synthEnvelope_ = 0.72f + (synthEnvelope_ - 0.72f) * synthDecay_;
            }
        } else {
            synthEnvelope_ *= synthRelease_;
            if (synthEnvelope_ < 1.0e-6f) {
                synthEnvelope_ = 0.0f;
                synthActive_ = false;
                return 0.0f;
            }
        }
        synthVelocity_ += velocityStep_ * (synthVelocityTarget_ - synthVelocity_);
        pitchEnvelope_ *= pitchDecay_;
        if (pitchEnvelope_ < 1.0e-6f) pitchEnvelope_ = 0.0f;
        if (spectralAttackRemaining_ > 0) {
            spectralEnvelope_ += (1.0f - spectralEnvelope_)
                / static_cast<float>(spectralAttackRemaining_);
            --spectralAttackRemaining_;
        } else {
            spectralEnvelope_ *= spectralDecay_;
        }
        if (spectralEnvelope_ < 1.0e-6f) spectralEnvelope_ = 0.0f;
        if (punchAttackRemaining_ > 0) {
            // Rise from the current level on a retrigger: never jump an active
            // waveform or its coefficients, even for repeated identical notes.
            punchEnvelope_ += (1.0f - punchEnvelope_)
                / static_cast<float>(punchAttackRemaining_);
            --punchAttackRemaining_;
        } else {
            punchEnvelope_ *= punchDecay_;
        }
        if (punchEnvelope_ < 1.0e-6f) punchEnvelope_ = 0.0f;
        const float eightOhEight = smoothstep((morphCurrent_ - 0.75f) * 4.0f);
        const double glide = synthLegato_ ? interpolate(glideCoefficients_, eightOhEight) : 0.0;
        synthFrequency_ = targetFrequency_ + glide * (synthFrequency_ - targetFrequency_);
        if (std::abs(synthFrequency_ - targetFrequency_) < 1.0e-6) synthLegato_ = false;
        const double frequency = std::min(sampleRate_ * 0.43,
            synthFrequency_ * bendRatio
                * std::exp2(static_cast<double>(pitchEnvelope_ * eightOhEight) / 12.0));

        // Additive waves deliberately contain only a bounded set of harmonics.
        // Each partial fades out before Nyquist, including during bend/glide.
        // The dance spectrum keeps a controlled saw/pulse character with a
        // progressively softer top. The 808 has a strong fundamental and a few
        // restrained overtones; the higher-frequency strike is only a short click.
        // Pitch, phase and the low-frequency punch remain continuous and tuned.
        const double twiceCosine = 2.0 * std::cos(synthPhase_);
        double previousSine = 0.0;
        double sine = std::sin(synthPhase_);
        const float fundamental = static_cast<float>(sine);
        double previousCosine = 1.0;
        double cosine = 0.5 * twiceCosine;
        float wave = 0.0f;
        float strike = 0.0f;
        for (std::size_t harmonic = 0; harmonic < danceHarmonics_.size(); ++harmonic) {
            const double harmonicFrequency = frequency * static_cast<double>(harmonic + 1);
            const float bandLimit = smoothstep(static_cast<float>(
                (sampleRate_ * 0.45 - harmonicFrequency) / (sampleRate_ * 0.04)));
            const float colour = 1.0f - 0.45f * (1.0f - spectralEnvelope_)
                * static_cast<float>(harmonic) / static_cast<float>(danceHarmonics_.size() - 1);
            const float dance = danceHarmonics_[harmonic] * colour;
            const float amount = dance + eightOhEight * (subHarmonics_[harmonic] - dance);
            wave += static_cast<float>(sine) * amount * bandLimit;
            strike += (static_cast<float>(sine) * punchSine_[harmonic]
                + static_cast<float>(cosine) * punchCosine_[harmonic]) * bandLimit;
            const double nextSine = twiceCosine * sine - previousSine;
            previousSine = sine;
            sine = nextSine;
            const double nextCosine = twiceCosine * cosine - previousCosine;
            previousCosine = cosine;
            cosine = nextCosine;
        }
        // Rebalance the dance anchor for its larger clean-synth share. The 808
        // stage level and master gain are unchanged; its energy was redistributed.
        const float stageGain = 1.38f + 0.57f * eightOhEight;
        // A short coherent low-frequency push plus a separate harmonic strike
        // preserves the attack. At the 808 end only the upper click is shorter
        // and quieter; the full low-frequency punch is not attenuated.
        const float clickEnvelope = punchEnvelope_
            * (1.0f - eightOhEight + eightOhEight * punchEnvelope_);
        const float clickGain = 0.42f - 0.24f * eightOhEight;
        const float articulated = wave
            + punchEnvelope_ * 1.80f * fundamental + clickEnvelope * clickGain * strike;
        const float output = articulated * synthEnvelope_ * synthVelocity_ * synthLevel_ * stageGain;
        synthPhase_ += 2.0 * pi_ * frequency / sampleRate_;
        if (synthPhase_ >= 2.0 * pi_) synthPhase_ -= 2.0 * pi_;
        return output;
    }

    std::vector<Sample> samples_;
    std::vector<int> softPartners_;
    std::array<std::array<Choices, 2>, 128> choices_{};
    std::array<std::array<std::uint32_t, 2>, 128> rrCounters_{};
    std::array<std::array<HeldKey, 128>, 16> held_{};
    std::array<Voice, 8> tails_{};
    Voice current_;
    std::array<float, compensationSize_> compensation_{};
    std::array<double, 129> glideCoefficients_{};
    std::array<float, 129> toneFilterSteps_{};
    std::array<float, 64> danceHarmonics_{}, subHarmonics_{};
    std::array<float, 64> punchSine_{}, punchCosine_{};
    double sampleRate_ = 44100.0;
    float morphTarget_ = 0.0f, morphCurrent_ = 0.0f;
    float gainTarget_ = 1.0f, gainCurrent_ = 1.0f;
    float bendTarget_ = 0.0f, bendCurrent_ = 0.0f;
    float parameterStep_ = 0.01f, bendStep_ = 0.01f, velocityStep_ = 0.01f;
    float synthDecay_ = 0.999f, synthRelease_ = 0.999f, pitchDecay_ = 0.999f;
    float roundedAttackStep_ = 0.01f, spectralDecay_ = 0.999f;
    float punchDecay_ = 0.999f;
    int crossfadeSamples_ = 256, releaseSamples_ = 4410, synthAttackSamples_ = 132;
    int activeNote_ = -1, activeChannel_ = -1, lastRoundRobin_ = -1;
    std::uint64_t orderCounter_ = 0;
    bool synthGate_ = false, synthActive_ = false, synthLegato_ = false;
    float synthEnvelope_ = 0.0f, synthVelocity_ = 0.0f, synthVelocityTarget_ = 0.0f;
    float pitchEnvelope_ = 0.0f;
    float spectralEnvelope_ = 0.0f;
    int spectralAttackRemaining_ = 0;
    float punchEnvelope_ = 0.0f;
    int punchAttackRemaining_ = 0;
    int synthAttackRemaining_ = 0;
    double synthPhase_ = 0.0, synthFrequency_ = 55.0, targetFrequency_ = 55.0;
    float previousSample_ = 0.0f, previousDriven_ = 0.0f;
    float toneFilter1_ = 0.0f;
    float bankInputGain_ = 1.0f, synthLevel_ = 0.22f, blockPeak_ = 0.0f;
};

} // namespace sculptbass
