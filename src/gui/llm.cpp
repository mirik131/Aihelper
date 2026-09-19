#include "gui/llm.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

namespace llm {
namespace {

// быстрая и умная из живых на Groq (проверено руками). Запасная — 20b.
const char* kModel = "openai/gpt-oss-120b";
const char* kModelFallback = "openai/gpt-oss-20b";
const char* kModelFast = "openai/gpt-oss-20b"; // голос: скорость+послушность (замер ~0.5с)

const char* kSystem =
    "Ты FixAi — голосовой помощник в приложении. Отвечай по-русски, коротко (1-3 предложения), без воды. "
    "Если пользователь просит ДЕЙСТВИЕ — ответь РОВНО одной строкой вида CMD:действие:параметр без пояснений. "
    "Действия: openSite:<адрес> — сайт; openApp:<имя> — приложение (имена пиши как услышал: телеграм, вскод, блокнот); "
    "closeApp:<имя> — закрыть; openFolder:<папка> — папка (Рабочий стол, Документы, Загрузки, Музыка, Диск С); "
    "typeText:<текст> — напечатать текст как есть (команда «напиши ...» — весь текст после неё целиком); "
    "musicSearch:<где>:<что> — найти и включить музыку (где: youtube, tiktok, spotify, yandex, soundcloud, piped без рекламы, или auto если не уточнили; «включи музыку из тиктока» без названия — пустое что); "
    "closeTab:<пусто> — закрыть вкладку в активном окне; "
    "openSvc:<music|work|fun> — открыть выбранный сервис («открой работу», «включи развлечения»); "
    "Сайты (ютуб, тикток, вк) открывай через openSite: youtube.com, tiktok.com, vk.com. "
    "Популярное раскрывай сам: kwork→kwork.ru, авито→avito.ru, озон→ozon.ru, гугл→google.com. "
    "«Закрой вкладку» — closeTab (текущая); «закрой ютуб/телеграм» — closeApp ГОЛЫМ именем без .com (ютуб, а не youtube.com). "
    "mediaLike:<пусто> — лайк (ТикТок); screenshot:<пусто>; mediaPlay/mediaNext/mediaPrev:<пусто> — плеер; "
    "volUp:<пусто> — громче; volDown:<пусто> — тише (громкость плеера, не системы). "
    "Во всех остальных случаях — обычный короткий ответ, никаких CMD.";

struct HistMsg {
    std::string role;
    std::string text;
};

std::thread g_thr;
std::mutex g_mtx;
bool g_busy = false;
bool g_done = false;
std::string g_answer;
std::thread g_fthr; // быстрый голосовой путь (состояние отдельно от чата)
bool g_fbusy = false;
bool g_fdone = false;
std::string g_fanswer;
std::string g_fquery;
std::vector<HistMsg> g_hist; // без system, его клеим при каждом запросе

// ключ из .env (рядом с exe или в проекте)
std::string LoadKey()
{
    const char* paths[] = { ".env", "../../.env" };
    for (const char* p : paths) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "r") != 0 || !f)
            continue;
        char line[512];
        std::string key;
        while (fgets(line, sizeof(line), f)) {
            std::string s = line;
            size_t a = s.find("GROQ_API_KEY");
            if (a == std::string::npos)
                continue;
            size_t eq = s.find('=', a);
            if (eq == std::string::npos)
                continue;
            size_t b = eq + 1;
            while (b < s.size() && (s[b] == ' ' || s[b] == '\t'))
                b++;
            size_t e = s.size();
            while (e > b && (s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == ' ' || s[e - 1] == '\t'))
                e--;
            if (e > b) {
                key = s.substr(b, e - b);
                break;
            }
        }
        fclose(f);
        if (!key.empty())
            return key;
    }
    return "";
}

// JSON-экранирование (кириллица едет байтами UTF-8 как есть)
std::string Escape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

