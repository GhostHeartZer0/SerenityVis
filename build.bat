@echo off
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" (
    call "%VCVARS%"
    cl.exe /EHsc /O2 /Fe:AudioServer.exe AudioServer\AudioServer.cpp
) else (
    echo MSVC compiler not found at "%VCVARS%"
    exit /b 1
)
