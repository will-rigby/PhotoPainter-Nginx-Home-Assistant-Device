# convert_progressive.ps1
# Recursively convert progressive JPEGs to baseline and shrink oversized
# photos to the frame's 800x480 panel (cover + centred crop, same as the
# device does at display time). Originals are replaced in place with
# timestamps preserved. Files that are already baseline and no larger than
# the panel are skipped, so re-running is a no-op.

$targetW = 800
$targetH = 480

$root = Get-Location
Write-Host "Processing folder: $root"
Write-Host ""

$files = Get-ChildItem -Recurse -Include *.jpg, *.jpeg -File

if ($files.Count -eq 0) {
    Write-Host "No JPEG files found."
    exit
}

$converted = 0
$skipped   = 0
$failed    = 0

foreach ($file in $files) {

    try {
        $probe = magick identify -format "%[interlace]|%[width]|%[height]" "$($file.FullName)"
        if ($LASTEXITCODE -ne 0 -or -not $probe) { throw "identify failed" }

        $interlace, $w, $h = $probe -split '\|'
        $needsBaseline = $interlace -ne 'None'
        $needsResize   = ([int]$w -gt $targetW) -and ([int]$h -gt $targetH)

        if (-not $needsBaseline -and -not $needsResize) {
            $skipped++
            continue
        }

        Write-Host "Converting: $($file.FullName)  ($interlace, ${w}x${h})"

        $tmpPath = "$($file.FullName).tmp.jpg"
        $args = @("$($file.FullName)", '-auto-orient')
        if ($needsResize) {
            $args += @('-resize', "${targetW}x${targetH}^", '-gravity', 'center', '-extent', "${targetW}x${targetH}")
        }
        $args += @('-interlace', 'none', '-quality', '92', "$tmpPath")

        magick @args

        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $tmpPath)) { throw "magick failed" }

        $created  = $file.CreationTime
        $modified = $file.LastWriteTime

        Move-Item -Force $tmpPath $file.FullName

        (Get-Item $file.FullName).CreationTime  = $created
        (Get-Item $file.FullName).LastWriteTime = $modified

        $converted++
        Write-Host "  -> Done."
    }
    catch {
        $failed++
        Write-Warning "  !! Failed to convert $($file.Name): $_"
        if (Test-Path "$($file.FullName).tmp.jpg") { Remove-Item "$($file.FullName).tmp.jpg" }
    }
}

Write-Host ""
Write-Host "Done. Converted: $converted  Skipped (already OK): $skipped  Failed: $failed"
