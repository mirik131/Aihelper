#pragma once
#include <string>

// Голос -> текст через Whisper (whisper.dll + models/ggml-tiny.bin).
// DLL грузим руками — без неё программа всё равно стартует.
// Русский выставлен явно. Долгая работа (модель+расчёт) — в фоне.

namespace stt {

bool TranscribeFile(const char* wavPath); // старт фоном, false если занято/нечем
bool Poll(std::string& outText);          // true когда готово (текст может быть пустым)
bool Busy();
void Shutdown();

} // namespace stt
