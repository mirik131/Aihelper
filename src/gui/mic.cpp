#include "gui/mic.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

namespace mic {
namespace {

std::thread g_thr;
std::atomic<bool> g_run{ false };
std::atomic<bool> g_rec{ false };
std::string g_path;
double g_secs = 0.0;

// дописываем WAV-заголовок под формат микса (PCM/float/extensible)
bool WriteHeader(FILE* f, const WAVEFORMATEX* wfx, uint32_t dataBytes)
{
    uint16_t fmtSize = (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? 40 : 18;
    uint32_t riffSize = 4 + (8 + fmtSize) + (8 + dataBytes);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fs = fmtSize;
    fwrite(&fs, 4, 1, f);
    fwrite(wfx, 1, fmtSize, f);
    if (fmtSize == 18) {
        uint16_t zero = 0;
        fwrite(&zero, 2, 1, f); // cbSize для WAVEFORMATEX
    }
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    return true;
}

void Worker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* cl = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    IAudioCaptureClient* cap = nullptr;
    FILE* f = nullptr;
    bool ok = false;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (SUCCEEDED(hr))
        hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    if (SUCCEEDED(hr))
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl);
    if (SUCCEEDED(hr))
        hr = cl->GetMixFormat(&wfx);
    if (SUCCEEDED(hr))
        hr = cl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wfx, nullptr);
    if (SUCCEEDED(hr))
        hr = cl->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    printf("mic chain hr=0x%X\n", (unsigned)hr);
    fflush(stdout);
    if (SUCCEEDED(hr))
        hr = fopen_s(&f, g_path.c_str(), "wb") == 0 ? S_OK : E_FAIL;
    printf("mic fopen hr=0x%X f=%p\n", (unsigned)hr, (void*)f);
    fflush(stdout);
    if (SUCCEEDED(hr)) {
        // место под заголовок, допишем в конце
        uint16_t fmtSize = (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? 40 : 18;
        uint32_t headSize = 12 + 8 + fmtSize + (fmtSize == 18 ? 2 : 0) + 8;
        std::vector<uint8_t> zero(headSize, 0);
        fwrite(zero.data(), 1, headSize, f);
        hr = cl->Start();
    }

    auto t0 = std::chrono::steady_clock::now();
    uint32_t dataBytes = 0;
    uint32_t frameBytes = 0;
    if (SUCCEEDED(hr)) {
        frameBytes = wfx->nBlockAlign;
        ok = true;
    }
    while (ok && g_run.load()) {
        // не пишем дольше минуты чтобы файл не рос бесконечно
        double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed > 60.0)
            break;
        UINT32 packets = 0;
        if (FAILED(cap->GetNextPacketSize(&packets))) {
            Sleep(10);
            continue;
        }
        if (packets == 0) {
            Sleep(10);
            continue;
        }
        BYTE* buf = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&buf, &frames, &flags, nullptr, nullptr))) {
            Sleep(10);
            continue;
        }
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            fwrite(buf, 1, frames * frameBytes, f);
            dataBytes += frames * frameBytes;
        } else {
            // тишина — нули чтобы время не плыло
            std::vector<uint8_t> silence((size_t)frames * frameBytes, 0);
            fwrite(silence.data(), 1, silence.size(), f);
            dataBytes += (uint32_t)silence.size();
        }
        cap->ReleaseBuffer(frames);
    }

    if (f) {
        // чиним заголовок под реальный размер
        fseek(f, 0, SEEK_SET);
        WriteHeader(f, wfx, dataBytes);
        fclose(f);
        f = nullptr;
        printf("mic file %s bytes=%u\n", g_path.c_str(), dataBytes);
        fflush(stdout);
    }
    g_secs = ok ? std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() : 0.0;
    if (cl)
        cl->Stop();
    if (cap)
        cap->Release();
    if (wfx)
        CoTaskMemFree(wfx);
    if (cl)
        cl->Release();
    if (dev)
        dev->Release();
    if (en)
        en->Release();
    CoUninitialize();
}

} // namespace

// --- непрерывный стрим для ears: пакеты в кольцо, формат отдельно.
// Файловый режим не трогаем (shared mode терпит двух захватчиков).
std::thread g_sthr;
std::atomic<bool> g_srun{ false };
std::mutex g_rmtx;
std::deque<std::vector<uint8_t>> g_ring;
std::vector<uint8_t> g_fmtBlob;
const size_t kRingMax = 3000; // ~30-60с пакетов, дальше старьё выкидываем
std::atomic<unsigned long long> g_okPkts{ 0 };
std::atomic<unsigned long long> g_failPkts{ 0 };
std::atomic<unsigned long long> g_silPkts{ 0 }; // пакеты с флагом SILENT (движку нечего дать)

