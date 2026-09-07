@ECHO OFF
SETLOCAL ENABLEEXTENSIONS
CD /D "%~dp0"

TITLE Build (MinGW + CMake)

rem ===========================================================================
rem Setup
rem ===========================================================================
set "ROOT_DIR=%CD%"
set "BUILD_DIR=%CD%\build"
::set "RELEASE_DIR=%CD%\release"
set "BIN_DIR=%BUILD_DIR%\bin"

rem MinGW paths (use short name to avoid space-in-path issues)
set "MINGW_ROOT=D:\ProgramFiles\MinGW"
set "GCC_VER=15.2.0"
set "CMAKE_EXE=%MINGW_ROOT%\bin\cmake.exe"
set "MINGW_MAKE=%MINGW_ROOT%\bin\mingw32-make.exe"
set "MINGW_STRIP=%MINGW_ROOT%\x86_64-w64-mingw32\bin\strip"

::where cmake >nul 2>&1 && set "CMAKE_EXE=cmake"

set "BUILD_TYPE=Release"
set "ACTION=Build"

rem ===========================================================================
rem Parse arguments
rem ===========================================================================
IF /I "%~1" == "help"     GOTO SHOWHELP
IF /I "%~1" == "/help"    GOTO SHOWHELP
IF /I "%~1" == "Clean"    SET "ACTION=Clean"    & GOTO PARSECONFIG
IF /I "%~1" == "Rebuild"  SET "ACTION=Rebuild"  & GOTO PARSECONFIG
IF /I "%~1" == "Config"   SET "ACTION=Config"   & GOTO CONFIGURE

:PARSECONFIG

rem ===========================================================================
rem Set LIBRARY_PATH to work around MinGW path-with-spaces bug
rem ===========================================================================
set "LIBRARY_PATH=%MINGW_ROOT%\lib\gcc\x86_64-w64-mingw32\%GCC_VER%;%MINGW_ROOT%\x86_64-w64-mingw32\lib;%MINGW_ROOT%\lib"

ECHO.
ECHO ============================================================
ECHO  Action:      %ACTION%
ECHO  Config:      %BUILD_TYPE%
ECHO  MinGW:       %MINGW_ROOT%
ECHO  Output:      %BIN_DIR%
ECHO ============================================================
ECHO.

rem ============================================================================
rem Configure (if not already configured)
rem ============================================================================
IF "%ACTION%" == "Build" (
    IF NOT EXIST "%BUILD_DIR%\CMakeCache.txt" (
        ECHO [*] Configuring CMake for the first time...
        CALL :CONFIGURE
    )
)
IF "%ACTION%" == "Rebuild" (
    ECHO [*] Reconfiguring CMake...
    IF EXIST "%BUILD_DIR%" RD /S /Q "%BUILD_DIR%"
    CALL :CONFIGURE
)

rem ============================================================================
rem Build
rem ============================================================================
IF "%ACTION%" == "Clean" GOTO DOCLEAN

ECHO [*] Building (mingw32-make -j%NUMBER_OF_PROCESSORS%)...
CD /D "%BUILD_DIR%"
%MINGW_MAKE% -j%NUMBER_OF_PROCESSORS%
IF %ERRORLEVEL% NEQ 0 (
    ECHO.
    ECHO [ERROR] Build failed! Errorlevel: %ERRORLEVEL%
    GOTO END
)

ECHO.
ECHO [SUCCESS] Build completed!
::IF EXIST "%RELEASE_DIR%" RD /S /Q "%RELEASE_DIR%"
::MKDIR "%RELEASE_DIR%"
copy %BUILD_DIR%\*.exe %ROOT_DIR%\ /y
%MINGW_STRIP% --strip-all %ROOT_DIR%\*.exe

GOTO END

:DOCLEAN
ECHO [*] Cleaning...
IF EXIST "%BUILD_DIR%" RD /S /Q "%BUILD_DIR%"
MKDIR "%BUILD_DIR%"
ECHO [DONE] Clean finished.
GOTO END

:CONFIGURE
IF NOT EXIST "%BUILD_DIR%" MKDIR "%BUILD_DIR%"
CD /D "%BUILD_DIR%"
"%CMAKE_EXE%" -G "MinGW Makefiles" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY ^
    -DCMAKE_C_COMPILER="%MINGW_ROOT%/bin/gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%MINGW_ROOT%/bin/g++.exe" ^
    ..
IF %ERRORLEVEL% NEQ 0 (
    ECHO [ERROR] CMake configuration failed!
    EXIT /B 1
)
EXIT /B 0

:SHOWHELP
ECHO Usage: %~nx0 [Action] [Config]
ECHO.
ECHO Actions:  Build (default), Clean, Rebuild
ECHO Config:   Release (default)
ECHO.
GOTO END

:END
CD /D "%~dp0"
