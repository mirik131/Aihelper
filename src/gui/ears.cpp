#include "gui/ears.h"
#include "gui/mic.h"
#include "gui/stt.h"
#include "gui/llm.h"
#include "gui/brain.h"
#include <windows.h>
#include <mmreg.h> // WAVE_FORMAT_EXTENSIBLE
#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>

namespace ears {
namespace {

// --- конфиг VAD (классика walkie-talkie систем) ---
const int kFrameMs = 100;     // кадр энергии
const int kStartFrames = 3;   // 300мс громко — начали фразу
const int kEndFrames = 8;      // 800мс тихо — конец фразы (было 1200, шустрее отклик)
const int kPreFrames = 3;     // преролл 300мс (не режем начало)
const int kTailKeep = 3;      // хвоста тишины оставляем 300мс
const int kMinSpeech = 4;     // короче 400мс — не фраза (клики/стуки)
const int kMaxFrames = 200;   // 20с — режем принудительно
const double kAbsSpeech = 0.008; // было 0.02 — слышал только впритык, опускаем
const double kAbsSilence = 0.006;
const double kMaxSpeech = 0.03;  // потолок порога: игровой шум из колонок не глушит команды
const double kMicGain = 3.0; // усиление тихого микрофона (крути тут: 1.0 — без)
const int kSpecFrames = 20;     // упреждающая транскрибация на 2-й секунде
const size_t kQMax = 4; // очередь фраз (старьё выкидываем)

std::thread g_vadThr, g_procThr;
std::mutex g_mtx;
std::atomic<bool> g_stop{ false };
std::atomic<bool> g_maybeMuted{ false }; // ~30с гробовой тишины при работе
bool g_general = false, g_pc = false, g_mus = false;
bool g_requireName = true;
int g_phase = 0; // 0 выкл, 1 слушаю, 2 думаю, 3 делаю
std::string g_heard, g_result;
struct QItem {
    std::string path;
    bool spec = false; // упреждающий кусок (финал фразы ещё пишется)
    unsigned long long id = 0;
};
std::deque<QItem> g_q;

bool Enabled()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_general && (g_pc || g_mus);
}

bool PcOn()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_pc;
}

bool MusOn()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_mus;
}

// нижний регистр UTF-8 (ASCII + кириллица) — для поиска обращения
std::string LowerRu(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            o += (char)tolower(c);
            i++;
        } else if (c == 0xD0 && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            if (d >= 0x90 && d <= 0x9F) {
                o += (char)0xD0;
                o += (char)(d + 0x20);
            } else if (d >= 0xA0 && d <= 0xAF) {
                o += (char)0xD1;
                o += (char)(d - 0x20);
            } else if (d == 0x81) {
                o += (char)0xD1;
                o += (char)0x91;
            } else {
                o += (char)c;
                o += (char)d;
            }
            i += 2;
        } else {
            o += (char)c;
            i++;
        }
    }
    return o;
}

// есть ли обращение по имени («фикс ...», «... фикс»)
bool HasName(const std::string& heard)
{
    std::string l = LowerRu(heard);
    static const char* names[] = { "фикс", "фикас", "фиксу", "фиксом", "фиксе", "фикси", "fix" };
    for (auto& n : names) {
        if (l.find(n) != std::string::npos)
            return true;
    }
    return false;
}

bool RequireName()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_requireName;
}

void SetPhase(int ph, const std::string& heard, const std::string& result)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_phase = ph;
    g_heard = heard;
    g_result = result;
}

void SetPhaseOnly(int ph)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_phase = ph;
}

std::string TrimSp(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (unsigned char)s[a] <= ' ')
        a++;
    size_t b = s.size();
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        b--;
    return s.substr(a, b - a);
}

bool IsMusicAct(const std::string& a)
{
    return a == "musicSearch" || a == "mediaLike";
}

bool IsMediaAct(const std::string& a)
{
    return a == "mediaPlay" || a == "mediaNext" || a == "mediaPrev" ||
           a == "volUp" || a == "volDown";
}

void SleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// RMS моно-кадра из сырых байтов стрима
double MonoRms(const std::vector<uint8_t>& b, int ch, int bps, bool isFloat)
{
    if (ch <= 0 || (bps != 2 && bps != 3 && bps != 4) || b.empty())
        return 0;
    size_t frames = b.size() / ((size_t)bps * ch);
    if (!frames)
        return 0;
    double sum = 0;
    for (size_t i = 0; i < frames; i++) {
        double s = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t* p = b.data() + (i * ch + c) * bps;
            float v = 0;
            if (bps == 2) {
                int16_t s16 = (int16_t)(p[0] | (p[1] << 8));
                v = s16 / 32768.0f;
            } else if (bps == 3) {
                int32_t s24 = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
                if (s24 & 0x800000)
                    s24 |= (int32_t)0xFF000000;
                v = s24 / 8388608.0f;
            } else {
                memcpy(&v, p, 4);
            }
            s += v;
        }
        s /= ch;
        sum += s * s;
    }
    return sqrt(sum / frames);
}

// программное усиление микрофона (тихий/дальний микрофон)
void ApplyGain(std::vector<uint8_t>& b, int ch, int bps, bool isFloat, double gain)
{
    if (gain <= 1.0 || ch <= 0 || (bps != 2 && bps != 3 && bps != 4) || b.empty())
        return;
    size_t frames = b.size() / ((size_t)bps * ch);
    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < ch; c++) {
            uint8_t* p = b.data() + (i * ch + c) * bps;
            if (bps == 2) {
                int v = (int)(int16_t)(p[0] | (p[1] << 8));
                v = (int)(v * gain);
                if (v > 32767)
                    v = 32767;
                if (v < -32768)
                    v = -32768;
                p[0] = (uint8_t)(v & 0xFF);
                p[1] = (uint8_t)((v >> 8) & 0xFF);
            } else if (bps == 3) {
                int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
                if (v & 0x800000)
                    v |= (int32_t)0xFF000000;
                v = (int32_t)(v * gain);
                if (v > 8388607)
                    v = 8388607;
                if (v < -8388608)
                    v = -8388608;
                p[0] = (uint8_t)(v & 0xFF);
                p[1] = (uint8_t)((v >> 8) & 0xFF);
                p[2] = (uint8_t)((v >> 16) & 0xFF);
            } else {
                float v;
                memcpy(&v, p, 4);
                v = (float)(v * gain);
                if (v > 1.0f)
                    v = 1.0f;
                if (v < -1.0f)
                    v = -1.0f;
                memcpy(p, &v, 4);
            }
        }
    }
}

