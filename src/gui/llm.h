#pragma once
#include <string>

// Понимание через Groq (ключ в .env, модель константой в llm.cpp).
// История последних реплик хранится тут — отсюда контекст.
// AskAsync не ждёт, Poll забирает. Без сети/ключа — false, дальше фолбэк.

namespace llm {

bool AskAsync(const std::string& userText);
bool Poll(std::string& outAnswer); // true когда готово
bool Busy();
void Shutdown();

// быстрый путь для голосовых команд: 20b без истории, SSE-стрим,
// CMD исполняется по первой строке не дожидаясь конца. Состояние отдельно от чата.
bool AskFastAsync(const std::string& userText);
bool FastPoll(std::string& outAnswer); // true когда готово
bool FastBusy();

std::string ApiKey(); // ключ из .env, пусто если нет
std::string JsonField(const std::string& js, const std::string& field); // строковое поле + развёртка escapes

} // namespace llm