void StreamWorker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* cl = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    IAudioCaptureClient* cap = nullptr;
    bool ok = false;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (SUCCEEDED(hr))
        hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    if (SUCCEEDED(hr))
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl);
    if (SUCCEEDED(hr))
        hr = cl->GetMixFormat(&wfx);
    if (SUCCEEDED(hr))
        hr = cl->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wfx, nullptr);
    if (SUCCEEDED(hr))
        hr = cl->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    // имя устройства — в консоль (диагностика «не тот микрофон»)
    if (SUCCEEDED(hr)) {
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal) {
                char nb[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, nb, sizeof(nb), nullptr, nullptr);
                printf("mic: %s\n", nb);
                fflush(stdout);
            }
            PropVariantClear(&pv);
            ps->Release();
        }
    }
    if (SUCCEEDED(hr)) {
        size_t blob = (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? 40 : sizeof(WAVEFORMATEX);
        std::lock_guard<std::mutex> lk(g_rmtx);
        g_fmtBlob.assign((uint8_t*)wfx, (uint8_t*)wfx + blob);
        g_ring.clear();
        hr = cl->Start();
        ok = SUCCEEDED(hr);
    }
    if (!ok)
        printf("mic stream hr=0x%X\n", (unsigned)hr);
    while (ok && g_srun.load()) {
        UINT32 packets = 0;
        if (FAILED(cap->GetNextPacketSize(&packets))) {
            Sleep(10);
            continue;
        }
        if (packets == 0) {
            Sleep(10);
            continue;
        }
        BYTE* buf = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&buf, &frames, &flags, nullptr, nullptr))) {
            g_failPkts++;
            Sleep(10);
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_rmtx);
            g_okPkts++;
            size_t n = (size_t)frames * wfx->nBlockAlign;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && buf && n > 0) {
                g_ring.emplace_back(buf, buf + n);
            } else if (n > 0) {
                g_silPkts++;
                g_ring.emplace_back(n, 0); // тишина нулями — время не плывёт
            }
            while (g_ring.size() > kRingMax)
                g_ring.pop_front();
        }
        cap->ReleaseBuffer(frames);
    }
    if (cl)
        cl->Stop();
    if (cap)
        cap->Release();
    if (wfx)
        CoTaskMemFree(wfx);
    if (cl)
        cl->Release();
    if (dev)
        dev->Release();
    if (en)
        en->Release();
    CoUninitialize();
}

bool StreamStart()
{
    if (g_srun.load())
        return true;
    if (g_sthr.joinable())
        g_sthr.join();
    g_srun = true;
    g_sthr = std::thread(StreamWorker);
    return true;
}

void StreamStop()
{
    g_srun = false;
    if (g_sthr.joinable())
        g_sthr.join();
    std::lock_guard<std::mutex> lk(g_rmtx);
    g_ring.clear();
}

bool StreamRead(std::vector<uint8_t>& out)
{
    std::lock_guard<std::mutex> lk(g_rmtx);
    if (g_ring.empty())
        return false;
    out = std::move(g_ring.front());
    g_ring.pop_front();
    return true;
}

bool StreamFormat(std::vector<uint8_t>& fmtBlob)
{
    std::lock_guard<std::mutex> lk(g_rmtx);
    if (g_fmtBlob.empty())
        return false;
    fmtBlob = g_fmtBlob;
    return true;
}

void StreamStats(unsigned long long& okPkts, unsigned long long& failed, unsigned long long& silentPkts)
{
    okPkts = g_okPkts.load();
    failed = g_failPkts.load();
    silentPkts = g_silPkts.load();
}

void Shutdown()
{
    StreamStop();
    Stop();
}

bool Start()
{
    if (g_rec.load())
        return true;
    char name[64];
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif
    snprintf(name, sizeof(name), "voice_%02d%02d%02d.wav", tm.tm_hour, tm.tm_min, tm.tm_sec);
    g_path = name;
    g_secs = 0.0;
    g_run = true;
    g_rec = true;
    printf("mic start %s\n", name);
    fflush(stdout);
    if (g_thr.joinable())
        g_thr.join();
    g_thr = std::thread(Worker);
    return true;
}

double Stop()
{
    if (!g_rec.load())
        return 0.0;
    g_run = false;
    if (g_thr.joinable())
        g_thr.join();
    g_rec = false;
    return g_secs;
}

const char* LastPath()
{
    return g_path.c_str();
}

bool IsRecording()
{
    return g_rec.load();
}

} // namespace mic
