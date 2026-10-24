@echo off
rem Optional second build of the TokenLIKE DuckDB extension with MSVC (Visual Studio Build Tools).
rem Output: build\duckdb-msvc\tokenlike.duckdb_extension. Run from any directory:
rem     integration\duckdb\tools\build_msvc.cmd
setlocal
for %%I in ("%~dp0..\..\..") do set "ROOT=%%~fI"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio Build Tools & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No MSVC x64 toolset found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
where ninja >nul 2>nul || if exist "C:\msys64\ucrt64\bin\ninja.exe" set "NINJA_ARG=-DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe"
set "PY=%ROOT%\.venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"
cmake -S "%ROOT%\integration\duckdb" -B "%ROOT%\build\duckdb-msvc" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CXX_COMPILER=cl "-DCMAKE_CXX_FLAGS=/O2 /DNDEBUG" %NINJA_ARG% -DPython3_EXECUTABLE="%PY%" || exit /b 1
cmake --build "%ROOT%\build\duckdb-msvc" || exit /b 1
echo Built %ROOT%\build\duckdb-msvc\tokenlike.duckdb_extension
