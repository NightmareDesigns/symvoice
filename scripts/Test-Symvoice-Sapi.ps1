param(
    [string]$Text = "Hello, this is my voice."
)

$ErrorActionPreference = "Stop"

try {
    $synthesizer = New-Object -ComObject SAPI.SpVoice
    $voices = $synthesizer.GetVoices()
    $symvoice = $null
    for ($index = 0; $index -lt $voices.Count; $index++) {
        $candidate = $voices.Item($index)
        if ($candidate.GetDescription() -match "Symvoice") {
            $symvoice = $candidate
            break
        }
    }
    if ($null -eq $symvoice) {
        throw "Symvoice is not listed by SAPI. Register it for this Windows user first."
    }

    $synthesizer.Voice = $symvoice
    Write-Host "Speaking with $($symvoice.GetDescription()): $Text"
    $null = $synthesizer.Speak($Text, 0)
}
finally {
    if ($null -ne $synthesizer) {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($synthesizer)
    }
}
