@echo off
REM ---------------------------------------------------------------
REM Gravacao manual do Pro Micro, sem a IDE no caminho.
REM
REM Uso:  gravar.bat COM7
REM       (troque COM7 pela porta que o bootloader assume)
REM
REM Sequencia:
REM   1. Compile na IDE (Ctrl+R) para gerar o .hex atualizado.
REM   2. Rode este .bat passando a porta do bootloader.
REM   3. Assim que aparecer "tentando", curto-circuite RST no GND
REM      duas vezes rapido.
REM
REM O script tenta por ~30 segundos, entao nao ha pressa de reflexo:
REM ele pega a janela do bootloader assim que ela abrir.
REM ---------------------------------------------------------------
setlocal
if "%~1"=="" (
  echo Falta a porta. Exemplo:  gravar.bat COM7
  pause
  exit /b 1
)
set "PORT=%~1"
set "AV=C:\Users\mathe\AppData\Local\Arduino15\packages\arduino\tools\avrdude\8.0.0-arduino1"
set "HEX=C:\Users\mathe\AppData\Local\arduino\sketches\CABF01AAAC128AD30A1D10B3C8E7A3AB\UPS.ino.hex"

if not exist "%HEX%" (
  echo .hex nao encontrado:
  echo   %HEX%
  echo Compile na IDE primeiro ^(Ctrl+R^).
  pause
  exit /b 1
)

echo.
echo === Curto-circuite RST no GND duas vezes, rapido ===
echo tentando %PORT% ...
echo.

for /L %%i in (1,1,30) do (
  "%AV%\bin\avrdude.exe" "-C%AV%\etc\avrdude.conf" -V -patmega32u4 -cavr109 -P%PORT% -b57600 -D "-Uflash:w:%HEX%:i" >nul 2>&1
  if not errorlevel 1 goto ok
  <nul set /p "=."
  timeout /t 1 /nobreak >nul
)
echo.
echo FALHOU. Rodando uma ultima vez com log completo:
"%AV%\bin\avrdude.exe" "-C%AV%\etc\avrdude.conf" -v -V -patmega32u4 -cavr109 -P%PORT% -b57600 -D "-Uflash:w:%HEX%:i"
pause
exit /b 1

:ok
echo.
echo === GRAVADO ===
pause
