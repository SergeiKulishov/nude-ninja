@echo off
setlocal
set "BUILD_DIR=%~dp0build_x64\rundir\RelWithDebInfo"
set "OBS_DIR=%ProgramFiles%\obs-studio"

if not exist "%BUILD_DIR%\fullblur-filter.dll" (
    echo DLL not found: %BUILD_DIR%\fullblur-filter.dll
    echo Build the plugin first: cmake --preset windows-x64 ^&^& cmake --build --preset windows-x64
    exit /b 1
)

if not exist "%OBS_DIR%" (
    echo OBS Studio not found at %OBS_DIR%
    exit /b 1
)

echo Installing to %OBS_DIR% ...
copy /y "%BUILD_DIR%\fullblur-filter.dll" "%OBS_DIR%\obs-plugins\64bit\" || goto :need_admin
copy /y "%BUILD_DIR%\onnxruntime.dll" "%OBS_DIR%\obs-plugins\64bit\" || goto :need_admin
copy /y "%BUILD_DIR%\onnxruntime_providers_shared.dll" "%OBS_DIR%\obs-plugins\64bit\" || goto :need_admin
copy /y "%BUILD_DIR%\DirectML.dll" "%OBS_DIR%\obs-plugins\64bit\" || goto :need_admin
if exist "%BUILD_DIR%\fullblur-filter" (
    xcopy /y /e /i "%BUILD_DIR%\fullblur-filter" "%OBS_DIR%\data\obs-plugins\fullblur-filter" >nul || goto :need_admin
)
echo Done. Restart OBS Studio.
exit /b 0

:need_admin
echo Access denied. Run this script as Administrator.
exit /b 1
