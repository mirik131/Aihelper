#pragma once
#include <string>

// Действия бота по командам. Всё через системный WinAPI, без либ:
// скрин — фотка экрана в файл, сайт — в браузере по умолчанию,
// приложения — поиском по меню Пуск (*.lnk, как сама винда),
// закрыть — окну по заголовку, музыка — мультимедиа-клавиши.
// Лайк тут нет специально: ему нужен контекст (ютуб? вк?) — спросим.

namespace actions {

bool screenshot(const char* path);   // весь экран в BMP, true если ок
bool openSite(const std::string& url); // https:// подставит сам
bool openApp(const std::string& name); // ищет в Пуске по имени, запускает
bool closeApp(const std::string& name); // WM_CLOSE окну по заголовку
void mediaPlayPause();               // вкл/пауза любому плееру
void mediaNext();                    // следующий трек
void mediaPrev();                    // предыдущий трек
std::string openFolder(const std::string& name); // известные папки + поиск по имени, путь или пусто
bool typeText(const std::string& text); // печать в текущий фокус (SendInput Unicode)
bool musicSearch(const std::string& provider, const std::string& query); // youtube/tiktok, прошлый сайт закрывает
bool mediaLike();                    // лайк (TikTok web: L), false если негде
std::string PlayerVol(int dir);      // громкость плеера (per-app, не мастер): +1/-1
bool openDota(bool clean);           // UmbrellaLoader.exe или чистый Steam (570)
void closeTab();                     // Ctrl+W foreground-вкладке
std::string SvcSel(const std::string& cat); // выбор сервиса категории music/work/fun
void SetLastVideoId(const std::string& id); // videoId для SponsorBlock
bool GetLastVideoIdFresh(std::string& out, unsigned long long maxAgeMs);
void SetSvcSel(const std::string& cat, const std::string& id); // + сохраняет в svc_sel.txt
std::string UrlEncode(const std::string& s); // UTF-8 -> %XX для запросов

} // namespace actions
