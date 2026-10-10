@echo off
setlocal

REM =========================================================
REM  Build Android app (Gradle) and collect APK to out\
REM
REM  Usage:  build_app.bat [label]
REM          label default = debug    e.g. build_app.bat v33
REM
REM  Output: out\BleEpd_app_<label>.apk
REM =========================================================

set "LABEL=%~1"
if "%LABEL%"=="" set "LABEL=debug"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

REM ---- pick a usable JAVA_HOME ----
REM some setups set JAVA_HOME to the \bin subfolder by mistake; normalize it
if defined JAVA_HOME if not exist "%JAVA_HOME%\bin\java.exe" if exist "%JAVA_HOME%\java.exe" for %%I in ("%JAVA_HOME%\..") do set "JAVA_HOME=%%~fI"
if not exist "%JAVA_HOME%\bin\java.exe" if exist "C:\Program Files\Android\Android Studio\jbr\bin\java.exe" set "JAVA_HOME=C:\Program Files\Android\Android Studio\jbr"
if not exist "%JAVA_HOME%\bin\java.exe" if exist "C:\Program Files\Eclipse Adoptium\jdk-25.0.1.8-hotspot\bin\java.exe" set "JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-25.0.1.8-hotspot"
if not exist "%JAVA_HOME%\bin\java.exe" goto :nojava
echo [INFO] JAVA_HOME: %JAVA_HOME%

cd /d "%ROOT%\AndroidApp"
call gradlew.bat assembleDebug
if errorlevel 1 goto :fail

set "APK=%ROOT%\AndroidApp\app\build\outputs\apk\debug\app-debug.apk"
if not exist "%APK%" goto :noapk
if not exist "%ROOT%\out" mkdir "%ROOT%\out"
copy /Y "%APK%" "%ROOT%\out\BleEpd_app_%LABEL%.apk" >nul
echo [OK] out\BleEpd_app_%LABEL%.apk
endlocal
exit /b 0

:nojava
echo [ERROR] no usable JDK. Set JAVA_HOME to a JDK root, not the \bin subfolder.
exit /b 1

:noapk
echo [ERROR] apk not found: %APK%
exit /b 1

:fail
echo [ERROR] android build failed
exit /b 1
