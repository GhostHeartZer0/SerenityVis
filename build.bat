@echo off
set "SCRIPT_DIR=%~dp0"
set "CACHE_DIR=%SCRIPT_DIR%.cache"
set "TEMP_DIR=%CACHE_DIR%\temp"

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"
if not exist "%CACHE_DIR%\nv_cuda" mkdir "%CACHE_DIR%\nv_cuda"
if not exist "%CACHE_DIR%\torch_extensions" mkdir "%CACHE_DIR%\torch_extensions"
if not exist "%CACHE_DIR%\torch_kernel_cache" mkdir "%CACHE_DIR%\torch_kernel_cache"
if not exist "%CACHE_DIR%\torch" mkdir "%CACHE_DIR%\torch"
if not exist "%CACHE_DIR%\triton" mkdir "%CACHE_DIR%\triton"
if not exist "%CACHE_DIR%\pip" mkdir "%CACHE_DIR%\pip"
if not exist "%CACHE_DIR%\huggingface" mkdir "%CACHE_DIR%\huggingface"

set "TEMP=%TEMP_DIR%"
set "TMP=%TEMP_DIR%"
set "TMPDIR=%TEMP_DIR%"
set "CUDA_CACHE_PATH=%CACHE_DIR%\nv_cuda"
set "TORCH_EXTENSIONS_DIR=%CACHE_DIR%\torch_extensions"
set "PYTORCH_KERNEL_CACHE_PATH=%CACHE_DIR%\torch_kernel_cache"
set "TORCH_HOME=%CACHE_DIR%\torch"
set "TRITON_CACHE_DIR=%CACHE_DIR%\triton"
set "PIP_CACHE_DIR=%CACHE_DIR%\pip"
set "HF_HOME=%CACHE_DIR%\huggingface"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" (
    call "%VCVARS%"
    cl.exe /EHsc /O2 /Fe:AudioServer.exe AudioServer\AudioServer.cpp
) else (
    echo MSVC compiler not found at "%VCVARS%"
    exit /b 1
)
