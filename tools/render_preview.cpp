// Offline audition of the actual BassEngine and the included real sample bank.
// Build: c++ -std=c++17 -O2 -I Source tools/render_preview.cpp -o build/render_preview
// Run:   build/render_preview Samples Preview/SculptBass-demo.wav
#include "BassEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static std::uint32_t u32(std::istream& in) {
    unsigned char b[4]{}; in.read(reinterpret_cast<char*>(b), 4);
    return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
}
static std::uint16_t u16(std::istream& in) {
    unsigned char b[2]{}; in.read(reinterpret_cast<char*>(b), 2);
    return std::uint16_t(b[0]) | (std::uint16_t(b[1]) << 8);
}
static sculptbass::Sample readSample(const fs::path& path) {
    sculptbass::Sample sample;
    if (std::sscanf(path.filename().string().c_str(), "bass_%d_v%d_rr%d.wav",
                    &sample.rootNote, &sample.velocityLayer, &sample.roundRobin) != 3)
        throw std::runtime_error("Invalid factory filename");
    std::ifstream in(path, std::ios::binary);
    char id[4]{}; in.read(id, 4);
    if (std::string(id, 4) != "RIFF") throw std::runtime_error("Invalid RIFF");
    u32(in); in.read(id, 4);
    if (std::string(id, 4) != "WAVE") throw std::runtime_error("Invalid WAVE");
    bool valid = false;
    while (in.read(id, 4)) {
        const auto size = u32(in);
        const auto next = in.tellg() + static_cast<std::streamoff>(size + (size & 1));
        if (std::string(id, 4) == "fmt ") {
            const auto format = u16(in), channels = u16(in);
            sample.sampleRate = u32(in); u32(in); u16(in);
            const auto bits = u16(in);
            valid = format == 1 && channels == 1 && bits == 16;
        } else if (std::string(id, 4) == "data") {
            if (!valid) throw std::runtime_error("Expected mono PCM16 factory WAV");
            sample.audio.resize(size / 2);
            for (float& value : sample.audio)
                value = static_cast<std::int16_t>(u16(in)) / 32768.0f;
            return sample;
        }
        in.seekg(next);
    }
    throw std::runtime_error("WAV data missing");
}
static void le(std::ostream& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 255));
}
static void writeWav(const fs::path& path, const std::vector<float>& data, unsigned rate) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const auto bytes = static_cast<std::uint32_t>(data.size() * 2);
    out.write("RIFF", 4); le(out, 36 + bytes, 4); out.write("WAVEfmt ", 8);
    le(out, 16, 4); le(out, 1, 2); le(out, 1, 2); le(out, rate, 4);
    le(out, rate * 2, 4); le(out, 2, 2); le(out, 16, 2);
    out.write("data", 4); le(out, bytes, 4);
    for (float value : data) {
        const auto pcm = static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
        le(out, static_cast<std::uint16_t>(pcm), 2);
    }
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: render_preview Samples output.wav");
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(argv[1]))
            if (entry.path().extension() == ".wav") files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        std::vector<sculptbass::Sample> bank;
        for (const auto& file : files) bank.push_back(readSample(file));
        sculptbass::BassEngine engine;
        engine.setSamples(std::move(bank));
        constexpr unsigned rate = 44100;
        engine.prepare(rate);
        std::vector<float> output;
        std::array<float, 256> left{}, right{};
        auto render = [&](int frames, bool capture = true) {
            while (frames > 0) {
                const int n = std::min(frames, static_cast<int>(left.size()));
                engine.render(left.data(), right.data(), n);
                if (capture) output.insert(output.end(), left.begin(), left.begin() + n);
                frames -= n;
            }
        };
        const std::array<int, 16> notes {35, 35, 38, 40, 35, 42, 40, 38, 35, 35, 47, 45, 42, 40, 38, 35};
        for (const float morph : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            engine.reset(); engine.setMorph(morph); engine.setOutputGain(0.8f);
            render(static_cast<int>(rate / 10), false);
            const auto start = output.size();
            for (std::size_t i = 0; i < notes.size(); ++i) {
                engine.noteOn(notes[i], i % 4 == 0 ? 0.85f : 0.72f);
                render(static_cast<int>(rate * 0.28));
                engine.noteOff(notes[i]);
                render(static_cast<int>(rate * 0.055));
            }
            double energy = 0.0;
            float anchorPeak = 0.0f;
            for (auto i = start; i < output.size(); ++i) {
                energy += double(output[i]) * output[i];
                anchorPeak = std::max(anchorPeak, std::abs(output[i]));
            }
            std::cout << "Morph " << morph << ": RMS "
                      << 10.0 * std::log10(std::max(1.0e-20, energy / double(output.size() - start)))
                      << " dBFS, peak " << anchorPeak << '\n';
            engine.allNotesOff(); render(static_cast<int>(rate * 0.7));
        }
        engine.reset(); engine.setMorph(0.0f); engine.setOutputGain(0.8f);
        render(static_cast<int>(rate / 10), false);
        engine.noteOn(35, 0.8f);
        for (int block = 0; block < 640; ++block) {
            // Both directions fit inside the recording's natural tail.
            const float progress = block < 320 ? static_cast<float>(block) / 319.0f
                                               : static_cast<float>(639 - block) / 319.0f;
            engine.setMorph(progress);
            render(256);
        }
        engine.noteOff(35); render(rate);
        float peak = 0.0f;
        for (float value : output) {
            if (!std::isfinite(value)) throw std::runtime_error("Non-finite audio");
            peak = std::max(peak, std::abs(value));
        }
        if (peak > 1.0f) throw std::runtime_error("Preview clips; fix gain before writing");
        writeWav(argv[2], output, rate);
        std::cout << "Actual bank: " << files.size() << " files; duration " << output.size() / double(rate)
                  << " s; peak " << peak << "; no normalisation or post processing.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
