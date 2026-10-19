#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Register')]
    [string] $ComPort,

    [Parameter(ParameterSetName = 'Register')]
    [string] $LogFile = '',

    [Parameter(Mandatory = $true, ParameterSetName = 'Unregister')]
    [switch] $Unregister
)

$ErrorActionPreference = 'Stop'

# Must match DeviceRegistryKey in src/config/registry_settings.h.
$keyPath = 'SOFTWARE\PassThruSupport.04.04\OBDLink STN'

foreach ($entry in @(
        @{ View = [Microsoft.Win32.RegistryView]::Registry32; Dll = 'obdstn32.dll' },
        @{ View = [Microsoft.Win32.RegistryView]::Registry64; Dll = 'obdstn64.dll' }
    )) {
    $root = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $entry.View)
    if ($Unregister) {
        $root.DeleteSubKeyTree($keyPath, $false)
        Write-Host "Removed $($entry.View) registration"
        continue
    }

    $dllPath = Join-Path $PSScriptRoot $entry.Dll
    if (-not (Test-Path $dllPath)) {
        Write-Host "Skipping $($entry.View): $dllPath not found"
        continue
    }

    $key = $root.CreateSubKey($keyPath)
    $key.SetValue('Name', 'OBDLink STN', [Microsoft.Win32.RegistryValueKind]::String)
    $key.SetValue('Vendor', 'OBDLink STN J2534', [Microsoft.Win32.RegistryValueKind]::String)
    $key.SetValue('FunctionLibrary', $dllPath, [Microsoft.Win32.RegistryValueKind]::String)
    $key.SetValue('ConfigApplication', (Join-Path $PSScriptRoot 'register.ps1'), [Microsoft.Win32.RegistryValueKind]::String)
    $key.SetValue('CAN', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $key.SetValue('ISO15765', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $key.SetValue('CAN_PS', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $key.SetValue('ISO15765_PS', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $key.SetValue('ComPort', $ComPort, [Microsoft.Win32.RegistryValueKind]::String)
    $key.SetValue('LogFile', $LogFile, [Microsoft.Win32.RegistryValueKind]::String)
    $key.Close()
    Write-Host "Registered $dllPath in the $($entry.View) view"
}
