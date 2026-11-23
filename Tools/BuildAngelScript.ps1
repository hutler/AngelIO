[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$version = '2.39.0'
$cache = Join-Path ([IO.Path]::GetTempPath()) "AngelIO-sdk-$version"
New-Item -ItemType Directory -Force $cache | Out-Null
$archive = Join-Path $cache 'sdk.zip'
if (!(Test-Path $archive)) {
    Invoke-WebRequest "https://www.angelcode.com/angelscript/sdk/files/angelscript_$version.zip" -OutFile $archive
}
if ((Get-FileHash $archive).Hash -ne 'F029BEA4E52C360C750D3A805B80F9317D177241FADBC8F93845E38355D11287') {
    throw "Unexpected SDK checksum in $archive"
}
Expand-Archive $archive -DestinationPath $cache -Force
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $locator -latest -products '*' -version '[17,18)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2022 C++ tools are required to rebuild the SDK.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$build = Join-Path $cache 'build'
$sdkCmake = Join-Path $cache 'sdk/angelscript/projects/cmake/CMakeLists.txt'
Add-Content $sdkCmake 'target_sources(angelscript PRIVATE ../../../add_on/scriptstdstring/scriptstdstring.cpp)'
& $cmake -S "$cache/sdk/angelscript/projects/cmake" -B $build -G 'Visual Studio 17 2022' -A x64 -DBUILD_SHARED_LIBS=OFF -DAS_DISABLE_INSTALL=ON
if ($LASTEXITCODE) { throw 'CMake configuration failed' }
& $cmake --build $build --config Release --parallel
if ($LASTEXITCODE) { throw 'AngelScript build failed' }
$dest = Join-Path $PSScriptRoot '../Plugins/AngelIO/Source/ThirdParty/AngelScript'
New-Item -ItemType Directory -Force "$dest/include","$dest/lib/Win64" | Out-Null
Copy-Item "$cache/sdk/angelscript/include/angelscript.h" "$dest/include/"
Copy-Item "$cache/sdk/add_on/scriptstdstring/scriptstdstring.h" "$dest/include/"
Copy-Item "$build/Release/angelscript.lib" "$dest/lib/Win64/"
