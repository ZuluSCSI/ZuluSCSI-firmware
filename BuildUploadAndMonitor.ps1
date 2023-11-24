$env = "ZuluSCSI_Pico_DaynaPORT"
$proj = "M:\Projects\Computer Stuff\CLassic Mac\Expansion Cards\Silly Tiny SCSI (ZuluSCSI)\firmware\ZuluSCSI-firmware-crdev"

# no need to edit these paths
$inpath = "$proj\.pio\build\$env\firmware.uf2"
$srch_label = "RPI-RP2"
$srch_PID = "VID_2E8A"

Write-host "Building $env in $proj ..."
pio run -e $env

if ($LASTEXITCODE -eq 0) {
        
    if (Test-Path $inpath) {
        Write-host -NoNewline "Waiting for drive with label $srch_label"

        while (! ($DRV = get-ciminstance Win32_Volume | ?{$_.Label -eq $srch_label} | Select -First 1)) {
            sleep 1
            write-host -NoNewline "."
        }
    
        Write-host -NoNewLine "`r`nUploading..."

        Copy-Item $inpath $DRV.Caption
        
        Write-host -NoNewLine "Done.`r`nWaiting for COM port with PID $srch_PID"

        while (! ($COM = Get-CimInstance -Class Win32_SerialPort | ? {$_.PNPDeviceID -like "*$srch_PID*"} | select -first 1)) {
            sleep -Milliseconds 100
            write-host -NoNewline "."
        }

        Write-host "`r`nBegin monitor mode."

        pio device monitor -p $COM.DeviceID
    } else {
        Write-host "$inpath is missing!"
    }
} else {
    write-host "Build failed."
}