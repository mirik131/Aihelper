#include "gui/stt.h"
#include "gui/llm.h"
#include "whisper.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

namespace stt {
namespace {

typedef struct whisper_context* (*FnInitFile)(const char*, struct whisper_context_params);
typedef struct whisper_context_params (*FnCtxDefault)(void);
typedef struct whisper_full_params (*FnFullDefault)(enum whisper_sampling_strategy);
typedef int (*FnFull)(struct whisper_context*, struct whisper_full_params, const float*, int);
typedef int (*FnNSeg)(struct whisper_context*);
typedef const char* (*FnSegText)(struct whisper_context*, int);
typedef void (*FnFree)(struct whisper_context*);

HMODULE g_dll = nullptr;
FnInitFile f_init = nullptr;
FnCtxDefault f_ctxdef = nullptr;
FnFullDefault f_fulldef = nullptr;
FnFull f_full = nullptr;
FnNSeg f_nseg = nullptr;
FnSegText f_segtext = nullptr;
FnFree f_free = nullptr;
whisper_context* g_ctx = nullptr;

std::thread g_thr;
std::mutex g_mtx;
bool g_busy = false;
bool g_done = false;
std::string g_text;
std::string g_wav;

bool LoadDll()
{
    if (g_dll)
        return true;
    g_dll = LoadLibraryA("whisper.dll");
    if (!g_dll)
        return false;
    f_init = (FnInitFile)GetProcAddress(g_dll, "whisper_init_from_file_with_params");
    f_ctxdef = (FnCtxDefault)GetProcAddress(g_dll, "whisper_context_default_params");
    f_fulldef = (FnFullDefault)GetProcAddress(g_dll, "whisper_full_default_params");
    f_full = (FnFull)GetProcAddress(g_dll, "whisper_full");
    f_nseg = (FnNSeg)GetProcAddress(g_dll, "whisper_full_n_segments");
    f_segtext = (FnSegText)GetProcAddress(g_dll, "whisper_full_get_segment_text");
    f_free = (FnFree)GetProcAddress(g_dll, "whisper_free");
    if (!f_init || !f_ctxdef || !f_fulldef || !f_full || !f_nseg || !f_segtext || !f_free) {
        FreeLibrary(g_dll);
        g_dll = nullptr;
        return false;
    }
    return true;
}

bool EnsureModel()
{
    if (g_ctx)
        return true;
    const char* paths[] = { "models/ggml-small.bin", "../../models/ggml-small.bin",
                              "models/ggml-tiny.bin", "../../models/ggml-tiny.bin" };
    for (const char* p : paths) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") != 0 || !f)
            continue;
        fclose(f);
        g_ctx = f_init(p, f_ctxdef());
        if (g_ctx) {
            printf("stt model ok: %s\n", p);
            fflush(stdout);
            return true;
        }
    }
    printf("stt: no model\n");
    fflush(stdout);
    return false;
}

// WAV -> моно float 16кГц (микс из чего угодно: PCM16/float, любое число каналов/частот)
bool ReadWavMono16k(const char* path, std::vector<float>& out)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }
    int audioFormat = 0, channels = 0, sampleRate = 0, bits = 0;
    std::vector<uint8_t> data;
    while (true) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8)
            break;
        uint32_t size = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            std::vector<uint8_t> fmt(size);
            if (fread(fmt.data(), 1, size, f) != size)
                break;
            audioFormat = fmt[0] | (fmt[1] << 8);
            channels = fmt[2] | (fmt[3] << 8);
            sampleRate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            if (audioFormat == 0xFFFE && size >= 40) {
                // WAVEFORMATEXTENSIBLE: настоящий формат в SubFormat GUID
                uint32_t sub = fmt[24] | (fmt[25] << 8) | (fmt[26] << 16) | (fmt[27] << 24);
                audioFormat = (sub == 1) ? 1 : (sub == 3 ? 3 : 0);
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            data.resize(size);
            size_t got = fread(data.data(), 1, size, f);
            data.resize(got);
            break;
        } else {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    if (channels <= 0 || sampleRate <= 0 || data.empty())
        return false;
    int bytesPerSample = bits / 8;
    if ((audioFormat != 1 && audioFormat != 3) ||
        (bytesPerSample != 2 && bytesPerSample != 3 && bytesPerSample != 4))
        return false;
    size_t frames = data.size() / (bytesPerSample * channels);
    if (frames == 0)
        return false;
    // в моно float
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; i++) {
        double sum = 0;
        for (int c = 0; c < channels; c++) {
            const uint8_t* p = data.data() + (i * channels + c) * bytesPerSample;
            float v = 0;
            if (bytesPerSample == 2) {
                int16_t s = (int16_t)(p[0] | (p[1] << 8));
                v = s / 32768.0f;
            } else if (bytesPerSample == 3) {
                int32_t s24 = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
                if (s24 & 0x800000)
                    s24 |= (int32_t)0xFF000000;
                v = s24 / 8388608.0f;
            } else {
                memcpy(&v, p, 4);
            }
            sum += v;
        }
        mono[i] = (float)(sum / channels);
    }
    // ресемпл в 16к линейкой
    const int dstRate = 16000;
    size_t dstN = (size_t)((double)frames * dstRate / sampleRate);
    if (dstN == 0)
        return false;
    out.resize(dstN);
    for (size_t i = 0; i < dstN; i++) {
        double pos = (double)i * sampleRate / dstRate;
        size_t i0 = (size_t)pos;
        size_t i1 = i0 + 1 < frames ? i0 + 1 : i0;
        double fr = pos - i0;
        out[i] = (float)(mono[i0] * (1.0 - fr) + mono[i1] * fr);
    }
    return true;
}

