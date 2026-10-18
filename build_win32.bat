@echo off
setlocal
set OUT_DIR=Debug
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
cl /nologo /std:c++17 /W4 /EHsc /MD /utf-8 /Ithird_party\imgui /Ithird_party\imgui\backends /Ithird_party\imgui\misc\freetype /Ithird_party\freetype\include main.cpp panorama_ui.cpp embedded_fonts.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_win32.cpp third_party\imgui\backends\imgui_impl_dx11.cpp third_party\imgui\misc\freetype\imgui_freetype.cpp /Fe%OUT_DIR%\game_settings.exe /Fo%OUT_DIR%\ /link /LIBPATH:third_party\freetype\lib\x64 freetype.lib d3d11.lib d3dcompiler.lib dxgi.lib ole32.lib windowscodecs.lib user32.lib
exit /b %errorlevel%