// сырые байты + формат -> WAV файл
bool WriteWav(const char* path, const std::vector<uint8_t>& fmtBlob, const std::vector<uint8_t>& data)
{
    if (fmtBlob.size() < 18 || data.empty())
        return false;
    WAVEFORMATEX wfx = {};
    memcpy(&wfx, fmtBlob.data(), sizeof(wfx));
    uint16_t fmtSize = (wfx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? 40 : 18;
    if (fmtBlob.size() < fmtSize)
        return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    uint32_t dataBytes = (uint32_t)data.size();
    uint32_t riffSize = 4 + (8 + fmtSize) + (8 + dataBytes);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fs = fmtSize;
    fwrite(&fs, 4, 1, f);
    fwrite(fmtBlob.data(), 1, fmtSize, f);
    if (fmtSize == 18) {
        uint16_t zero = 0;
        fwrite(&zero, 2, 1, f);
    }
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

void Enqueue(const std::string& wav, bool spec, unsigned long long id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_q.size() >= kQMax) {
        printf("ears: queue full, drop oldest\n");
        fflush(stdout);
        g_q.pop_front();
    }
    QItem it;
    it.path = wav;
    it.spec = spec;
    it.id = id;
    g_q.push_back(it);
}

struct Frame {
    std::vector<uint8_t> b;
    bool speech = false;
};

// VAD: захват идёт всегда, фразы режем по тишине и кладём в очередь
void VadThread()
{
    printf("ears: vad on\n");
    fflush(stdout);
    std::vector<uint8_t> fmt;
    bool streaming = false;
    int sampleRate = 0, channels = 0, bytesPerSample = 0;
    bool isFloat = false;
    size_t framesPerChunk = 0;
    std::vector<uint8_t> frameBytes;
    size_t frameFrames = 0;
    std::deque<Frame> pre;
    std::vector<Frame> utt;
    int consecLoud = 0, silenceFrames = 0, speechFrames = 0;
    bool inSpeech = false;
    double noise = 0.003; // пол шума (EMA по тихим кадрам)
    double peak = 0;      // диагностика уровней
    int lvlN = 0;
    unsigned long long lastOk = 0, lastFail = 0, lastSil = 0;
    int quietTicks = 0; // сколько окон подряд гробовая тишина при включённом слушателе
    double spkSum = 0;    // средний RMS речи в текущей фразе (диагностика резки)
    int spkN = 0, gapN = 0; // gapN — голодания стрима (дропы пакетов)
    int wavNo = 0;
    unsigned long long uttSeq = 0; // id фраз (дедуп спек/финал)
    unsigned long long curId = 0;
    bool specSent = false;

    auto resetUtt = [&]() {
        utt.clear();
        consecLoud = 0;
        silenceFrames = 0;
        speechFrames = 0;
        inSpeech = false;
        spkSum = 0;
        spkN = 0;
        specSent = false;
    };

    while (!g_stop) {
        if (!Enabled()) {
            if (streaming) {
                mic::StreamStop();
                streaming = false;
            }
            g_maybeMuted = false;
            resetUtt();
            pre.clear();
            frameBytes.clear();
            frameFrames = 0;
            SleepMs(300);
            continue;
        }
        if (!streaming) {
            mic::StreamStart();
            if (!mic::StreamFormat(fmt)) {
                SleepMs(200);
                continue;
            }
            WAVEFORMATEX wfx = {};
            memcpy(&wfx, fmt.data(), sizeof(wfx));
            channels = wfx.nChannels;
            sampleRate = wfx.nSamplesPerSec;
            bytesPerSample = wfx.wBitsPerSample / 8;
            isFloat = false;
            if (wfx.wFormatTag == 3)
                isFloat = true;
            else if (wfx.wFormatTag == 0xFFFE && fmt.size() >= 40) {
                uint32_t sub = fmt[24] | (fmt[25] << 8) | (fmt[26] << 16) | (fmt[27] << 24);
                isFloat = (sub == 3);
            }
            if (sampleRate <= 0 || channels <= 0 ||
                (bytesPerSample != 2 && bytesPerSample != 3 && bytesPerSample != 4)) {
                printf("ears: bad fmt %dHz ch=%d bps=%d\n", sampleRate, channels, bytesPerSample);
                fflush(stdout);
                mic::StreamStop();
                SleepMs(1000);
                continue;
            }
            framesPerChunk = (size_t)sampleRate * kFrameMs / 1000;
            streaming = true;
            printf("ears: stream %dHz ch=%d\n", sampleRate, channels);
            fflush(stdout);
        }
        if (mic::IsRecording()) { // ручной микрофон — VAD на паузе, кадры сливаем
            std::vector<uint8_t> dump;
            while (mic::StreamRead(dump)) {
            }
            resetUtt();
            pre.clear();
            frameBytes.clear();
            frameFrames = 0;
            SleepMs(100);
            continue;
        }
        // добираем 100мс кадр (ждём до 300мс)
        bool have = false;
        auto t0 = std::chrono::steady_clock::now();
        while (frameFrames < framesPerChunk && !g_stop) {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el > 0.3) {
                if (frameFrames < framesPerChunk / 2)
                    gapN++; // стрим голодает — возможные заикания/резка
                break;
            }
            std::vector<uint8_t> pkt;
            if (mic::StreamRead(pkt)) {
                ApplyGain(pkt, channels, bytesPerSample, isFloat, kMicGain);
                size_t fr = pkt.size() / ((size_t)bytesPerSample * channels);
                frameBytes.insert(frameBytes.end(), pkt.begin(), pkt.end());
                frameFrames += fr;
                have = true;
            } else {
                SleepMs(10);
            }
        }
        if (!have || frameFrames == 0)
            continue;
        double rms = MonoRms(frameBytes, channels, bytesPerSample, isFloat);
        if (rms > peak)
            peak = rms;
        if (++lvlN >= 50) { // раз в ~5с: видно глухой микрофон и пол шума
            unsigned long long okN = 0, failN = 0, silN = 0;
            mic::StreamStats(okN, failN, silN);
            unsigned long long dFail = failN >= lastFail ? failN - lastFail : 0;
            unsigned long long dSil = silN >= lastSil ? silN - lastSil : 0;
            unsigned long long dOk = okN >= lastOk ? okN - lastOk : 0;
            lastOk = okN;
            lastFail = failN;
            lastSil = silN;
            int silPct = (dOk + dSil) ? (int)(dSil * 100 / (dOk + dSil)) : 0;
            printf("ears lvl: peak=%.3f noise=%.4f gaps=%d micE=%llu sil=%d%%\n",
                peak, noise, gapN, dFail, silPct);
            fflush(stdout);
            // сторож — СМОТРИМ ДО обнуления: минута гробовой тишины при
            // включённом слушателе = дефолтный микрофон сменился/ушёл в
            // эксклюзив: переоткрываем текущий дефолт
            if (Enabled() && streaming && peak < 0.005 && noise < 0.004)
                quietTicks++;
            else
                quietTicks = 0;
            peak = 0;
            lvlN = 0;
            gapN = 0;
            g_maybeMuted = (quietTicks >= 6);
            if (quietTicks >= 12) {
                quietTicks = 0;
                printf("ears: mic re-open (silence watchdog)\n");
                fflush(stdout);
                mic::StreamStop();
                streaming = false;
            }
            // сторож ошибок: девайс отвалился/ушёл в эксклюзив (дота любит) —
            // дёргаем захват заново, вдруг оживёт
            if (dFail > 50 && streaming) {
                printf("ears: mic re-open (error watchdog: %llu)\n", dFail);
                fflush(stdout);
                mic::StreamStop();
                streaming = false;
            }
        }
        // пороги: абсолютный пол + относительный от шума (адаптация к комнате)
        double thSpeech = kAbsSpeech, t2 = noise * 3.0;
        if (t2 > thSpeech)
            thSpeech = t2;
        if (thSpeech > kMaxSpeech)
            thSpeech = kMaxSpeech;
        double thSil = kAbsSilence, t3 = noise * 2.0;
        if (t3 > thSil)
            thSil = t3;
        if (thSil > thSpeech)
            thSil = thSpeech;
        bool loud = rms >= thSpeech;
        bool quiet = rms <= thSil;
        Frame fr;
        fr.b = std::move(frameBytes);
        frameBytes.clear();
        frameFrames = 0;

        if (!inSpeech) {
            if (loud) {
                consecLoud++;
                pre.push_back(std::move(fr));
                while (pre.size() > (size_t)kPreFrames)
                    pre.pop_front();
                if (consecLoud >= kStartFrames) {
                    inSpeech = true;
                    silenceFrames = 0;
                    speechFrames = 0;
                    curId = ++uttSeq;
                    specSent = false;
                    for (auto& pf : pre) {
                        pf.speech = true;
                        utt.push_back(std::move(pf));
                    }
                    speechFrames = (int)utt.size();
                    pre.clear();
                }
            } else {
                consecLoud = 0;
                noise = noise * 0.95 + rms * 0.05;
                pre.push_back(std::move(fr));
                while (pre.size() > (size_t)kPreFrames)
                    pre.pop_front();
            }
            continue;
        }
        // внутри фразы: гистерезис (порог тишины вдвое ниже) + адаптивный хвост
        // (короткие фразы рано не режем — чинит «о»/«и» вместо команды)
        bool reallyQuiet = rms <= thSil * 0.5;
        fr.speech = !quiet;
        if (fr.speech) {
            speechFrames++;
            silenceFrames = 0;
            spkSum += rms;
            spkN++;
        } else if (reallyQuiet) {
            silenceFrames++;
        } else {
            // между громко и тихо — счётчик тишины не трогаем
        }
        utt.push_back(std::move(fr));
        // упреждение: на 2-й секунде шлём кусок на распознавание не дожидаясь конца
        if (!specSent && (int)utt.size() >= kSpecFrames) {
            specSent = true;
            std::vector<uint8_t> sraw;
            for (auto& f : utt)
                sraw.insert(sraw.end(), f.b.begin(), f.b.end());
            wavNo++;
            char spath[64];
            snprintf(spath, sizeof(spath), "ears_s%03d.wav", wavNo % 1000);
            if (!sraw.empty() && sampleRate > 0 && WriteWav(spath, fmt, sraw)) {
                printf("ears spec: #%llu\n", curId);
                fflush(stdout);
                Enqueue(spath, true, curId);
            }
        }
        int endNeed = ((int)utt.size() * kFrameMs < 2000) ? 14 : kEndFrames;
        if (silenceFrames >= endNeed || (int)utt.size() >= kMaxFrames) {
            // финал: хвост тишины режем до kTailKeep
            size_t keep = utt.size();
            int tail = 0;
            while (keep > 0 && !utt[keep - 1].speech && tail < (kEndFrames - kTailKeep)) {
                keep--;
                tail++;
            }
            std::vector<uint8_t> raw;
            for (size_t i = 0; i < keep; i++)
                raw.insert(raw.end(), utt[i].b.begin(), utt[i].b.end());
            double durS = 0;
            if (sampleRate > 0 && channels > 0 && bytesPerSample > 0)
                durS = (double)raw.size() / (bytesPerSample * channels * sampleRate);
            if (speechFrames >= kMinSpeech && !raw.empty()) {
                wavNo++;
                char path[64];
                snprintf(path, sizeof(path), "ears_%03d.wav", wavNo % 1000);
                if (WriteWav(path, fmt, raw)) {
                    printf("ears utt: %.1fs spk=%.3f gaps=%d\n", durS,
                        spkN ? spkSum / spkN : 0, gapN);
                    fflush(stdout);
                    Enqueue(path, false, curId);
                }
            }
            resetUtt();
        }
    }
    if (streaming)
        mic::StreamStop();
    printf("ears: vad off\n");
    fflush(stdout);
}

