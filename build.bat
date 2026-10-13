@echo off
rem RetroFlatbed - builds the core library, CLI tool, TWAIN data source and WIA driver.
rem   build.bat          x64 build  -> build\
rem   build.bat x86      32-bit build (core, CLI, TWAIN source, test tool; no WIA) -> build\x86\
setlocal
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
set "VCDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"
if /i "%ARCH%"=="x86" (set "VCVARS=%VCDIR%\vcvars32.bat") else (set "VCVARS=%VCDIR%\vcvars64.bat")
if not exist "%VCVARS%" goto :novc
call "%VCVARS%" >nul || exit /b 1
goto :havevc
:novc
echo %VCVARS% not found
exit /b 1
:havevc

set "ROOT=%~dp0"
if /i "%ARCH%"=="x86" (set "OUT=%ROOT%build\x86") else (set "OUT=%ROOT%build")
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"

set "CFLAGS=/nologo /O2 /MT /W3 /wd4244 /wd4267 /wd4018 /wd4101 /wd4102 /wd4090 /wd4133 /wd4146 /wd4996 /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 /DWIN32 /DHAVE_SYS_TIME_H /I"%ROOT%src\compat" /FI"%ROOT%src\compat\lide20_compat.h""

echo [1/5] core library (%ARCH%)
cl %CFLAGS% /c "%ROOT%src\lide20_core.c" "%ROOT%src\winusb_usb.c" "%ROOT%third_party\sane\sanei\sanei_lm983x.c" || exit /b 1
lib /nologo /OUT:lide20core.lib lide20_core.obj winusb_usb.obj sanei_lm983x.obj || exit /b 1

echo [2/5] retroflatbed-scan.exe
cl %CFLAGS% /I"%ROOT%src" "%ROOT%tools\lide20scan.c" /Fe:retroflatbed-scan.exe /link lide20core.lib winusb.lib setupapi.lib advapi32.lib user32.lib || exit /b 1

echo [3/5] RetroFlatbed.ds
rc /nologo /I"%ROOT%src\twain" /fo ds.res "%ROOT%src\twain\ds.rc" || exit /b 1
cl /nologo /O2 /MT /W3 /wd4244 /wd4267 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%src" /I"%ROOT%src\twain" /LD "%ROOT%src\twain\ds.c" /Fe:RetroFlatbed.ds /link /DEF:"%ROOT%src\twain\ds.def" ds.res lide20core.lib winusb.lib setupapi.lib advapi32.lib user32.lib gdi32.lib || exit /b 1

echo [4/5] twain_test.exe
cl /nologo /O2 /MT /W3 /wd4244 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%src\twain" "%ROOT%tools\twain_test.c" /Fe:twain_test.exe /link user32.lib || exit /b 1

if /i "%ARCH%"=="x86" goto :finish
echo [5/5] RetroFlatbedWIA.dll
cl /nologo /O2 /MT /EHsc /W3 /wd4244 /wd4267 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%src" /LD "%ROOT%wia\lide20wia.cpp" /Fe:RetroFlatbedWIA.dll /link /DEF:"%ROOT%wia\lide20wia.def" lide20core.lib winusb.lib setupapi.lib wiaguid.lib wiaservc.lib sti.lib ole32.lib oleaut32.lib propsys.lib advapi32.lib user32.lib || exit /b 1
:finish
echo done.
endlocal
