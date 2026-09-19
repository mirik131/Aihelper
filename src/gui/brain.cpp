#include "gui/brain.h"
#include "gui/actions.h"
#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdio>

namespace brain {
namespace {

// нижний регистр для ASCII + кириллицы (Ё отдельно)
std::string lowerRu(const std::string& s)
{
    std::string o = s;
    for (size_t i = 0; i < o.size();) {
        unsigned char c = (unsigned char)o[i];
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z')
                o[i] = (char)(c + 32);
            i++;
        } else if (c == 0xD0 && i + 1 < o.size()) {
            unsigned char d = (unsigned char)o[i + 1];
            if (d >= 0x90 && d <= 0x9F) { // А-Я
                o[i + 1] = (char)(d + 32);
            } else if (d == 0x81) { // Ё -> ё
                o[i] = (char)0xD1;
                o[i + 1] = (char)0x91;
            } else if (d >= 0xA0 && d <= 0xAF) { // Р-Я -> р-я
                o[i] = (char)0xD1;
                o[i + 1] = (char)(d - 32);
            }
            i += 2;
        } else if (c == 0xD1 && i + 1 < o.size()) {
            i += 2; // строчные уже
        } else {
            i++;
        }
    }
    return o;
}

// "телеграм" -> ayugram и другие соответствия (точное совпадение, без угадайки)
std::string AliasApp(const std::string& name)
{
    size_t a = 0;
    while (a < name.size() && (unsigned char)name[a] <= ' ')
        a++;
    size_t b = name.size();
    while (b > a && (unsigned char)name[b - 1] <= ' ')
        b--;
    std::string l = lowerRu(name.substr(a, b - a));
    // режем URL-шелуху: «youtube.com» -> «youtube» (модель любит приписывать .com)
    if (l.rfind("www.", 0) == 0)
        l = l.substr(4);
    {
        size_t dot = l.find('.');
        if (dot != std::string::npos) {
            std::string tail = l.substr(dot + 1);
            size_t slash = tail.find('/');
            if (slash != std::string::npos)
                tail = tail.substr(0, slash);
            if (tail == "com" || tail == "ru" || tail == "org" || tail == "net" ||
                tail == "io" || tail == "su" || tail == "by" || tail == "ua")
                l = l.substr(0, dot);
        }
    }
    static const char* pairs[][2] = {
        { "телеграм", "ayugram" }, { "телега", "ayugram" }, { "тг", "ayugram" },
        { "вскод", "Code" }, { "вс код", "Code" }, { "vscode", "Code" }, { "визуалка", "Code" },
        { "опенкод", "OpenCode" }, { "opencode", "OpenCode" },
        { "блокнот", "notepad" }, { "калькулятор", "calc" },
        { "проводник", "explorer" }, { "хром", "chrome" }, { "браузер", "chrome" },
        { "дискорд", "Discord" }, { "стим", "steam" }, { "спотифай", "Spotify" },
        { "ютуб", "YouTube" }, { "youtube", "YouTube" },
        { "тикток", "TikTok" }, { "tiktok", "TikTok" },
        { "spotify", "Spotify" },
        { "саундклауд", "SoundCloud" }, { "soundcloud", "SoundCloud" },
        { "яндекс музыка", "Яндекс Музыка" },
        { "твич", "Twitch" }, { "twitch", "Twitch" },
        { "рутуб", "Rutube" }, { "rutube", "Rutube" },
        { "вк", "VK" }, { "vk", "VK" },
        { "дота", "dota" }, { "доту", "dota" }, { "доты", "dota" }, { "dota", "dota" },
    };
    for (auto& p : pairs) {
        if (l == p[0])
            return p[1];
    }
    return name.substr(a, b - a);
}

bool has(const std::string& s, const char* sub)
{
    return s.find(sub) != std::string::npos;
}

std::string nowTime()
{
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[16];
#ifdef _WIN32
    std::tm tm{};
    localtime_s(&tm, &t);
#else
    std::tm tm = *std::localtime(&t);
#endif
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string nowDate()
{
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[16];
#ifdef _WIN32
    std::tm tm{};
    localtime_s(&tm, &t);
#else
    std::tm tm = *std::localtime(&t);
#endif
    snprintf(buf, sizeof(buf), "%02d.%02d.%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    return buf;
}

} // namespace

std::string Answer(const std::string& text)
{
    std::string t = lowerRu(text);

    // --- команды железу (до болтовни) ---
    auto restAfter = [&](const char* kw) -> std::string {
        size_t p = t.find(kw);
        if (p == std::string::npos)
            return "";
        std::string r = t.substr(p + strlen(kw));
        size_t a = 0;
        while (a < r.size() && (unsigned char)r[a] <= ' ')
            a++;
        size_t b = r.size();
        while (b > a && (unsigned char)r[b - 1] <= ' ')
            b--;
        return r.substr(a, b - a);
    };
    if (has(t, "скрин") || has(t, "снимок") || has(t, "screenshot")) {
        if (actions::screenshot("screenshot.bmp"))
            return "Готово, скрин в screenshot.bmp рядом с программой.";
        return "Не смог снять экран.";
    }
    if (has(t, "следующий трек") || has(t, "след трек") || has(t, "next track")) {
        actions::mediaNext();
        return "Включил следующий трек.";
    }
    if (has(t, "предыдущий трек") || has(t, "пред трек") || has(t, "prev track")) {
        actions::mediaPrev();
        return "Включил предыдущий трек.";
    }
    if (has(t, "музык") || has(t, "music") || has(t, "пауза") || has(t, "продолжи")) {
        actions::mediaPlayPause();
        return "Жму play/pause плееру.";
    }
    if (has(t, "лайк") || has(t, "like"))
        return "Лайк где — ютуб, вк, спотифай? Скажи, прикручу под него.";
    {
        std::string r = restAfter("закрой");
        if (r.empty())
            r = restAfter("закрыть");
        if (!r.empty())
            return actions::closeApp(r) ? ("Закрываю «" + r + "».") : ("Не нашёл окно «" + r + "».");
    }
    {
        std::string r = restAfter("открой");
        if (r.empty())
            r = restAfter("открыть");
        if (!r.empty()) {
            if (r == "ютуб" || r == "youtube")
                r = "youtube.com";
            else if (r == "вк" || r == "vk")
                r = "vk.com";
            else if (r == "музыку" || r == "музыка" || r == "spotify" || r == "спотифай") {
                actions::mediaPlayPause();
                return "Включаю музыку.";
            }
            if (r.find('.') != std::string::npos || r.find("http") != std::string::npos)
                return actions::openSite(r) ? ("Открываю " + r + ".") : ("Не смог открыть " + r + ".");
            return actions::openApp(r) ? ("Открываю «" + r + "».") : ("Не нашёл «" + r + "» в меню Пуск.");
        }
    }
    if (has(t, "привет") || has(t, "здравствуй") || has(t, "добрый день") || has(t, "добрый вечер"))
        return "Привет! Я FixAi, пока учусь. Спроси время или дату — это я уже умею.";
    if (has(t, "время") || has(t, "который час"))
        return "Сейчас " + nowTime() + ".";
    if (has(t, "дата") || has(t, "число") || has(t, "какой сегодня день"))
        return "Сегодня " + nowDate() + ".";
    if (has(t, "как тебя") || has(t, "кто ты") || has(t, "твоё имя") || has(t, "твое имя") || has(t, "зовут"))
        return "Я FixAi — помощник. Пока отвечаю из локальной заглушки, нейронку прикрутим позже.";
    if (has(t, "что умеешь") || has(t, "помощь") || has(t, "help") || has(t, "команды") || has(t, "умеешь"))
        return "Пока немного: время, дата, простые ответы. Скоро: голос и нормальные ответы.";
    if (has(t, "спасибо"))
        return "Пожалуйста!";
    if (has(t, "пока"))
        return "До связи!";
    return "Понял тебя. Пока отвечаю просто — настоящий мозг в пути.";
}

// выполняет CMD от модели: "действие:параметр", возвращает фразу для чата ("" если чужое)
std::string ExecCmd(const std::string& act, const std::string& prm)
{
    if (act == "openSite")
        return actions::openSite(prm) ? ("Открываю " + prm + ".") : ("Не смог открыть " + prm + ".");
    if (act == "openApp") {
        std::string low = lowerRu(prm);
        if (low.find("dota") != std::string::npos || low.find("дота") != std::string::npos ||
            low.find("доту") != std::string::npos || low.find("доты") != std::string::npos) {
            bool clean = (low.find("без чит") != std::string::npos ||
                          low.find("без говна") != std::string::npos ||
                          low.find("чист") != std::string::npos);
            if (actions::openDota(clean))
                return clean ? "Запускаю чистую Доту." : "Запускаю Доту.";
            return "Не нашёл UmbrellaLoader.";
        }
        std::string app = AliasApp(prm);
        return actions::openApp(app) ? ("Открываю «" + prm + "».") : ("Не нашёл «" + prm + "» в меню Пуск.");
    }
    if (act == "closeApp") {
        std::string app = AliasApp(prm);
        return actions::closeApp(app) ? ("Закрываю «" + prm + "».") : ("Не нашёл запущенную «" + prm + "».");
    }
    if (act == "openFolder") {
        std::string p = actions::openFolder(prm);
        return p.empty() ? ("Не нашёл папку «" + prm + "».") : ("Открываю «" + p + "».");
    }
    if (act == "typeText")
        return actions::typeText(prm) ? "Напечатал." : "Не смог напечатать.";
    if (act == "musicSearch") {
        // параметр "где:что" (двоеточие первое — разделитель)
        size_t c = prm.find(':');
        std::string pv = (c == std::string::npos) ? "youtube" : prm.substr(0, c);
        std::string q = (c == std::string::npos) ? prm : prm.substr(c + 1);
        if (actions::musicSearch(pv, q))
            return q.empty() ? "Включаю." : ("Включаю «" + q + "».");
        return "Не смог открыть музыку.";
    }
    if (act == "mediaLike")
        return actions::mediaLike() ? "Лайкнул." : "Лайки ставлю только в ТикТоке — открой его.";
    if (act == "volUp") {
        std::string w = actions::PlayerVol(+1);
        return w.empty() ? "Ничего не играет." : ("Громче (" + w + ").");
    }
    if (act == "volDown") {
        std::string w = actions::PlayerVol(-1);
        return w.empty() ? "Ничего не играет." : ("Тише (" + w + ").");
    }
    if (act == "closeTab") {
        actions::closeTab();
        return "Закрыл вкладку.";
    }
    if (act == "openSvc") {
        std::string u;
        if (prm == "work")
            u = actions::SvcSel("work");
        else if (prm == "fun")
            u = actions::SvcSel("fun");
        else
            u = actions::SvcSel("music");
        return actions::openSite(u) ? "Открываю." : "Не смог открыть.";
    }
    if (act == "screenshot")
        return actions::screenshot("screenshot.bmp") ? "Готово, скрин в screenshot.bmp рядом с окном."
                                                     : "Не смог сделать скриншот.";
    if (act == "mediaPlay") {
        actions::mediaPlayPause();
        return "Готово.";
    }
    if (act == "mediaNext") {
        actions::mediaNext();
        return "Следующий трек.";
    }
    if (act == "mediaPrev") {
        actions::mediaPrev();
        return "Предыдущий трек.";
    }
    return "";
}

std::string FixMusicProvider(const std::string& heard, const std::string& provider)
{
    if (provider != "youtube")
        return provider; // явный выбор модели не трогаем
    std::string l = lowerRu(heard);
    if (l.find("ютуб") != std::string::npos || l.find("youtube") != std::string::npos ||
        l.find("you tube") != std::string::npos || l.find("ютюб") != std::string::npos)
        return provider;
    return "auto";
}

bool StartsWith(const std::string& s, const char* p)
{
    size_t n = strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

bool Contains(const std::string& s, const char* p)
{
    return s.find(p) != std::string::npos;
}

std::string TrimLc(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (unsigned char)s[a] <= ' ')
        a++;
    size_t b = s.size();
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        b--;
    return s.substr(a, b - a);
}

// выкинуть слово целиком (по границам), схлопнуть пробелы
void EraseWord(std::string& t, const char* w)
{
    size_t n = strlen(w);
    size_t p = 0;
    while ((p = t.find(w, p)) != std::string::npos) {
        bool lb = (p == 0) || (unsigned char)t[p - 1] <= ' ';
        size_t e = p + n;
        bool rb = (e >= t.size()) || (unsigned char)t[e] <= ' ';
        if (lb && rb)
            t.erase(p, n);
        else
            p += n;
    }
    std::string o;
    bool sp = true;
    for (char c : t) {
        if ((unsigned char)c <= ' ') {
            if (!sp) {
                o += ' ';
                sp = true;
            }
        } else {
            o += c;
            sp = false;
        }
    }
    if (!o.empty() && o.back() == ' ')
        o.pop_back();
    t = o;
}

// мгновенные команды без LLM: точные бытовые паттерны (0мс).
// Возвращает "действие:параметр" или "". lowerRu длину байт не меняет,
// поэтому позиции из lowered годятся для нарезки оригинала (напиши).
std::string InstantCmd(const std::string& heard)
{
    std::string o = TrimLc(heard);
    if (o.size() < 2)
        return "";
    std::string t = lowerRu(o);

    // «напиши ...» — раньше всех: нужен ОРИГИНАЛЬНЫЙ регистр
    {
        size_t p = t.find("напиши ");
        if (p != std::string::npos) {
            std::string r = TrimLc(o.substr(p + 13));
            if (!r.empty())
                return "typeText:" + r;
        }
    }
    // обращение и вежливость выкидываем (мешают матчингу)
    const char* drop[] = { "фикс", "фикса", "фиксу", "фиксом", "фиксе", "фикси", "fix",
                           "пожалуйста", "пожалуста", "плиз", nullptr };
    for (int i = 0; drop[i]; i++)
        EraseWord(t, drop[i]);
    if (t.size() < 2)
        return "";

    if (Contains(t, "закрой вкладку"))
        return "closeTab:";
    const char* closeV[] = { "закрой ", "закрыть ", "крой ", nullptr };
    for (int i = 0; closeV[i]; i++) {
        if (StartsWith(t, closeV[i])) {
            std::string r = TrimLc(t.substr(strlen(closeV[i])));
            if (r.empty())
                break;
            // «закрой музыку» = стоп, а не окно с таким именем
            if (Contains(r, "музык") || Contains(r, "трек") || Contains(r, "песн") || Contains(r, "радио"))
                return "mediaPlay:";
            return "closeApp:" + r;
        }
    }
    const char* openV[] = { "открой ", "открыть ", "покажи ", "показать ", "запусти ", "запустить ", nullptr };
    for (int i = 0; openV[i]; i++) {
        if (StartsWith(t, openV[i])) {
            std::string r = TrimLc(t.substr(strlen(openV[i])));
            if (r.empty())
                break;
            if (r.find('.') != std::string::npos || r.find("http") != std::string::npos)
                return "openSite:" + r;
            if (r == "ютуб" || r == "youtube")
                return "openSite:youtube.com";
            if (r == "тикток" || r == "tiktok")
                return "openSite:tiktok.com";
            if (r == "вк" || r == "vk")
                return "openSite:vk.com";
            if (r == "спотифай" || r == "spotify")
                return "openSite:open.spotify.com";
            // популярные сайты (а то «открой kwork» искало программу в Пуске)
            static const char* sites[][2] = {
                { "кворк", "kwork.ru" }, { "kwork", "kwork.ru" },
                { "авито", "avito.ru" }, { "avito", "avito.ru" },
                { "озон", "ozon.ru" }, { "вайлдберриз", "wildberries.ru" },
                { "гугл", "google.com" }, { "google", "google.com" },
                { "яндекс", "ya.ru" }, { "yandex", "ya.ru" },
                { "дзен", "dzen.ru" },
                { "госуслуги", "gosuslugi.ru" },
                { "твич", "twitch.tv" }, { "twitch", "twitch.tv" },
                { "стим", "store.steampowered.com" }, { "steam", "store.steampowered.com" },
                { "гитхаб", "github.com" }, { "github", "github.com" },
                { "рутуб", "rutube.ru" }, { "rutube", "rutube.ru" },
                { "кинопоиск", "kinopoisk.ru" },
            };
            for (auto& s : sites) {
                if (r == s[0])
                    return std::string("openSite:") + s[1];
            }
            if (r == "музыку" || r == "музыка")
                return "mediaPlay:";
            if (StartsWith(r, "папку ") || StartsWith(r, "папка "))
                r = TrimLc(r.substr(r.find(' ') + 1));
            if (r == "рабочий стол" || r == "документы" || r == "загрузки" || r == "музыка" ||
                r == "картинки" || r == "фото" || r == "видео" || StartsWith(r, "диск "))
                return "openFolder:" + r;
            return "openApp:" + r;
        }
    }
    const char* playV[] = { "включи ", "включить ", "вруби ", "поставь ", nullptr };
    for (int i = 0; playV[i]; i++) {
        if (StartsWith(t, playV[i])) {
            std::string r = TrimLc(t.substr(strlen(playV[i])));
            if (r.empty())
                break;
            if (Contains(r, "музык") || Contains(r, "трек") || Contains(r, "песн") ||
                Contains(r, "музон") || Contains(r, "радио")) {
                // конкретный сервис — пусть решает LLM (откроет, а не засёрчит)
                if (Contains(r, "ютуб") || Contains(r, "youtube") || Contains(r, "спотифай") ||
                    Contains(r, "spotify") || Contains(r, "яндекс музык") || Contains(r, "саундклауд") ||
                    Contains(r, "soundcloud"))
                    break;
                std::string pv = "auto";
                if (Contains(r, "тикток"))
                    pv = "tiktok";
                std::string q = r;
                const char* junk[] = { "музыку", "музыка", "трек", "песню", "песня", "музон",
                                       "радио", "включи", "включить", "вруби", "поставь",
                                       "из тиктока", "тиктока", "тикток", "мне", nullptr };
                for (int j = 0; junk[j]; j++)
                    EraseWord(q, junk[j]);
                return "musicSearch:" + pv + ":" + q;
            }
            // «включи <название>» — сразу ютуб-автоплей без LLM
            // (лайк обрабатывается ниже точным правилом — пропускаем)
            if (Contains(r, "лайк") || Contains(r, "like"))
                break;
            {
                std::string q = r;
                const char* svc[] = { "ютуб", "youtube", "тикток", "tiktok", "спотифай", "spotify",
                                      "яндекс музыка", "саундклауд", "soundcloud", "музыку", "музыка",
                                      "включи", "включить", "вруби", "поставь", "мне", "пожалуйста", nullptr };
                for (int j = 0; svc[j]; j++)
                    EraseWord(q, svc[j]);
                if (!q.empty())
                    return "musicSearch:youtube:" + q;
            }
        }
    }
    if (t == "пауза" || Contains(t, "поставь на паузу") || Contains(t, "останови музыку") ||
        Contains(t, "выключи музыку") || Contains(t, "останови трек"))
        return "mediaPlay:";
    if (StartsWith(t, "продолжи") || t == "играй" || Contains(t, "продолжи музыку"))
        return "mediaPlay:";
    if (Contains(t, "следующ") || Contains(t, "дальше") || Contains(t, "следующий трек") ||
        Contains(t, "некст") || Contains(t, "переключи") || Contains(t, "переключить") ||
        Contains(t, "переключай") || Contains(t, "перелистни") || Contains(t, "скипни") ||
        t == "скип" || Contains(t, "пропусти трек"))
        return "mediaNext:";
    if (Contains(t, "предыдущ") || t == "назад" || Contains(t, "прошлый трек") ||
        Contains(t, "предыдущий трек") || Contains(t, "верни трек") || Contains(t, "верни назад"))
        return "mediaPrev:";
    if (Contains(t, "громче") || Contains(t, "прибавь") || Contains(t, "погромче") ||
        Contains(t, "громкость больше") || Contains(t, "громкость выше"))
        return "volUp:";
    if (Contains(t, "потише") || Contains(t, "тише") || Contains(t, "убавь") || t == "тихо" ||
        Contains(t, "громкость меньше") || Contains(t, "громкость ниже"))
        return "volDown:";
    if (Contains(t, "лайк") || Contains(t, "like"))
        return "mediaLike:";
    return "";
}

// ключи для подсчёта повторов команды в фразе (длинные первыми!).
// Пусто = исполнить один раз (открыть/печатать/музыка/тоглы).
std::vector<std::string> InstantKeys(const std::string& act)
{
    if (act == "mediaNext")
        return { "следующий трек", "переключить", "переключай", "перелистни", "пропусти трек",
                 "следующ", "дальше", "некст", "переключи", "скипни", "скип" };
    if (act == "mediaPrev")
        return { "предыдущий трек", "предыдущ", "прошлый трек", "верни трек", "верни назад", "назад" };
    if (act == "volUp")
        return { "громкость больше", "громкость выше", "погромче", "прибавь", "громче" };
    if (act == "volDown")
        return { "громкость меньше", "громкость ниже", "потише", "убавь", "тише", "тихо" };
    if (act == "mediaLike")
        return { "лайк", "like" };
    return {};
}

} // namespace brain