// обработка очереди: STT -> LLM -> выполнение (захват тем временем идёт)
void ProcThread()
{
    printf("ears: proc on\n");
    fflush(stdout);
    std::deque<std::pair<unsigned long long, int>> specExec; // id фразы -> сколько исполнили
    while (!g_stop) {
        QItem it;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_q.empty()) {
                it = g_q.front();
                g_q.pop_front();
            }
        }
        // NB: Enabled() лочит тот же мьютекс — только ВНЕ лока выше
        bool en = Enabled();
        if (it.path.empty()) {
            if (!en) {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_q.clear();
            }
            if (en)
                SetPhaseOnly(1); // слушаю (результат прошлой держим)
            else
                SetPhase(0, "", "");
            SleepMs(200);
            continue;
        }
        // STT: ждём освобождения до 5с (чат главнее), дальше фразу роняем
        bool started = false;
        for (int i = 0; i < 50 && !g_stop; i++) {
            if (stt::TranscribeFile(it.path.c_str())) {
                started = true;
                break;
            }
            SleepMs(100);
        }
        if (!started)
            continue;
        std::string heard;
        bool got = false;
        for (int i = 0; i < 450 && !g_stop; i++) {
            if (!Enabled())
                break;
            if (stt::Poll(heard)) {
                got = true;
                break;
            }
            SleepMs(100);
        }
        if (!got)
            continue;
        heard = TrimSp(heard);
        // мусор из 1 фонемы («и», «о», «а») — сразу в утиль, без LLM
        {
            int cps = 0;
            for (unsigned char c : heard) {
                if ((c & 0xC0) != 0x80)
                    cps++;
            }
            if (cps < 2)
                continue;
        }
        if (!Enabled())
            continue;
        if (RequireName() && !HasName(heard)) {
            printf("ears skip (no name): %s\n", heard.c_str());
            fflush(stdout);
            SetPhase(1, heard, "мимо (без обращения)");
            continue;
        }
        // уровень 1: мгновенный матчер (0мс, без LLM)
        {
            std::string cmd = brain::InstantCmd(heard);
            if (!cmd.empty()) {
                size_t cc = cmd.find(':');
                std::string act = (cc == std::string::npos) ? cmd : cmd.substr(0, cc);
                std::string prm = (cc == std::string::npos) ? "" : cmd.substr(cc + 1);
                if (act == "musicSearch") {
                    size_t c2 = prm.find(':');
                    std::string pv = (c2 == std::string::npos) ? "auto" : prm.substr(0, c2);
                    std::string qq = (c2 == std::string::npos) ? prm : prm.substr(c2 + 1);
                    prm = brain::FixMusicProvider(heard, pv) + ":" + qq;
                }
                bool pc = PcOn(), mus = MusOn();
                bool allow = IsMediaAct(act) ? (pc || mus) : (IsMusicAct(act) ? mus : pc);
                if (allow) {
                    // сколько раз просили: повторы в одной фразе исполняем все.
                    // Уже исполненное по упреждению вычитаем.
                    int want = 1;
                    std::vector<std::string> keys = brain::InstantKeys(act);
                    if (!keys.empty()) {
                        std::string low = LowerRu(heard);
                        want = 0;
                        for (auto& k : keys) {
                            size_t p = 0;
                            while (want < 3 && (p = low.find(k, p)) != std::string::npos) {
                                want++;
                                low.erase(p, k.size());
                            }
                        }
                        if (want < 1)
                            want = 1;
                    }
                    int already = 0;
                    for (auto& e : specExec) {
                        if (e.first == it.id) {
                            already = e.second;
                            break;
                        }
                    }
                    int todo = want - already;
                    if (todo <= 0) {
                        printf("ears dup: skip #%llu\n", it.id);
                        fflush(stdout);
                        continue;
                    }
                    std::string done;
                    for (int k = 0; k < todo; k++) {
                        done = brain::ExecCmd(act, prm);
                        if (done.empty())
                            done = cmd;
                        if (k + 1 < todo)
                            SleepMs(250);
                    }
                    bool found = false;
                    for (auto& e : specExec) {
                        if (e.first == it.id) {
                            e.second = already + todo;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        specExec.emplace_back(it.id, todo);
                        if (specExec.size() > 30)
                            specExec.pop_front();
                    }
                    printf("ears instant: %s x%d -> %s\n", act.c_str(), todo, done.c_str());
                    fflush(stdout);
                    SetPhase(3, heard, done);
                } else {
                    SetPhase(1, heard, IsMusicAct(act) ? "Включи Music." : "Включи Control PC.");
                }
                continue;
            }
        }
        if (it.spec)
            continue; // упреждение не сматчилось — ждём финал фразы
        SetPhase(2, heard, "");
        printf("ears heard: %s\n", heard.c_str());
        fflush(stdout);
        bool asked = false;
        for (int i = 0; i < 50 && !g_stop; i++) {
            if (llm::AskFastAsync(heard)) {
                asked = true;
                break;
            }
            SleepMs(100);
        }
        if (!asked)
            continue;
        std::string ans;
        bool agot = false;
        for (int i = 0; i < 400 && !g_stop; i++) {
            if (!Enabled())
                break;
            if (llm::FastPoll(ans)) {
                agot = true;
                break;
            }
            SleepMs(100);
        }
        if (!agot)
            continue;
        ans = TrimSp(ans);
        if (ans.rfind("CMD:", 0) != 0) {
            SetPhase(1, heard, ans); // поболтала — только в статус
            continue;
        }
        size_t c1 = ans.find(':', 4);
        std::string act = (c1 == std::string::npos) ? ans.substr(4) : ans.substr(4, c1 - 4);
        std::string prm = (c1 == std::string::npos) ? "" : ans.substr(c1 + 1);
        if (act == "musicSearch") {
            // youtube без просьбы -> выбор юзера
            size_t c2 = prm.find(':');
            std::string pv = (c2 == std::string::npos) ? "youtube" : prm.substr(0, c2);
            std::string qq = (c2 == std::string::npos) ? prm : prm.substr(c2 + 1);
            prm = brain::FixMusicProvider(heard, pv) + ":" + qq;
        }
        bool pc = PcOn(), mus = MusOn();
        bool allow = IsMediaAct(act) ? (pc || mus) : (IsMusicAct(act) ? mus : pc);
        std::string done;
        if (allow) {
            done = brain::ExecCmd(act, prm);
            if (done.empty())
                done = ans;
        } else if (!pc && !mus) {
            done = "Всё выключено.";
        } else if (IsMusicAct(act)) {
            done = "Включи Music.";
        } else {
            done = "Включи Control PC.";
        }
        printf("ears done: %s -> %s\n", act.c_str(), done.c_str());
        fflush(stdout);
        SetPhase(3, heard, done);
    }
    printf("ears: proc off\n");
    fflush(stdout);
}

void Ensure()
{
    if (!g_vadThr.joinable() || !g_procThr.joinable()) {
        if (g_vadThr.joinable())
            g_vadThr.join();
        if (g_procThr.joinable())
            g_procThr.join();
        g_stop = false;
        g_vadThr = std::thread(VadThread);
        g_procThr = std::thread(ProcThread);
    }
}

} // namespace

void SetGeneral(bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_general = on;
    }
    Ensure();
}

void SetControlPc(bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pc = on;
    }
    Ensure();
}

void SetMusic(bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_mus = on;
    }
    Ensure();
}

void SetRequireName(bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_requireName = on;
    }
    Ensure();
}

bool Snapshot(Snap& out)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    out.running = g_general && (g_pc || g_mus);
    out.phase = out.running ? g_phase : 0;
    out.heard = g_heard;
    out.result = g_result;
    out.maybeMuted = out.running && g_maybeMuted.load();
    return true;
}

void Shutdown()
{
    g_stop = true;
    if (g_vadThr.joinable())
        g_vadThr.join();
    if (g_procThr.joinable())
        g_procThr.join();
    mic::StreamStop();
}

} // namespace ears
