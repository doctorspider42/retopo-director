@echo off
rem Configure, build, and optionally run.
rem
rem   build.cmd                                  build only
rem   build.cmd -run                             build, then open the window
rem   build.cmd -run --mesh high.obj --no-llm    every other argument is the app's
rem
rem Exits with the compiler's code when the build fails, and with the
rem application's own code when -run was given: 0 passed, 3 validation failed.
setlocal

set "PRESET=mingw-release"
set "ROOT=%~dp0"
set "BUILD=%ROOT%build\%PRESET%"
set "EXE=%BUILD%\bin\retopo-director.exe"
set "RUN=0"
set "APPARGS="

:parse
if "%~1"=="" goto parsed
rem -run and -help are the only two flags this script keeps for itself. The
rem application has a --run of its own (start the pipeline on open) and a
rem --help, and both have to survive the trip, so the double dash forms are
rem never touched here.
if /i "%~1"=="-run" (
    set "RUN=1"
    shift
    goto parse
)
if /i "%~1"=="-help" goto usage
if /i "%~1"=="/?"    goto usage
rem Everything else is for the application. Re-quoting each one keeps paths
rem with spaces in one piece; the app's parser strips the quotes again.
set "APPARGS=%APPARGS% "%~1""
shift
goto parse
:parsed

rem Ninja re-runs CMake by itself when CMakeLists.txt changes, so configuring is
rem only needed the first time, or after the build directory has been wiped.
if not exist "%BUILD%\CMakeCache.txt" (
    echo === configure %PRESET%
    cmake --preset %PRESET%
    if errorlevel 1 exit /b 1
)

echo === build %PRESET%
cmake --build --preset %PRESET%
if errorlevel 1 (
    echo.
    echo build failed
    exit /b 1
)

if not "%RUN%"=="1" exit /b 0

if not exist "%EXE%" (
    echo.
    echo built, but %EXE% is not there
    exit /b 1
)

echo === run
"%EXE%"%APPARGS%
exit /b %ERRORLEVEL%

:usage
echo Retopo Director build script.
echo.
echo   build.cmd                                  build only
echo   build.cmd -run                             build, then open the window
echo   build.cmd -run --mesh high.obj --no-llm    anything else goes to the app
echo.
echo Preset: %PRESET%. -run and -help are this script's; every other argument
echo belongs to the application, including its own --run and --help.
exit /b 0
