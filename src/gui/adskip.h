#pragma once

// Автопропуск рекламы: фоном ищет кнопку «Пропустить/Skip Ad» в окнах
// браузеров через UI Automation и жмёт её. Работает даже если браузер не в фокусе.
namespace adskip {

void SetEnabled(bool on);
bool IsEnabled();
// NOTE: пропуск прероллов (кнопка) и интеграций (SponsorBlock) — один тогл.
void Shutdown();

} // namespace adskip
