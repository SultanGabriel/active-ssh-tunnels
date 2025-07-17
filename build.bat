@echo off
REM Chief Tunnel Officer - Windows Build Script
REM ==========================================

setlocal EnableDelayedExpansion

if "%1"=="" goto help
if "%1"=="help" goto help
if "%1"=="setup" goto setup
if "%1"=="build" goto build
if "%1"=="test" goto test
if "%1"=="clean" goto clean
if "%1"=="run" goto run
if "%1"=="config" goto config

echo Unknown command: %1
goto help

:help
echo.
echo Chief Tunnel Officer - Windows Build Script
echo ==========================================
echo.
echo Available commands:
echo   build.bat setup   - Download cJSON and create example config
echo   build.bat build   - Build the tunnel manager
echo   build.bat test    - Build and run unit tests
echo   build.bat clean   - Clean build files
echo   build.bat run     - Build and run the tunnel manager
echo   build.bat config  - Create example config.json
echo   build.bat help    - Show this help
echo.
echo Requirements:
echo   - MinGW-w64 or MSYS2 with gcc
echo   - Make sure gcc is in your PATH
echo.
echo Example:
echo   build.bat setup
echo   build.bat build
echo   build.bat run
echo.
goto end

:setup
echo Setting up Chief Tunnel Officer...
echo.

REM Check if cJSON exists
if not exist cjson\cJSON.c (
    echo Downloading cJSON library...
    if not exist cjson mkdir cjson
    powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c' -OutFile 'cjson/cJSON.c'"
    powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h' -OutFile 'cjson/cJSON.h'"
    echo cJSON downloaded successfully
) else (
    echo cJSON already exists
)

REM Create config if it doesn't exist
if not exist config.json (
    echo Creating example config.json...
    call :config
) else (
    echo config.json already exists
)

if not exist logs mkdir logs

echo.
echo Setup complete!
echo.
echo Next steps:
echo 1. Edit config.json with your SSH tunnels
echo 2. Run 'build.bat build' to compile
echo 3. Run 'build.bat run' to start
echo.
goto end

:build
echo Building Chief Tunnel Officer...
echo.

REM Check for gcc
gcc --version >nul 2>&1
if errorlevel 1 (
    echo Error: gcc not found in PATH
    echo.
    echo Install MinGW-w64 or MSYS2:
    echo   MSYS2: https://www.msys2.org/
    echo   MinGW-w64: https://www.mingw-w64.org/
    echo.
    echo Make sure gcc is in your PATH
    exit /b 1
)

REM Compile cJSON
echo Compiling cJSON...
gcc -Wall -Wextra -std=c99 -O2 -Icjson -c cjson/cJSON.c -o cjson/cJSON.o
if errorlevel 1 (
    echo Error compiling cJSON
    exit /b 1
)

REM Compile main program
echo Compiling tunnel manager...
gcc -Wall -Wextra -std=c99 -pthread -O2 -DWINDOWS -Icjson -c main.c -o main.o
if errorlevel 1 (
    echo Error compiling main.c
    exit /b 1
)

REM Link executable
echo Linking tunnel_manager.exe...
gcc main.o cjson/cJSON.o -o tunnel_manager.exe -pthread -lws2_32
if errorlevel 1 (
    echo Error linking executable
    exit /b 1
)

echo.
echo Build successful: tunnel_manager.exe
echo.
goto end

:test
echo Building and running unit tests...
echo.

REM Check for gcc
gcc --version >nul 2>&1
if errorlevel 1 (
    echo Error: gcc not found in PATH
    goto end
)

REM Compile test program
echo Compiling test suite...
gcc -Wall -Wextra -std=c99 -O2 -DWINDOWS -Icjson -I. test.c -o test_tunnel_manager.exe
if errorlevel 1 (
    echo Error compiling tests
    exit /b 1
)

echo.
echo Running tests...
test_tunnel_manager.exe
goto end

:clean
echo Cleaning build files...
if exist main.o del main.o
if exist test.o del test.o
if exist cjson\cJSON.o del cjson\cJSON.o
if exist tunnel_manager.exe del tunnel_manager.exe
if exist test_tunnel_manager.exe del test_tunnel_manager.exe
echo Clean complete
goto end

:run
call :build
if errorlevel 1 goto end

echo.
echo Starting Chief Tunnel Officer...
echo.
tunnel_manager.exe
goto end

:config
echo Creating example config.json...
(
echo {
echo   "tunnels": [
echo     {
echo       "name": "db-prod",
echo       "host": "db.example.com",
echo       "port": 22,
echo       "user": "admin",
echo       "local_port": 3307,
echo       "remote_host": "127.0.0.1",
echo       "remote_port": 3306,
echo       "reconnect_delay": 5
echo     },
echo     {
echo       "name": "web-staging",
echo       "host": "staging.example.com",
echo       "port": 22,
echo       "user": "deploy",
echo       "local_port": 8080,
echo       "remote_host": "localhost",
echo       "remote_port": 80,
echo       "reconnect_delay": 3
echo     }
echo   ]
echo }
) > config.json
echo Example config.json created
goto end

:end
echo.
pause
