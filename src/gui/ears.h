#pragma once
#include <string>

// Автослушатель: пишет микро чанками, STT -> LLM -> выполнение. Всё в фоне.
// Тоглы из карточек AiHelper дёргают Set*. Ручной микрофон главнее (раунд скипается).
namespace ears {

void SetGeneral(bool on);   // мастер
void SetControlPc(bool on); // действия с ПК
void SetMusic(bool on);     // музыкальные команды
void SetRequireName(bool on); // выполнять только обращения по имени (защита от болтовни)

struct Snap {
    bool running = false;
    int phase = 0; // 0 выкл, 1 слушаю, 2 думаю, 3 делаю
    std::string heard;
    std::string result;
    bool maybeMuted = false; // ~30с гробовой тишины при работе
};

bool Snapshot(Snap& out);
void Shutdown();

} // namespace ears
