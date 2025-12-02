#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <mpg123.h>
#include <portaudio.h>
extern "C" {
#include "vosk_api.h"
}

static std::string secs_to_srt_timestamp(double t) {
    if (t < 0) t = 0;
    int hours = (int)(t / 3600);
    int minutes = (int)((t - hours*3600) / 60);
    int seconds = (int)(t) % 60;
    int millis = (int)((t - (int)t) * 1000 + 0.5);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", hours, minutes, seconds, millis);
    return std::string(buf);
}
struct WordEntry { double start; double end; std::string word; };

static std::vector<WordEntry> parse_vosk_result_words(const std::string &json) {
    std::vector<WordEntry> out;
    size_t pos = 0;
    pos = json.find("\"result\"");
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return out;
    size_t endarr = json.find(']', pos);
    if (endarr == std::string::npos) return out;
    size_t i = pos + 1;
    while (i < endarr) {
        size_t brace = json.find('{', i);
        if (brace == std::string::npos || brace > endarr) break;
        size_t close = json.find('}', brace);
        if (close == std::string::npos || close > endarr) break;
        std::string item = json.substr(brace, close-brace+1);
        // find start:
        size_t pstart = item.find("\"start\"");
        size_t pend = item.find("\"end\"");
        size_t pword = item.find("\"word\"");
        if (pstart != std::string::npos && pend != std::string::npos && pword != std::string::npos) {
            double s=0,e=0; std::string w;
            size_t col = item.find(':', pstart);
            if (col!=std::string::npos) s = atof(item.c_str()+col+1);
            col = item.find(':', pend);
            if (col!=std::string::npos) e = atof(item.c_str()+col+1);
            size_t q1 = item.find('"', pword+6);
            if (q1!=std::string::npos) {
                size_t q2 = item.find('"', q1+1);
                if (q2!=std::string::npos) w = item.substr(q1+1, q2-q1-1);
            }
            if (!w.empty()) out.push_back({s,e,w});
        }
        i = close+1;
    }
    return out;
}

static void write_srt_from_words(const std::vector<WordEntry>& words, const std::string &outpath) {
    FILE *f = fopen(outpath.c_str(), "w");
    if (!f) return;
    int idx = 1;
    const int N = 6;
    const double MAX_DUR = 4.0;
    size_t i = 0;
    while (i < words.size()) {
        size_t j = i;
        double start = words[i].start;
        double end = words[i].end;
        std::ostringstream line;
        int count = 0;
        while (j < words.size() && count < N && (words[j].end - start) <= MAX_DUR) {
            if (count) line << " ";
            line << words[j].word;
            end = words[j].end;
            ++j; ++count;
        }
        fprintf(f, "%d\n", idx++);
        std::string s1 = secs_to_srt_timestamp(start);
        std::string s2 = secs_to_srt_timestamp(end);
        fprintf(f, "%s --> %s\n", s1.c_str(), s2.c_str());
        fprintf(f, "%s\n\n", line.str().c_str());
        i = j;
    }
    fclose(f);
}

int transcribe_file(const char *model_path, const char *mp3_path, const char *out_srt) {
    if (mpg123_init() != MPG123_OK) return 1;
    mpg123_handle *mh = mpg123_new(NULL, NULL);
    if (!mh) return 1;
    if (mpg123_open(mh, mp3_path) != MPG123_OK) return 1;
    long rate = 16000;
    int channels = 1;
    int encoding = MPG123_ENC_SIGNED_16;
    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, encoding);
    Model *model = vosk_model_new(model_path);
    if (!model) {
        std::cerr << "Failed to load model\n";
        return 1;
    }
    KaldiRecognizer *rec = vosk_recognizer_new(model, (float)rate);
    vosk_recognizer_set_max_alternatives(rec, 0);
    vosk_recognizer_set_spk_model(rec, nullptr);

    const size_t buffer_size = 4096;
    std::vector<unsigned char> buffer(buffer_size);
    size_t done = 0;
    std::vector<WordEntry> allwords;

    while (true) {
        int err = 0;
        long read = mpg123_read(mh, buffer.data(), buffer_size, (size_t*)&done);
        if (done > 0) {
            if (vosk_recognizer_accept_waveform(rec, (const char*)buffer.data(), (int)done)) {
                const char *res = vosk_recognizer_result(rec);
                if (res) {
                    std::string sres(res);
                    auto words = parse_vosk_result_words(sres);
                    allwords.insert(allwords.end(), words.begin(), words.end());
                }
            } else {
            }
        }
        if (read == MPG123_ERR || done == 0) break;
    }
    const char *final_res = vosk_recognizer_final_result(rec);
    if (final_res) {
        std::string fres(final_res);
        auto words = parse_vosk_result_words(fres);
        allwords.insert(allwords.end(), words.begin(), words.end());
    }
    write_srt_from_words(allwords, out_srt);

    vosk_recognizer_free(rec);
    vosk_model_free(model);
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();
    return 0;
}

