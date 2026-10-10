@echo off
setlocal enabledelayedexpansion

REM =========================================================
REM  Build CC2640R2 firmware (IAR) and collect artifacts to out\
REM
REM  Usage:  build_fw.bat [label]
REM          label default = fw       e.g. build_fw.bat v7
REM
REM  Env:    set IAR_PATH=<IAR install root>   (optional)
REM          e.g. set IAR_PATH=C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.0
REM
REM  Notes:
REM    * stack config = FlashROM_Library, app config = FlashROM_StackLibrary
REM    * -varfile carries the custom arg vars (SIMPLELINK_CORE_SDK_INSTALL_DIR, ...)
REM    * Output: out\BleEpd_fw_<label>.hex  (+ .out)
REM =========================================================

set "LABEL=%~1"
if "%LABEL%"=="" set "LABEL=fw"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "IARDIR=%ROOT%\5.14_advance_peripheral_uarttrans\tirtos\iar"
set "VARFILE=%IARDIR%\5.14_advance_peripheral_uarttrans.custom_argvars"
set "STACK=%IARDIR%\stack\cc2640r2lp_stack.ewp"
set "APP=%IARDIR%\app\cc2640r2lp_app.ewp"
set "STACK_CFG=FlashROM_Library"
set "APP_CFG=FlashROM_StackLibrary"
set "EXE_DIR=%IARDIR%\app\FlashROM_StackLibrary\Exe"
set "BASE=ble5_simple_peripheral_cc2640r2lp_app_FlashROM_StackLibrary"

set "IARBUILD="
if defined IAR_PATH if exist "%IAR_PATH%\common\bin\IarBuild.exe" set "IARBUILD=%IAR_PATH%\common\bin\IarBuild.exe"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 9.40"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 9.30"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 9.20"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 8.50"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 8.40"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 8.32"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 8.30"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 8.20"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 8.11"
call :try "C:\Program Files\IAR Systems\Embedded Workbench 8.10"
call :try "C:\Program Files (x86)\IAR Systems\Embedded Workbench 9.40"
call :try "C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.50"
call :try "C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.30"
call :try "C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.20"
call :try "C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.11"
call :try "C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.10"
call :try "C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.0"

if not defined IARBUILD goto :no_iar
echo [INFO] IarBuild: %IARBUILD%

echo [1/2] Building stack (%STACK_CFG%) ...
"%IARBUILD%" "%STACK%" -build %STACK_CFG% -varfile "%VARFILE%" -log warnings
if errorlevel 1 goto :fail

echo [2/2] Building app (%APP_CFG%) ...
"%IARBUILD%" "%APP%" -build %APP_CFG% -varfile "%VARFILE%" -log warnings
if errorlevel 1 goto :fail

if not exist "%ROOT%\out" mkdir "%ROOT%\out"
if not exist "%EXE_DIR%\%BASE%.hex" goto :no_artifact
copy /Y "%EXE_DIR%\%BASE%.hex" "%ROOT%\out\BleEpd_fw_%LABEL%.hex" >nul
copy /Y "%EXE_DIR%\%BASE%.out" "%ROOT%\out\BleEpd_fw_%LABEL%.out" >nul
echo [OK] out\BleEpd_fw_%LABEL%.hex
endlocal
exit /b 0

:try
if not defined IARBUILD if exist "%~1\common\bin\IarBuild.exe" set "IARBUILD=%~1\common\bin\IarBuild.exe"
exit /b 0

:no_iar
echo [ERROR] IarBuild.exe not found. Point IAR_PATH at your IAR install root, e.g.
echo         set IAR_PATH=C:\Program Files ^(x86^)\IAR Systems\Embedded Workbench 8.0
exit /b 1

:no_artifact
echo [ERROR] expected artifact not found: %EXE_DIR%\%BASE%.hex
exit /b 1

:fail
echo [ERROR] firmware build failed
exit /b 1
