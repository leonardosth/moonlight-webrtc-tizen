@echo off
echo ======================================================================
echo   Instalando Visual Studio 2022 C++ Build Tools (MSVC Toolchain)
echo ======================================================================
echo.
echo Uma janela de confirmacao (UAC) do Windows aparecera na sua tela.
echo Clique em "Sim" para autorizar a instalacao das ferramentas de C++.
echo.

set "INSTALLER=%LOCALAPPDATA%\Temp\WinGet\Microsoft.VisualStudio.2022.BuildTools.17.14.41\vs_BuildTools.exe"

if not exist "%INSTALLER%" (
    echo Baixando o instalador do Visual Studio Build Tools via winget...
    winget download --id Microsoft.VisualStudio.2022.BuildTools -d "%LOCALAPPDATA%\Temp\WinGet\Microsoft.VisualStudio.2022.BuildTools.17.14.41"
)

"%INSTALLER%" --passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ======================================================================
    echo   Instalacao do Visual Studio Build Tools concluida com sucesso!
    echo   Agora voce ja pode compilar o gateway executando build-gateway.bat
    echo ======================================================================
) else (
    echo.
    echo O instalador finalizou com codigo %ERRORLEVEL%.
    echo Se nao concedeu permissao de Administrador, clique com o botao direito
    echo neste arquivo e selecione "Executar como Administrador".
)

pause