// --- Microphone transcription using PortAudio + Vosk
int transcribe_mic(const char *model_path, const char *out_srt) {
    PaError paerr = Pa_Initialize();
    if (paerr != paNoError) return 1;
    PaStream *stream;
    PaStreamParameters inputParams;
    inputParams.device = Pa_GetDefaultInputDevice();
    if (inputParams.device == paNoDevice) {
        Pa_Terminate(); return 1;
    }
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = NULL;
    double sample_rate = 16000;
    paerr = Pa_OpenStream(&stream, &inputParams, NULL, sample_rate, paFramesPerBufferUnspecified, paClipOff, NULL, NULL);
    if (paerr != paNoError) { Pa_Terminate(); return 1; }
    paerr = Pa_StartStream(stream);
    if (paerr != paNoError) { Pa_CloseStream(stream); Pa_Terminate(); return 1; }

    Model *model = vosk_model_new(model_path);
    KaldiRecognizer *rec = vosk_recognizer_new(model, (float)sample_rate);

    std::vector<WordEntry> allwords;
    const int FRAMES = 4096;
    std::vector<int16_t> buffer(FRAMES);
    bool running = true;
    std::cerr << "Listening... press Ctrl+C to stop and finalize.\n";
    while (running) {
        paerr = Pa_ReadStream(stream, buffer.data(), FRAMES);
        if (paerr != paNoError) {
            // try continue
            continue;
        }
        int bytes = FRAMES * sizeof(int16_t);
        if (vosk_recognizer_accept_waveform(rec, (const char*)buffer.data(), bytes)) {
            const char *res = vosk_recognizer_result(rec);
            if (res) {
                std::string sres(res);
                auto words = parse_vosk_result_words(sres);
                allwords.insert(allwords.end(), words.begin(), words.end());
                // also print interim text:
                // extract "text":"..." naive
                size_t p = sres.find("\"text\"");
                if (p!=std::string::npos) {
                    size_t q = sres.find(':', p);
                    size_t qq = sres.find('\"', q);
                    size_t qqq = sres.find('\"', qq+1);
                    if (qq!=std::string::npos && qqq!=std::string::npos && qqq>qq) {
                        std::string t = sres.substr(qq+1, qqq-qq-1);
                        std::cerr << "Result: " << t << "\n";
                    }
                }
            }
        } else {
            const char *pres = vosk_recognizer_partial_result(rec);
            // ignore partial for SRT; can show to user if desired
            (void)pres;
        }
        // Note: This simple loop never breaks automatically. Stop with Ctrl+C in terminal.
    }
    const char *final_res = vosk_recognizer_final_result(rec);
    if (final_res) {
        std::string fres(final_res);
        auto words = parse_vosk_result_words(fres);
        allwords.insert(allwords.end(), words.begin(), words.end());
    }
    write_srt_from_words(allwords, out_srt);

    vosk_recognizer_free(rec);
    vosk_model_free(model);
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage:\n  " << argv[0] << " MODEL_PATH file.mp3 output.srt\n"
                  << "  " << argv[0] << " MODEL_PATH --mic output.srt\n";
        return 1;
    }
    const char *model = argv[1];
    if (std::string(argv[2]) == "--mic") {
        const char *out = argv[3];
        return transcribe_mic(model, out);
    } else {
        const char *infile = argv[2];
        const char *out = argv[3];
        return transcribe_file(model, infile, out);
    }
}