// строковое поле JSON + развёртка escapes (включая \u)
std::string ExtractField(const std::string& js, const std::string& field)
{
    size_t p = js.find("\"" + field + "\"");
    if (p == std::string::npos)
        return "";
    p = js.find(':', p);
    if (p == std::string::npos)
        return "";
    p++;
    while (p < js.size() && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r'))
        p++;
    if (p >= js.size() || js[p] != '"')
        return "";
    p++;
    std::string out;
    while (p < js.size() && js[p] != '"') {
        if (js[p] == '\\' && p + 1 < js.size()) {
            char e = js[p + 1];
            if (e == 'n') { out += '\n'; p += 2; continue; }
            if (e == 't') { out += '\t'; p += 2; continue; }
            if (e == 'r') { out += '\r'; p += 2; continue; }
            if (e == '"') { out += '"'; p += 2; continue; }
            if (e == '\\') { out += '\\'; p += 2; continue; }
            if (e == '/') { out += '/'; p += 2; continue; }
            if (e == 'u' && p + 5 < js.size()) {
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = js[p + 2 + i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        cp |= (unsigned)(h - 'A' + 10);
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 11 < js.size() &&
                    js[p + 6] == '\\' && js[p + 7] == 'u') {
                    unsigned lo = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = js[p + 8 + i];
                        lo <<= 4;
                        if (h >= '0' && h <= '9')
                            lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            lo |= (unsigned)(h - 'A' + 10);
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                if (cp < 0x80) {
                    out += (char)cp;
                } else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                p += 6;
                continue;
            }
            out += e;
            p += 2;
            continue;
        }
        out += js[p++];
    }
    return out;
}

// HTTPS-сессия одна на поток: без нового TLS-хендшейка каждый раз (~-0.3с с запроса)
HINTERNET GroqSession()
{
    thread_local HINTERNET s = nullptr;
    if (!s) {
        s = WinHttpOpen(L"helper_bot", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
        if (s)
            WinHttpSetTimeouts(s, 8000, 8000, 8000, 30000);
    }
    return s;
}

// один синхронный запрос, пусто при любой беде
std::string ChatOnce(const std::string& key, const char* model, const std::string& body)
{
    std::string out;
    HINTERNET ses = GroqSession();
    if (!ses)
        return out;
    HINTERNET con = WinHttpConnect(ses, L"api.groq.com", 443, 0);
    HINTERNET req = nullptr;
    if (con)
        req = WinHttpOpenRequest(con, L"POST", L"/openai/v1/chat/completions",
                                 NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (req) {
        std::string hs = "Content-Type: application/json\r\nAuthorization: Bearer " + key + "\r\n";
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
                std::string content = ExtractField(resp, "content");
                if (!content.empty())
                    out = content;
            }
        }
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    return out;
}

// SSE-стрим команды: первую CMD-строку возвращаем не дожидаясь [DONE],
// иначе полный текст. Пусто при любой беде.
std::string ChatStreamCmd(const std::string& key, const char* model, const std::string& userText)
{
    std::string out;
    std::string body = "{\"model\":\"";
    body += model;
    body += "\",\"temperature\":0.7,\"max_tokens\":300,\"stream\":true,\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"" + Escape(kSystem) + "\"},";
    body += "{\"role\":\"user\",\"content\":\"" + Escape(userText) + "\"}]}";
    HINTERNET ses = GroqSession();
    if (!ses)
        return out;
    HINTERNET con = WinHttpConnect(ses, L"api.groq.com", 443, 0);
    HINTERNET req = nullptr;
    if (con)
        req = WinHttpOpenRequest(con, L"POST", L"/openai/v1/chat/completions",
                                 NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    std::string buf;
    bool done = false;
    if (req) {
        std::string hs = "Content-Type: application/json\r\nAuthorization: Bearer " + key + "\r\n";
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
                while (!done) {
                    DWORD av = 0;
                    if (!WinHttpQueryDataAvailable(req, &av) || av == 0)
                        break;
                    size_t at = buf.size();
                    buf.resize(at + av);
                    DWORD rd = 0;
                    if (!WinHttpReadData(req, &buf[at], av, &rd)) {
                        buf.resize(at);
                        break;
                    }
                    buf.resize(at + rd);
                    if (rd == 0)
                        break;
                    // режем готовые строки, хвост ждёт следующий чанк
                    size_t ls;
                    while (!done && (ls = buf.find('\n')) != std::string::npos) {
                        std::string line = buf.substr(0, ls);
                        buf.erase(0, ls + 1);
                        if (line.size() > 6 && line.compare(0, 6, "data: ") == 0) {
                            std::string payload = line.substr(6);
                            size_t s = 0;
                            while (s < payload.size() && (unsigned char)payload[s] <= ' ')
                                s++;
                            payload.erase(0, s);
                            if (payload == "[DONE]") {
                                done = true;
                            } else {
                                out += ExtractField(payload, "content");
                                // CMD однострочный — исполняем по первой строке сразу
                                size_t nl = out.find('\n');
                                if (out.rfind("CMD:", 0) == 0 && nl != std::string::npos) {
                                    out = out.substr(0, nl);
                                    done = true;
                                }
                                if (out.size() > 2000)
                                    done = true;
                            }
                        }
                    }
                }
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
    std::string answer;
    try {
        std::string key = LoadKey();
        if (!key.empty()) {
            auto buildBody = [&](const char* model) {
                std::string body = "{\"model\":\"";
                body += model;
                body += "\",\"temperature\":0.7,\"max_tokens\":300,\"messages\":[";
                body += "{\"role\":\"system\",\"content\":\"" + Escape(kSystem) + "\"},";
                std::lock_guard<std::mutex> lk(g_mtx);
                for (auto& m : g_hist)
                    body += "{\"role\":\"" + m.role + "\",\"content\":\"" + Escape(m.text) + "\"},";
                if (!body.empty() && body.back() == ',')
                    body.pop_back();
                body += "]}";
                return body;
            };
            answer = ChatOnce(key, kModel, buildBody(kModel));
            if (answer.empty())
                answer = ChatOnce(key, kModelFallback, buildBody(kModelFallback));
        }
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!answer.empty()) {
            g_hist.push_back({ "assistant", answer });
            while (g_hist.size() > 14)
                g_hist.erase(g_hist.begin());
        }
        g_answer = answer;
        g_done = true;
        g_busy = false;
    }
    printf("llm done: %s\n", answer.empty() ? "(empty/fallback)" : "ok");
    fflush(stdout);
}

} // namespace

void FastWorker()
{
    std::string answer;
    try {
        std::string key = LoadKey();
        if (!key.empty()) {
            std::string q;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                q = g_fquery;
            }
            answer = ChatStreamCmd(key, kModelFast, q);
            if (answer.empty())
                answer = ChatStreamCmd(key, kModelFast, q); // разовый ретрай на икоту API
        }
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_fanswer = answer;
        g_fdone = true;
        g_fbusy = false;
    }
    printf("llm fast: %s\n", answer.empty() ? "(empty)" : answer.substr(0, 60).c_str());
    fflush(stdout);
}

bool AskAsync(const std::string& userText)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_busy)
        return false;
    g_busy = true;
    g_done = false;
    g_answer.clear();
    g_hist.push_back({ "user", userText });
    if (g_thr.joinable())
        g_thr.join();
    g_thr = std::thread(Worker);
    return true;
}

bool Poll(std::string& outAnswer)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_done)
        return false;
    g_done = false;
    outAnswer = g_answer;
    return true;
}

bool Busy()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_busy;
}

bool AskFastAsync(const std::string& userText)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_fbusy || g_busy)
        return false;
    g_fbusy = true;
    g_fdone = false;
    g_fanswer.clear();
    g_fquery = userText;
    if (g_fthr.joinable())
        g_fthr.join();
    g_fthr = std::thread(FastWorker);
    return true;
}

bool FastPoll(std::string& outAnswer)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_fdone)
        return false;
    g_fdone = false;
    outAnswer = g_fanswer;
    return true;
}

bool FastBusy()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_fbusy;
}

void Shutdown()
{
    if (g_thr.joinable())
        g_thr.join();
    if (g_fthr.joinable())
        g_fthr.join();
}

std::string ApiKey()
{
    return LoadKey();
}

std::string JsonField(const std::string& js, const std::string& field)
{
    return ExtractField(js, field);
}

} // namespace llm
