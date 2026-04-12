@echo off
setlocal

pushd "%~dp0\.."
idf.py %* littlefs-flash
set "exit_code=%ERRORLEVEL%"
popd

exit /b %exit_code%