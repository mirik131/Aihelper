#pragma once
#include <d3d11.h>
#include <string>

// Сейчас играет (Windows SMTC): трек/артист/статус/позиция + обложка.
// Опрос висит в фоне (MTA-поток), снапшот забирается без блокировок.
// Без плеера — has=false, остров показывает AiHelper.
namespace media {

struct Info {
    bool has = false;     // есть сессия с названием
    bool playing = false; // реально играет (не пауза)
    std::string title;
    std::string artist;
    double pos = 0; // секунды НА МОМЕНТ tickMs (уже дотянуты от LastUpdatedTime)
    double dur = 0; // секунды, 0 если неизвестно
    unsigned long long tickMs = 0; // когда снято (GetTickCount64)
};

bool Init(ID3D11Device* dev);
void Shutdown();
bool Snapshot(Info& out);
void* Cover(); // SRV обложки текущего трека (ленивая сборка на потоке вызова), null если нет
void CoverSize(int& w, int& h);

} // namespace media
