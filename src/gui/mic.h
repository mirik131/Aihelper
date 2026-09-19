#pragma once
#include <string>
#include <vector>

// Микрофон через WASAPI (без либ, чистый WinAPI).
// Start пишет фоном в voice_*.wav, Stop возвращает секунды (0 — не писалось).
// Поток один, всё остальное дёргаем с главного.
// Плюс стрим-режим для ears: непрерывный захват в кольцо пакетов (VAD режет
// фразы сам). Файловый и стрим режимы живут параллельно (shared mode).
namespace mic {

bool Start();
double Stop(); // секунды записи, файл уже закрыт
bool IsRecording();
const char* LastPath(); // куда писали в последний раз
void Shutdown();

bool StreamStart();   // непрерывный захват в кольцо
void StreamStop();
bool StreamRead(std::vector<uint8_t>& out); // один пакет, false если пусто
bool StreamFormat(std::vector<uint8_t>& fmtBlob); // WAVEFORMATEX blob
void StreamStats(unsigned long long& okPkts, unsigned long long& failed, unsigned long long& silentPkts); // здоровье захвата

} // namespace mic
