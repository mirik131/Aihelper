@echo off
chcp 65001 >nul
REM сборка helper_bot (ImGui, Win32+DX11)

REM подхватываем cmake из winget-установки если PATH ещё старый
set "PATH=C:\Program Files\CMake\bin;%PATH%"

where cmake >nul 2>nul
if %errorlevel% neq 0 (
    echo cmake не найден. Поставь: winget install -e --id Kitware.CMake
    pause
    exit /b 1
)

REM проверяем MSVC (без него cmake не соберёт)
where cl >nul 2>nul
if %errorlevel% neq 0 (
    if not exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools" (
        echo Нужна нагрузка C++ для Visual Studio:
        echo 1. Открой "Visual Studio Installer"
        echo 2. Изменить -^> "Разработка классических приложений на C++" -^> Изменить
        echo    (там подтянется MSVC + Windows SDK 10/11 + CMake)
        echo 3. Потом снова запусти build.bat или открой helper_bot.sln
        pause
        exit /b 1
    )
)

echo Собираю через CMake...
cmake -S . -B build
if %errorlevel% neq 0 pause & exit /b 1
cmake --build build --config Release
echo Готово: build\Release\helper_bot.exe
pause
