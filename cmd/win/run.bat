if not defined BUILD_TYPE set "BUILD_TYPE=RelWithDebInfo"
bin\win\%BUILD_TYPE%\tests.exe %*
exit /b %ERRORLEVEL%
