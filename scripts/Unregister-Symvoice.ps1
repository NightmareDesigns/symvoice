$ErrorActionPreference = "Stop"

$clsid = "{73EA0E66-7B92-4D89-8BE0-A633F9274291}"
[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree(
    "Software\Microsoft\Speech\Voices\Tokens\Symvoice", $false)
[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree(
    "Software\Classes\CLSID\$clsid", $false)

Write-Host "Removed the Symvoice voice registration for the current Windows user."
