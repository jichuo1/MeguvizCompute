@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake --preset %1
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset %1
if errorlevel 1 exit /b %errorlevel%
ctest --preset %1
if errorlevel 1 exit /b %errorlevel%
cmake --install build/%1 --prefix build/install/%1
exit /b %errorlevel%
