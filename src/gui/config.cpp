#include "gui/config.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <map>
#include <mutex>

namespace config {
namespace {

std::mutex g_mtx;
std::map<std::string, std::string> g_kv;
std::string g_dir;
bool g_ok = false;

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

bool EnsureDir(const std::string& d)
{
    DWORD a = GetFileAttributesA(d.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        return true;
    return CreateDirectoryA(d.c_str(), nullptr) != FALSE;
}

void Save()
{
    if (!g_ok)
        return;
    std::string p = g_dir + "\\config.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "w") != 0 || !f)
        return;
    fputs("# FixAi config (key=value)\n", f);
    for (auto& kv : g_kv) {
        std::string line = kv.first + "=" + kv.second + "\n";
        fputs(line.c_str(), f);
    }
    fclose(f);
}

void Load()
{
    std::string p = g_dir + "\\config.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f)
        return;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        std::string s = TrimSp(buf);
        if (s.empty() || s[0] == '#' || s[0] == ';')
            continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = TrimSp(s.substr(0, eq));
        std::string v = TrimSp(s.substr(eq + 1));
        if (!k.empty())
            g_kv[k] = v;
    }
    fclose(f);
}

// разовый импорт старых выборов (потом файлы удаляем чтобы не путались)
void ImportLegacy()
{
    if (g_kv.count("svc.music") || g_kv.count("svc.work") || g_kv.count("svc.fun"))
        return;
    bool imported = false;
    FILE* f = nullptr;
    if (fopen_s(&f, "svc_sel.txt", "r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string s = TrimSp(line);
            size_t sp = s.find(' ');
            if (sp == std::string::npos)
                continue;
            std::string k = s.substr(0, sp);
            std::string v = TrimSp(s.substr(sp + 1));
            if (v.empty() || v.size() > 200)
                continue;
            if (k == "music" || k == "work" || k == "fun") {
                g_kv["svc." + k] = v;
                imported = true;
            }
        }
        fclose(f);
    } else if (fopen_s(&f, "music_sel.txt", "r") == 0 && f) {
        char b[32] = {};
        if (fgets(b, sizeof(b), f)) {
            std::string v = TrimSp(b);
            static const char* urls[][2] = {
                { "youtube", "https://www.youtube.com/" }, { "tiktok", "https://www.tiktok.com/" },
                { "spotify", "https://open.spotify.com/" }, { "yandex", "https://music.yandex.ru/" },
                { "soundcloud", "https://soundcloud.com/" },
            };
            for (auto& u : urls) {
                if (v == u[0]) {
                    g_kv["svc.music"] = u[1];
                    imported = true;
                    break;
                }
            }
        }
        fclose(f);
    }
    if (imported) {
        remove("svc_sel.txt");
        remove("music_sel.txt");
        Save();
    }
}

} // namespace

bool Init()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_ok)
        return true;
    if (EnsureDir("C:\\FixAi")) {
        g_dir = "C:\\FixAi";
    } else {
        char ap[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, ap)))
            return false;
        std::string d = std::string(ap) + "\\FixAi";
        if (!EnsureDir(d))
            return false;
        g_dir = d;
    }
    g_ok = true;
    Load();
    ImportLegacy();
    printf("config dir: %s\n", g_dir.c_str());
    fflush(stdout);
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    Save();
}

std::string Dir()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_dir;
}

std::string Get(const std::string& key, const std::string& def)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_kv.find(key);
    return it != g_kv.end() ? it->second : def;
}

bool GetBool(const std::string& key, bool def)
{
    std::string v = Get(key, "");
    if (v == "1" || v == "true" || v == "yes" || v == "on")
        return true;
    if (v == "0" || v == "false" || v == "no" || v == "off")
        return false;
    return def;
}

void Set(const std::string& key, const std::string& val)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_kv.find(key);
    if (it != g_kv.end() && it->second == val)
        return; // не менялось — диск не трогаем
    g_kv[key] = val;
    Save();
}

void SetBool(const std::string& key, bool v)
{
    Set(key, v ? "1" : "0");
}

} // namespace config
