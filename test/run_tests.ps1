#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Сборка и запуск хост-тестов драйвера I2C-CH32V003.

.DESCRIPTION
    Компилирует неизменённый src/i2c.c обычным компилятором, подменяя SDK-заголовок
    ch32v00x.h на программную модель периферии (test/mock/ch32v00x.h), и прогоняет
    набор тестов конечного автомата. Железо не требуется.

    Компилятор ищется в следующем порядке: g++, clang++, затем g++ из пакета
    PlatformIO toolchain-gccmingw32 (устанавливается вместе с платформой ch32v).

.EXAMPLE
    pwsh test/run_tests.ps1
    Прогон в конфигурации по умолчанию и с I2C_ATOMIC_CRITICAL=1.
#>

[CmdletBinding()]
param(
    # Дополнительные наборы макросов для прогона (каждый — отдельная сборка)
    [string[]]$Configs = @(
        '',
        '-DI2C_ATOMIC_CRITICAL=1',
        '-DI2C_LEGACY_STATUS=1',
        '-DI2C_TIMEOUT_MS=5',
        '-DI2C_DISABLE_ERROR_COUNTER',
        '-DI2C_DISABLE_BUS_RECOVERY',
        '-DI2C_DISABLE_BUFFER_API',
        '-DI2C_DISABLE_SCANNER',
        '-DI2C_LITE=1',
        '-DI2C_LITE=1 -DI2C_ATOMIC_CRITICAL=1'
    )
)

$ErrorActionPreference = 'Stop'
$testDir = $PSScriptRoot
$repoRoot = Split-Path -Parent $testDir

function Resolve-Compiler {
    foreach ($name in @('g++', 'clang++')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }

    $pioHome = if ($env:PLATFORMIO_CORE_DIR) { $env:PLATFORMIO_CORE_DIR } else { Join-Path $HOME '.platformio' }
    $mingw = Join-Path $pioHome 'packages/toolchain-gccmingw32/bin/g++.exe'
    if (Test-Path -LiteralPath $mingw) { return (Resolve-Path -LiteralPath $mingw).Path }

    throw "No host C++ compiler found. Install g++/clang++, or the PlatformIO ch32v platform (provides toolchain-gccmingw32)."
}

$cxx = Resolve-Compiler
Write-Output "Compiler: $cxx"

$outDir = Join-Path $testDir 'build'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$failures = 0

foreach ($config in $Configs) {
    $label = if ([string]::IsNullOrWhiteSpace($config)) { 'default' } else { $config }
    Write-Output ""
    Write-Output "===================================================================="
    Write-Output " Build & run: $label"
    Write-Output "===================================================================="

    $exe = Join-Path $outDir 'test_i2c.exe'
    Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue

    $args = @(
        '-std=c++11', '-O1', '-g',
        '-Wall', '-Wextra',
        "-I$testDir/mock",
        "-I$repoRoot/src",
        "-I$testDir",
        '-x', 'c++', "$repoRoot/src/i2c.c",
        '-x', 'c++', "$testDir/i2c_mock.cpp",
        '-x', 'c++', "$testDir/test_i2c.cpp",
        '-o', $exe
    )
    if (-not [string]::IsNullOrWhiteSpace($config)) {
        # Конфигурация может содержать несколько макросов через пробел
        $args = @($config -split '\s+' | Where-Object { $_ }) + $args
    }

    & $cxx @args
    if ($LASTEXITCODE -ne 0) {
        Write-Output "BUILD FAILED ($label)"
        $failures++
        continue
    }

    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Output "TESTS FAILED ($label)"
        $failures++
    }
}

Write-Output ""
if ($failures -eq 0) {
    Write-Output "All configurations passed."
    exit 0
}

Write-Output "$failures configuration(s) failed."
exit 1
