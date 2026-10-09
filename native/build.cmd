@echo off
rem Builds the EFML runtime with MSVC (no CMake needed) and runs the native tests.
rem Output: build\native\{EFML.Host.dll, EFML.Bootstrap.dll, xinput1_4.dll, EFML.Injector.exe, efml-tests.exe}
setlocal
cd /d "%~dp0.."
for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "EFML_VS=%%I"
if not defined EFML_VS (echo Visual Studio C++ tools not found & exit /b 2)
call "%EFML_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 2

set "OUT=%CD%\build\native"
set "OBJ=%CD%\build\obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%\minhook" mkdir "%OBJ%\minhook"
if not exist "%OBJ%\host" mkdir "%OBJ%\host"
if not exist "%OBJ%\loader" mkdir "%OBJ%\loader"
if not exist "%OBJ%\tests" mkdir "%OBJ%\tests"

set "COMMON=/nologo /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Zi"
set "CPP=%COMMON% /std:c++20 /EHsc /W4 /permissive-"
set "LINKSAFE=/DYNAMICBASE /NXCOMPAT /DEBUG /OPT:REF /OPT:ICF"

echo [1/5] MinHook
cl %COMMON% /W3 /c native\vendor\minhook\src\buffer.c native\vendor\minhook\src\hook.c native\vendor\minhook\src\trampoline.c native\vendor\minhook\src\hde\hde64.c /Fo"%OBJ%\minhook\\" /Fd"%OBJ%\minhook\\" >nul
if errorlevel 1 exit /b 1

echo [2/5] EFML.Host.dll
cl %CPP% /c /Inative\include /Inative\vendor\minhook\include native\host\*.cpp /Fo"%OBJ%\host\\" /Fd"%OBJ%\host\\"
if errorlevel 1 exit /b 1
link /nologo /DLL %LINKSAFE% /OUT:"%OUT%\EFML.Host.dll" "%OBJ%\host\*.obj" "%OBJ%\minhook\*.obj" ws2_32.lib bcrypt.lib user32.lib
if errorlevel 1 exit /b 1

echo [3/5] Loaders
cl %CPP% /DEFML_BOOTSTRAP /LD native\loader\input_proxy.cpp /Fo"%OBJ%\loader\bootstrap.obj" /Fd"%OBJ%\loader\bootstrap.pdb" /Fe"%OUT%\EFML.Bootstrap.dll" /link %LINKSAFE% >nul
if errorlevel 1 exit /b 1
cl %CPP% /DEFML_XINPUT_PROXY /LD native\loader\input_proxy.cpp /Fo"%OBJ%\loader\proxy.obj" /Fd"%OBJ%\loader\proxy.pdb" /Fe"%OUT%\xinput1_4.dll" /link %LINKSAFE% >nul
if errorlevel 1 exit /b 1
rc /nologo /fo "%OBJ%\loader\injector.res" native\loader\injector.rc
if errorlevel 1 exit /b 1
cl %CPP% native\loader\injector.cpp "%OBJ%\loader\injector.res" /Fo"%OBJ%\loader\injector.obj" /Fd"%OBJ%\loader\injector.pdb" /Fe"%OUT%\EFML.Injector.exe" /link %LINKSAFE% /MANIFEST:NO >nul
if errorlevel 1 exit /b 1

echo [4/5] Echo example module
if not exist "%OUT%\echo" mkdir "%OUT%\echo"
cl %CPP% /LD /Inative\include sdk\examples\echo\native\echo.cpp /Fo"%OBJ%\tests\echo.obj" /Fd"%OBJ%\tests\echo.pdb" /Fe"%OUT%\echo\example.echo.dll" /link %LINKSAFE% >nul
if errorlevel 1 exit /b 1

echo [5/5] Tests
cl %CPP% /LD /Inative\include native\tests\legacy_module.cpp /Fo"%OBJ%\tests\legacy.obj" /Fd"%OBJ%\tests\legacy.pdb" /Fe"%OUT%\test.legacy.dll" /link %LINKSAFE% >nul
if errorlevel 1 exit /b 1
cl %CPP% /Inative\include /Inative\vendor\minhook\include native\tests\module_host_test.cpp native\host\module_host.cpp native\host\game_thread.cpp native\host\paths.cpp native\host\logging.cpp /Fo"%OBJ%\tests\\" /Fd"%OBJ%\tests\\" /Fe"%OUT%\efml-tests.exe" /link ws2_32.lib bcrypt.lib >nul
if errorlevel 1 exit /b 1
"%OUT%\efml-tests.exe" "%OUT%\echo\example.echo.dll" "%OUT%\test.legacy.dll"
exit /b %errorlevel%
