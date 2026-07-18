# convert-heic.ps1
# Recursively convert HEIC → JPG
# Delete original only if conversion succeeds

$root = Get-Location
Write-Host "Processing folder: $root"
Write-Host ""

$files = Get-ChildItem -Recurse -Filter *.heic -File

if ($files.Count -eq 0) {
    Write-Host "No HEIC files found."
    exit
}

foreach ($file in $files) {

    $jpgPath = [System.IO.Path]::ChangeExtension($file.FullName, ".jpg")

    Write-Host "Converting: $($file.FullName)"

    try {
        magick "$($file.FullName)" -quality 92 "$jpgPath"

        if (Test-Path $jpgPath) {

            # Preserve timestamps
            (Get-Item $jpgPath).CreationTime  = $file.CreationTime
            (Get-Item $jpgPath).LastWriteTime = $file.LastWriteTime

            Remove-Item $file.FullName
            Write-Host "  → Converted and deleted original."
        }
        else {
            Write-Warning "  !! Conversion failed (no JPG created)"
        }
    }
    catch {
        Write-Warning "  !! Failed to convert $($file.Name)"
    }
}

Write-Host ""
Write-Host "Done."