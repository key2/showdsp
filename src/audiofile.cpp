// Decode wrapper over miniaudio (implementation lives in miniaudio_impl.c,
// compiled as C to avoid C++ header interactions).
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "../third_party/miniaudio/miniaudio.h"

#include "audiofile.h"

namespace showdsp {

bool decodeFile(const std::string& path, Audio& out, std::string& err) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        err = "cannot open/decode: " + path;
        return false;
    }
    out.sr = 44100; out.channels = 2;
    out.stereo.clear(); out.mono.clear();
    std::vector<float> buf(4096 * 2);
    ma_uint64 got = 0;
    while (ma_decoder_read_pcm_frames(&dec, buf.data(), 4096, &got) == MA_SUCCESS && got > 0) {
        out.stereo.insert(out.stereo.end(), buf.begin(), buf.begin() + got * 2);
        if (got < 4096) break;
    }
    ma_decoder_uninit(&dec);
    const size_t frames = out.stereo.size() / 2;
    out.mono.resize(frames);
    for (size_t i = 0; i < frames; i++)
        out.mono[i] = 0.5f * (out.stereo[2 * i] + out.stereo[2 * i + 1]);
    if (frames == 0) { err = "decoded 0 frames: " + path; return false; }
    return true;
}

} // namespace showdsp