std::string TrimStr(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (unsigned char)s[a] <= ' ')
        a++;
    size_t b = s.size();
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        b--;
    return s.substr(a, b - a);
}

// HTTPS-сессия одна на поток (без нового TLS-хендшейка каждый раз)
HINTERNET SttSession()
{
    thread_local HINTERNET s = nullptr;
    if (!s) {
        s = WinHttpOpen(L"helper_bot", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
        if (s)
            WinHttpSetTimeouts(s, 8000, 8000, 8000, 60000);
    }
    return s;
}

// Groq cloud STT: whisper-large-v3-turbo на их GPU. Пусто при любой беде — тогда локальный фолбэк.
std::string CloudTranscribe(const char* wavPath)
{
    std::string out;
    std::string key = llm::ApiKey();
    if (key.empty() || !wavPath || !wavPath[0])
        return out;
    FILE* f = nullptr;
    if (fopen_s(&f, wavPath, "rb") != 0 || !f)
        return out;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 44 || sz > 25 * 1024 * 1024) { // лимит Groq 25МБ
        fclose(f);
        return out;
    }
    std::vector<char> wav((size_t)sz);
    size_t got = fread(wav.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz)
        return out;

    // даунсемпл до 16кГц моно: файл в разы меньше -> быстрее аплоуд и инференс.
    // Качество то же (turbo всё равно ест 16кГц). При беде шлём как было.
    const char* upData = wav.data();
    size_t upSize = wav.size();
    std::vector<char> wav16;
    {
        std::vector<float> pcm;
        if (ReadWavMono16k(wavPath, pcm) && !pcm.empty() && pcm.size() < 50 * 1024 * 1024) {
            uint32_t dataBytes = (uint32_t)pcm.size() * 2;
            wav16.resize(44 + dataBytes);
            char* o = wav16.data();
            memcpy(o, "RIFF", 4);
            uint32_t riffSize = 36 + dataBytes;
            memcpy(o + 4, &riffSize, 4);
            memcpy(o + 8, "WAVEfmt ", 8);
            uint32_t fmtSize = 16;
            memcpy(o + 16, &fmtSize, 4);
            uint16_t fmtTag = 1, ch = 1, block = 2, bps = 16;
            uint32_t rate = 16000, byteRate = 32000;
            memcpy(o + 20, &fmtTag, 2);
            memcpy(o + 22, &ch, 2);
            memcpy(o + 24, &rate, 4);
            memcpy(o + 28, &byteRate, 4);
            memcpy(o + 32, &block, 2);
            memcpy(o + 34, &bps, 2);
            memcpy(o + 36, "data", 4);
            memcpy(o + 40, &dataBytes, 4);
            int16_t* dst = (int16_t*)(o + 44);
            for (size_t i = 0; i < pcm.size(); i++) {
                float v = pcm[i] * 32767.0f;
                if (v > 32767.0f)
                    v = 32767.0f;
                if (v < -32768.0f)
                    v = -32768.0f;
                dst[i] = (int16_t)v;
            }
            upData = wav16.data();
            upSize = wav16.size();
        }
    }

    const char* boundary = "----helperbotSTT7d4a";
    std::string body;
    body.reserve(upSize + 512);
    body += "--"; body += boundary; body += "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body.append(upData, upSize);
    body += "\r\n";
    const char* fields[][2] = { { "model", "whisper-large-v3-turbo" }, { "language", "ru" },
                                { "temperature", "0" }, { "response_format", "json" },
                                { "prompt", "Фикс открой закрой включи выключи музыку приложение телеграм блокнот ютуб пауза тише громче вкладку лайк трек" } };
    for (auto& fl : fields) {
        body += "--"; body += boundary; body += "\r\n";
        body += "Content-Disposition: form-data; name=\"";
        body += fl[0];
        body += "\"\r\n\r\n";
        body += fl[1];
        body += "\r\n";
    }
    body += "--"; body += boundary; body += "--\r\n";

    HINTERNET ses = SttSession();
    if (!ses)
        return out;
    HINTERNET con = WinHttpConnect(ses, L"api.groq.com", 443, 0);
    HINTERNET req = nullptr;
    if (con)
        req = WinHttpOpenRequest(con, L"POST", L"/openai/v1/audio/transcriptions",
                                 NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (req) {
        std::string hs = "Authorization: Bearer " + key +
                         "\r\nContent-Type: multipart/form-data; boundary=" + boundary + "\r\n";
        int wl = MultiByteToWideChar(CP_UTF8, 0, hs.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> wh(wl);
        MultiByteToWideChar(CP_UTF8, 0, hs.c_str(), -1, wh.data(), wl);
        if (WinHttpSendRequest(req, wh.data(), (DWORD)-1L,
                               (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, csz = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                NULL, &code, &csz, NULL);
            if (code == 200) {
                std::string resp;
                while (true) {
                    DWORD av = 0;
                    if (!WinHttpQueryDataAvailable(req, &av) || av == 0)
                        break;
                    size_t at = resp.size();
                    resp.resize(at + av);
                    DWORD rd = 0;
                    if (!WinHttpReadData(req, &resp[at], av, &rd)) {
                        resp.resize(at);
                        break;
                    }
                    resp.resize(at + rd);
                    if (rd == 0)
                        break;
                }
                out = TrimStr(llm::JsonField(resp, "text"));
            } else {
                printf("stt cloud http=%lu\n", (unsigned long)code);
                fflush(stdout);
            }
        }
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    return out;
}

void Worker()
{
    std::string text;
    do {
        text = CloudTranscribe(g_wav.c_str());
        if (!text.empty()) {
            printf("stt via groq cloud\n");
            fflush(stdout);
            break;
        }
        printf("stt cloud miss, local fallback\n");
        fflush(stdout);
        if (!LoadDll())
            break;
        if (!EnsureModel())
            break;
        std::vector<float> pcm;
        if (!ReadWavMono16k(g_wav.c_str(), pcm) || pcm.empty())
            break;
        auto fp = f_fulldef(WHISPER_SAMPLING_GREEDY);
        fp.language = "ru";
        fp.translate = false;
        if (f_full(g_ctx, fp, pcm.data(), (int)pcm.size()) != 0)
            break;
        int n = f_nseg(g_ctx);
        for (int i = 0; i < n; i++) {
            const char* s = f_segtext(g_ctx, i);
            if (s) {
                text += s;
                text += " ";
            }
        }
        text = TrimStr(text);
    } while (false);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_text = text;
        g_done = true;
        g_busy = false;
    }
    printf("stt done: %s\n", text.empty() ? "(empty)" : text.c_str());
    fflush(stdout);
}

} // namespace

bool TranscribeFile(const char* wavPath)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_busy)
        return false;
    g_busy = true;
    g_done = false;
    g_text.clear();
    g_wav = wavPath ? wavPath : "";
    if (g_thr.joinable())
        g_thr.join();
    g_thr = std::thread(Worker);
    return true;
}

bool Poll(std::string& outText)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_done)
        return false;
    g_done = false;
    outText = g_text;
    return true;
}

bool Busy()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_busy;
}

void Shutdown()
{
    if (g_thr.joinable())
        g_thr.join();
    if (g_ctx && f_free) {
        f_free(g_ctx);
        g_ctx = nullptr;
    }
    if (g_dll) {
        FreeLibrary(g_dll);
        g_dll = nullptr;
    }
}

} // namespace stt
