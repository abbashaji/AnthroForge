@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "RUST_CORE_DIR=%SCRIPT_DIR%rust-core"
set "DEST_DIR=%SCRIPT_DIR%unreal-plugin\Source\AnthroforgeEngine\Binaries\ThirdParty"

echo [copy_lib] building rust-core (release)...
pushd "%RUST_CORE_DIR%"
cargo build --release
if errorlevel 1 (
	echo [copy_lib] cargo build failed.
	popd
	exit /b 1
)
popd

if not exist "%DEST_DIR%" mkdir "%DEST_DIR%"

set "SRC=%RUST_CORE_DIR%\target\release\anthroforge_core.dll"
set "DEST=%DEST_DIR%\anthroforge_core.dll"

if not exist "%SRC%" (
	echo [copy_lib] expected build output not found at "%SRC%".
	exit /b 1
)

copy /Y "%SRC%" "%DEST%" >nul
echo [copy_lib] copied anthroforge_core.dll -^> %DEST%
