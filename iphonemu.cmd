@echo off
rem iphonemu: provisional command-line shortcut for the iPhone 4S machine (Windows).
rem
rem   iphonemu                    boot the default unit in an SDL window
rem   iphonemu boot [UNIT] [...]  boot UNIT; extra options go to `ios_prep.py device boot`
rem                               (--no-usb, --cpus 1, --display none, --detach, -- QEMU ARGS)
rem   iphonemu list               the units and their state
rem   iphonemu home|power|wake    press a button on the running unit
rem   iphonemu shot FILE.png      screenshot of the panel
rem   iphonemu poweroff           iOS's own power-off (QEMU exits by itself, the store flushed)
rem   iphonemu stop               QMP quit (a power cut for the guest; prefer poweroff)
rem   iphonemu input ...          any tools/dev/n94_input.py command (tap X Y, swipe ..., key ...)
rem
rem Settings (environment variables, all optional):
rem   IPHONEMU_DEVICES  folder holding the units (default E:\iphonemu-gl\devices if it exists,
rem                     else devices\ beside the repository, as ios_prep.py does)
rem   IPHONEMU_UNIT     the default unit (default gl5)
rem   IPHONEMU_QMP      the running unit's QMP port (default 4444)
rem
rem It wraps ios_prep.py's `device` commands and n94_input.py; nothing here is new behaviour.
setlocal EnableExtensions

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
for %%I in ("%REPO%\..") do set "WORKSPACE=%%~fI"
set "PY=%WORKSPACE%\.venv\Scripts\python.exe"
set "PREP=%REPO%\tools\ios-prep\ios_prep.py"
set "INPUT=%REPO%\tools\dev\n94_input.py"

if not exist "%PY%" (
    echo iphonemu: Python of the project not found at %PY%
    echo           create it with: python -m venv "%WORKSPACE%\.venv" ^&^& "%PY%" -m pip install cryptography pillow
    exit /b 1
)
if not exist "%REPO%\build\qemu-system-arm.exe" (
    echo iphonemu: %REPO%\build\qemu-system-arm.exe not built yet ^(toolchain\build-qemu.sh build^)
    exit /b 1
)

if not defined IPHONEMU_DEVICES if exist "E:\iphonemu-gl\devices" set "IPHONEMU_DEVICES=E:\iphonemu-gl\devices"
if not defined IPHONEMU_UNIT set "IPHONEMU_UNIT=gl5"
if not defined IPHONEMU_QMP set "IPHONEMU_QMP=4444"

set "CMD=%~1"
if "%CMD%"=="" goto boot_default
if /i "%CMD%"=="help" goto help
if /i "%CMD%"=="-h" goto help
if /i "%CMD%"=="--help" goto help
if /i "%CMD%"=="list" goto list
if /i "%CMD%"=="boot" goto boot
if /i "%CMD%"=="stop" goto stop
if /i "%CMD%"=="quit" goto stop
if /i "%CMD%"=="poweroff" goto input_one
if /i "%CMD%"=="home" goto key
if /i "%CMD%"=="power" goto key
if /i "%CMD%"=="wake" goto input_one
if /i "%CMD%"=="shot" goto shot
if /i "%CMD%"=="input" goto input
echo iphonemu: unknown command "%CMD%" ^(iphonemu help^)
exit /b 2

:help
setlocal EnableDelayedExpansion
for /f "usebackq delims=" %%L in (`findstr /b /c:"rem" "%~f0"`) do (
    set "LINE=%%L"
    echo(!LINE:~4!
)
exit /b 0

:boot_default
"%PY%" -I "%PREP%" device boot "%IPHONEMU_UNIT%" --qmp %IPHONEMU_QMP%
exit /b %ERRORLEVEL%

:boot
shift
set "UNIT=%~1"
if "%UNIT%"=="" set "UNIT=%IPHONEMU_UNIT%"
if "%UNIT:~0,1%"=="-" (
    set "UNIT=%IPHONEMU_UNIT%"
) else (
    shift
)
set "REST="
:boot_args
if "%~1"=="" goto boot_run
set "REST=%REST% %1"
shift
goto boot_args
:boot_run
"%PY%" -I "%PREP%" device boot "%UNIT%" --qmp %IPHONEMU_QMP% %REST%
exit /b %ERRORLEVEL%

:list
"%PY%" -I "%PREP%" device list
exit /b %ERRORLEVEL%

:stop
"%PY%" -I "%INPUT%" --port %IPHONEMU_QMP% quit
exit /b %ERRORLEVEL%

:key
"%PY%" -I "%INPUT%" --port %IPHONEMU_QMP% key %CMD%
exit /b %ERRORLEVEL%

:input_one
"%PY%" -I "%INPUT%" --port %IPHONEMU_QMP% %CMD%
exit /b %ERRORLEVEL%

:shot
if "%~2"=="" (
    echo iphonemu: shot needs a file name, e.g. iphonemu shot tela.png
    exit /b 2
)
"%PY%" -I "%INPUT%" --port %IPHONEMU_QMP% shot "%~f2"
exit /b %ERRORLEVEL%

:input
shift
set "REST="
:input_args
if "%~1"=="" goto input_run
set "REST=%REST% %1"
shift
goto input_args
:input_run
"%PY%" -I "%INPUT%" --port %IPHONEMU_QMP% %REST%
exit /b %ERRORLEVEL%
