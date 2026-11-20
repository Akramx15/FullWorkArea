@echo off
gcc -Os -s -nostdlib -mwindows -Wall -Wextra -Wpedantic -Werror ^
    -fno-ident -fno-asynchronous-unwind-tables -fno-unwind-tables ^
    -ffunction-sections -fdata-sections ^
    -Wl,--gc-sections -Wl,--file-alignment,512 ^
    -e WinMainCRTStartup "%~dp0full_workarea.c" -o "%~dp0FullWorkArea.exe" ^
    -lkernel32 -luser32 -lgdi32
if %ERRORLEVEL% EQU 0 (
    echo Build succeeded: FullWorkArea.exe
) else (
    echo Build failed.
    exit /b 1
)
