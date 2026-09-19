#pragma once
#include <string>

// Конфиг на диске: C:\FixAi\config.ini (нет прав — %LOCALAPPDATA%\FixAi).
// Что храним: тоглы (general/pc/mus/name/smooth/island), выборы сервисов.
// Пишем только по изменению. Старьё (svc_sel.txt/music_sel.txt) импортируем разок.
namespace config {

bool Init();
void Shutdown();
std::string Dir(); // куда легло
std::string Get(const std::string& key, const std::string& def = "");
bool GetBool(const std::string& key, bool def);
void Set(const std::string& key, const std::string& val);
void SetBool(const std::string& key, bool v);

} // namespace config
