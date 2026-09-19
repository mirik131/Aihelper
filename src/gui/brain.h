#pragma once
#include <string>
#include <vector>

// Мозг чата. Сейчас — локальная заглушка с честными правилами
// (привет/время/имя/помощь), дальше сюда встанут настоящие:
//   1. голос -> текст: Whisper.cpp + моделька ggml (качается отдельно, ~75МБ);
//   2. понимание: llama.cpp + маленькая GGUF-модель (скачивается отдельно, ГБ);
//   3. микрофон: WASAPI захват (кнопка уже есть, пока заглушка).
// Интерфейс не поменяется: Answer(текст) -> ответ.
namespace brain {

std::string Answer(const std::string& text);

// выполняет CMD от модели ("действие:параметр"), возвращает фразу для чата ("" если чужое)
std::string ExecCmd(const std::string& act, const std::string& prm);

// модель любит дефолтить музыку в youtube даже без просьбы — правим по фразе:
// youtube без упоминания ютуба в тексте -> auto (выбор юзера)
std::string FixMusicProvider(const std::string& heard, const std::string& provider);

// мгновенные команды без LLM: точные бытовые паттерны (0мс).
// Возвращает "действие:параметр" или "".
std::string InstantCmd(const std::string& heard);

// ключи для подсчёта повторов команды в фразе (длинные первыми!).
// Пусто = исполнить один раз (открыть/печатать/музыка/тоглы).
std::vector<std::string> InstantKeys(const std::string& act);

} // namespace brain
