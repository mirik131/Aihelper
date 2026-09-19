cmake_minimum_required(VERSION 3.16)

project(helper_bot VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(helper_bot
    src/main.cpp
    src/gui/gui.cpp
    src/gui/blurbg.cpp
    src/gui/select.cpp
    src/gui/photo.cpp
    src/gui/user.cpp
    src/gui/discord.cpp
    src/gui/brain.cpp
    src/gui/mic.cpp
    src/gui/actions.cpp
    src/gui/stt.cpp
    src/gui/svg.cpp
    src/gui/ears.cpp
    src/gui/config.cpp
    src/gui/adskip.cpp
    src/gui/helpers.cpp
    src/gui/llm.cpp
    src/gui/island.cpp
    src/gui/textfade.cpp
    src/gui/media.cpp
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_demo.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/imgui_widgets.cpp
    thirdparty/imgui/backends/imgui_impl_win32.cpp
    thirdparty/imgui/backends/imgui_impl_dx11.cpp
)

target_include_directories(helper_bot PRIVATE
    src
    thirdparty/imgui
    thirdparty/imgui/backends
)

# media.cpp ест C++/WinRT — ему нужен /await (остальные файлы не трогаем)
if (MSVC)
    set_source_files_properties(src/gui/media.cpp PROPERTIES COMPILE_OPTIONS "/await")
endif()

# для DX11 под виндой
if (WIN32)
    target_link_libraries(helper_bot PRIVATE d3d11 d3dcompiler dxgi dwmapi windowscodecs urlmon winhttp runtimeobject d2d1 uiautomationcore)
    target_compile_options(helper_bot PRIVATE /utf-8)
endif()
