$paths = @(
    "C:\projects\dolphin-gdb\Source",
    "C:\projects\dolphin-gdb\Externals",
    "C:\projects\dolphin-gdb\Languages"
)

foreach ($path in $paths) {
    Get-ChildItem -Path $path -Filter "*.vcxproj" -Recurse | ForEach-Object {
        $content = Get-Content $_.FullName -Raw
        if ($content -match "v143") {
            $content = $content -replace "<PlatformToolset>v143</PlatformToolset>", "<PlatformToolset>v145</PlatformToolset>"
            Set-Content -Path $_.FullName -Value $content -NoNewline
            Write-Host "Updated: $($_.Name)"
        }
    }
}
Write-Host "Done!"
