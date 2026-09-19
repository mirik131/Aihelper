#pragma once
#include <d3d11.h>
#include <string>

// Карточка юзера снизу: аватарка + ник + бесконечность.
// Имя и картинка — из дискорда (без ключей, из его локальных файлов),
// нет дискорда — имя винды + картинка учётки. Без разницы где запущено.
// Телега так не умеет (там только через api_id с логином).

struct UserCard {
    std::string name = "User";
    std::string sub = "∞";
    void* avatarTex = nullptr; // SRV 1:1 или nullptr (тогда буква)
};

namespace user {

bool Init(ID3D11Device* device); // грузит всё сразу, зовёт main
const UserCard& Get();           // только отдаёт кэш
void Shutdown();

} // namespace user
