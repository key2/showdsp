// showdsp — audio decode via miniaudio (MIT-0, vendored). MIT license.
// Decodes wav/flac/mp3 to 44.1 kHz f32; exposes stereo interleaved (for
// R128) and a mono mix (for analysis).
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace showdsp {

struct Audio {
    std::vector<float> stereo;   // interleaved L R, 44100 Hz
    std::vector<float> mono;     // (L+R)/2
    int sr = 44100;
    int channels = 2;
    double durationS() const { return mono.size() / (double)sr; }
};

// implemented in audiofile.cpp (miniaudio implementation unit)
bool decodeFile(const std::string& path, Audio& out, std::string& err);

} // namespace showdsp
