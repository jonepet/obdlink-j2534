#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Register')]
    [string] $ComPort,

    [Parameter(ParameterSetName = 'Register')]
    [string] $LogFile = '',

    [Parameter(ParameterSetName = 'Register')]
    [switch] $LowLatency,

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

if ($LowLatency) {
    # Opt-in: the FTDI latency timer defaults to 16 ms, delaying every adapter response by that much.
    $deviceParameters = Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\FTDIBUS' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -eq 'Device Parameters' -and (Get-ItemProperty $_.PSPath).PortName -eq $ComPort }
    foreach ($parameters in $deviceParameters) {
        Set-ItemProperty -Path $parameters.PSPath -Name 'LatencyTimer' -Value 2 -Type DWord
        Write-Host "Set FTDI LatencyTimer=2 ms for $ComPort"
    }
    # The VCP driver only reads the value when the devnode starts, so reload it instead of a replug.
    foreach ($device in @(Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like "*($ComPort)*" })) {
        try {
            Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
            Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
            Write-Host "Reloaded $ComPort so the new latency timer applies immediately"
        } catch {
            Write-Host "Set the latency timer for $ComPort; unplug and replug the adapter to apply it"
        }
    }
}
