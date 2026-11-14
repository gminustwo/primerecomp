@echo off
rem Builds primerecomp_ngx.dll: PrimeRecomp's NVIDIA DLSS support (runtime/nvn/dlss.h).
rem
rem NVIDIA's NGX library is built with MSVC and can't be linked into the MSYS2 / clang executable, so the code that
rem calls it is a DLL of its own, built here with Visual Studio's compiler. Needs:
rem   - Visual Studio 2019 / 2022 or its Build Tools, with "Desktop development with C++"
rem   - a clone of https://github.com/NVIDIA/DLSS  (NVIDIA's license: don't commit or redistribute it)
rem   - Vulkan headers: the LunarG Vulkan SDK (VULKAN_SDK is set by its installer), or point VULKAN_INC at an
rem     include folder that has vulkan\vulkan.h (MSYS2: C:\msys64\ucrt64\include)
rem
rem   set NGX_SDK=C:\src\DLSS
rem   tools\build_ngx_bridge_windows.bat
rem
rem Then put these next to primerecomp.exe:
rem   build\primerecomp_ngx.dll                          (this)
rem   %NGX_SDK%\lib\Windows_x86_64\rel\nvngx_dlss.dll    (NVIDIA's, from the SDK)
rem The Video tab then offers "NVIDIA DLSS" under Upscaler (GeForce RTX, a current NVIDIA driver).
setlocal
cd /d "%~dp0.."
if "%NGX_SDK%"=="" (
    echo Set NGX_SDK to your clone of https://github.com/NVIDIA/DLSS first.
    exit /b 1
)
if not exist "%NGX_SDK%\include\nvsdk_ngx_vk.h" (
    echo "%NGX_SDK%\include\nvsdk_ngx_vk.h" not found: NGX_SDK is not a DLSS SDK folder.
    exit /b 1
)
if "%VULKAN_INC%"=="" if not "%VULKAN_SDK%"=="" set "VULKAN_INC=%VULKAN_SDK%\Include"
if "%VULKAN_INC%"=="" if exist "C:\msys64\ucrt64\include\vulkan\vulkan.h" set "VULKAN_INC=C:\msys64\ucrt64\include"
if "%VULKAN_INC%"=="" (
    echo Vulkan headers not found: install the Vulkan SDK or set VULKAN_INC.
    exit /b 1
)
rem (labels, not a parenthesized block: %VAR% inside one is read before the block's own set lines run)
where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_cl
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" goto no_cl
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
goto have_cl
:no_cl
echo Visual Studio's C++ compiler not found: run this from an "x64 Native Tools Command Prompt".
exit /b 1
:have_cl
if not exist build\ngx mkdir build\ngx
rem (/MT with NVIDIA's static-runtime library: the DLL needs no Visual C++ redistributable on the player's PC)
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /DDLSS_BRIDGE_DLL /I"%NGX_SDK%\include" /I"%VULKAN_INC%" ^
   runtime\nvn\ngx\dlss_ngx.cpp /Fo:build\ngx\ /Fe:build\primerecomp_ngx.dll ^
   /link /LIBPATH:"%NGX_SDK%\lib\Windows_x86_64\x64" nvsdk_ngx_s.lib advapi32.lib user32.lib version.lib
if errorlevel 1 exit /b 1
echo.
echo build\primerecomp_ngx.dll is ready. Copy it, and nvngx_dlss.dll from
echo   %NGX_SDK%\lib\Windows_x86_64\rel
echo next to primerecomp.exe.
