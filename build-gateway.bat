@echo off
setlocal
cd /d "%~dp0"

echo ======================================================================
echo   Compilando Moonlight WebRTC Gateway (moonlight_webrtc.exe)
echo ======================================================================
echo.

cmake --build build-vcpkg-release --config Release --target moonlight_webrtc

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ======================================================================
    echo   SUCESSO: moonlight_webrtc.exe compilado com sucesso!
    echo   Arquivo gerado em: build-vcpkg-release\Release\moonlight_webrtc.exe
    echo ======================================================================
) else (
    echo.
    echo ======================================================================
    echo   ERRO durante a compilacao. Verifique as mensagens de erro acima.
    echo ======================================================================
)

pause
