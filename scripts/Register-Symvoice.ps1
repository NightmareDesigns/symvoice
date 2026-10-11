param(
    [Parameter(Mandatory = $true)]
    [string]$DllPath
)

$ErrorActionPreference = "Stop"
$resolvedDll = (Resolve-Path -LiteralPath $DllPath).Path
if ([IO.Path]::GetExtension($resolvedDll) -ne ".dll") {
    throw "DllPath must point to the Symvoice SAPI DLL."
}

$clsid = "{73EA0E66-7B92-4D89-8BE0-A633F9274291}"
$classPath = "Software\Classes\CLSID\$clsid"
$tokenPath = "Software\Microsoft\Speech\Voices\Tokens\Symvoice"

$classKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("$classPath\InprocServer32")
try {
    $classKey.SetValue("", $resolvedDll, [Microsoft.Win32.RegistryValueKind]::String)
    $classKey.SetValue("ThreadingModel", "Both", [Microsoft.Win32.RegistryValueKind]::String)
}
finally {
    $classKey.Dispose()
}

$voiceKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($tokenPath)
try {
    $voiceKey.SetValue("", "Symvoice", [Microsoft.Win32.RegistryValueKind]::String)
    $voiceKey.SetValue("CLSID", $clsid, [Microsoft.Win32.RegistryValueKind]::String)
}
finally {
    $voiceKey.Dispose()
}

$attributes = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("$tokenPath\Attributes")
try {
    $attributes.SetValue("Name", "Symvoice", [Microsoft.Win32.RegistryValueKind]::String)
    $attributes.SetValue("Language", "409", [Microsoft.Win32.RegistryValueKind]::String)
    $attributes.SetValue("Vendor", "Symvoice", [Microsoft.Win32.RegistryValueKind]::String)
}
finally {
    $attributes.Dispose()
}

Write-Host "Registered Symvoice for the current Windows user."
Write-Host "Keep the Symvoice speech server running before selecting this voice."
