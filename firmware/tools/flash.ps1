<#
.SYNOPSIS
    Build, flash and monitor the EthercatHMI firmware on the JC1060P470C (ESP32-P4).
.DESCRIPTION
    Started by firmware\flash.bat. Shows a menu unless -Action is given.
    Works on one app of firmware\apps (-App, or the menu asks when there are several); each app
    builds into <drive>:\esp\build\ehmi_<app>.
    Before writing to the board it:
      - reads the chip revision and refuses an image built for the other revision family
        (v1.x and v3.x images are not interchangeable, docs\SETUP.md section 4);
      - makes a full 16 MB backup of the board's flash once per board (keyed by MAC),
        unless one already exists (docs\SETUP.md section 3).
    After a flash it sets the board's clock and RTC to the PC's time (tools\set_clock.py).
    Messages are in Vietnamese. This file must stay UTF-8 *with BOM*, or Windows PowerShell 5.1
    misreads the Vietnamese strings.

    Functions that run idf.py or esptool are called as plain statements so the tool output goes
    straight to the console (live progress, a real TTY for the monitor). They report success
    through $script:Ok instead of a return value, because PowerShell would mix the tool output
    into the return value.
.EXAMPLE
    flash.bat
    flash.bat COM7
    powershell -File tools\flash.ps1 -Port COM7 -Action flash
    powershell -File tools\flash.ps1 -App general -Action build
#>
param(
    [string]$Port = "",
    [ValidateSet("", "build", "flash", "flash-only", "monitor", "info", "backup", "clock")]
    [string]$Action = "",
    [string]$App = ""
)

# ESP-IDF v6.1 on C: or D:, installed by EIM or by hand in <drive>:\esp\v6.1 with its tools in
# <drive>:\esp\tools61 (docs\SETUP.md section 0).
# Build output and flash backups go under <drive>:\esp on the same drive.
$IdfDrive = "C:"
$IdfProfile = ""
$IdfExport = ""
foreach ($d in @("C:", "D:")) {
    if (Test-Path "$d\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1") {
        $IdfDrive = $d
        $IdfProfile = "$d\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
        break
    }
    if ((Test-Path "$d\esp\v6.1\esp-idf\export.ps1") -and (Test-Path "$d\esp\tools61")) {
        $IdfDrive = $d
        $IdfExport = "$d\esp\v6.1\esp-idf\export.ps1"
        break
    }
}
$BackupRoot = "$IdfDrive\esp\backup"
$FlashBytes = 16MB
$FirmwareDir = Split-Path -Parent $PSScriptRoot
$AppsDir = Join-Path $FirmwareDir "apps"
# Set by Select-App: the app's project folder, and its build folder out of tree (short paths, and
# no build output in a synced folder)
$script:ProjectDir = ""
$script:BuildDir = ""
$UsbSerialJtag = "USB\VID_303A&PID_1001*" # Espressif USB-Serial-JTAG = the USB2 "Full Speed" port

$script:Py = ""
$script:IdfPy = ""
$script:Ok = $false

function Write-Step([string]$Text) { Write-Host ""; Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-Done([string]$Text) { Write-Host $Text -ForegroundColor Green }
function Write-Note([string]$Text) { Write-Host $Text -ForegroundColor Yellow }
function Write-Fail([string]$Text) { Write-Host $Text -ForegroundColor Red }
function Read-Answer([string]$Prompt) { return "$(Read-Host $Prompt)".Trim() }

# ---------------------------------------------------------------------------------------------
# Helpers that return values (no external tool output reaches their pipeline)

function Initialize-Idf {
    if ($IdfProfile -eq "" -and $IdfExport -eq "") {
        Write-Fail "Không tìm thấy ESP-IDF v6.1 ở ổ C: hoặc D:. Xem docs\SETUP.md mục 0."
        return $false
    }
    Write-Step "Nạp môi trường ESP-IDF v6.1"
    if ($IdfProfile -ne "") {
        . $IdfProfile *> $null
    } else {
        $env:PATH = "$IdfDrive\esp\python312;" + $env:PATH
        $env:IDF_TOOLS_PATH = "$IdfDrive\esp\tools61"
        . $IdfExport *> $null
    }
    $script:Py = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
    $script:IdfPy = Join-Path $env:IDF_PATH "tools\idf.py"
    foreach ($f in @($script:Py, $script:IdfPy)) {
        if (-not (Test-Path $f)) {
            Write-Fail "Thiếu $f"
            return $false
        }
    }
    return $true
}

function Find-BoardPort {
    $ports = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like $UsbSerialJtag -and $_.Name -match '\(COM\d+\)' } |
        ForEach-Object { [regex]::Match($_.Name, 'COM\d+').Value } |
        Sort-Object -Unique)
    if ($ports.Count -eq 1) {
        Write-Done "Tìm thấy board ở $($ports[0])"
        return $ports[0]
    }
    if ($ports.Count -gt 1) {
        Write-Note "Có nhiều thiết bị ESP USB-Serial-JTAG: $($ports -join ', ')"
        return (Read-Answer "Nhập cổng muốn dùng")
    }
    $all = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique)
    Write-Note "Không thấy board. Kiểm tra: cáp có dây dữ liệu, cắm vào cổng USB2 'Full Speed', board có nguồn."
    if ($all.Count -gt 0) {
        Write-Host "Các cổng COM đang có: $($all -join ', ')"
    }
    return (Read-Answer "Nhập cổng COM (vd COM5), hoặc Enter để huỷ")
}

function Get-Port {
    if ($script:Port -eq "") {
        $script:Port = "$(Find-BoardPort)".ToUpper()
    }
    if ($script:Port -notmatch '^COM\d+$') {
        $script:Port = ""
        Write-Note "Chưa chọn cổng COM."
        return ""
    }
    return $script:Port
}

function Get-ChipInfo([string]$P) {
    Write-Step "Đọc thông tin chip trên $P"
    $lines = @(& $script:Py -m esptool --chip esp32p4 -p $P flash-id 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $lines | ForEach-Object { Write-Host "    $_" }
    if ($code -ne 0) {
        Write-Fail "esptool không kết nối được với $P."
        Write-Note "Đóng mọi cửa sổ monitor đang mở cổng này. Nếu vẫn lỗi: giữ BOOT, nhấn rồi nhả RESET, nhả BOOT, rồi thử lại."
        return $null
    }
    $chip = [pscustomobject]@{ Major = -1; Minor = -1; FlashSize = ""; Mac = ""; Text = $lines }
    foreach ($l in $lines) {
        if ($l -match 'revision v(\d+)\.(\d+)') { $chip.Major = [int]$Matches[1]; $chip.Minor = [int]$Matches[2] }
        if ($l -match 'Detected flash size:\s*(\S+)') { $chip.FlashSize = $Matches[1] }
        if ($l -match '^MAC:\s*([0-9a-fA-F:]+)') { $chip.Mac = $Matches[1].ToLower() }
    }
    return $chip
}

# The apps: folders of firmware\apps with a CMakeLists.txt (an ESP-IDF project each).
function Get-Apps {
    return @(Get-ChildItem -Path $AppsDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "CMakeLists.txt") } |
        ForEach-Object { $_.Name } | Sort-Object)
}

# Picks the app ($Name, the only one, or asks) and sets the project and build folders.
function Select-App([string]$Name) {
    $apps = @(Get-Apps) # @(): a single app would come back as a string
    if ($apps.Count -eq 0) {
        Write-Fail "Không có app nào trong $AppsDir"
        return $false
    }
    if ($Name -eq "" -and $apps.Count -eq 1) { $Name = $apps[0] }
    while ($apps -notcontains $Name) {
        if ($Name -ne "") { Write-Note "Không có app '$Name'." }
        Write-Host "Các app: $($apps -join ', ')"
        $Name = Read-Answer "Chọn app"
        if ($Name -eq "") { return $false }
    }
    $script:App = $Name
    $script:ProjectDir = Join-Path $AppsDir $Name
    $script:BuildDir = "$IdfDrive\esp\build\ehmi_$Name"
    return $true
}

# "v1" when the build selects P4 revision < v3.0 (CONFIG_ESP32P4_SELECTS_REV_LESS_V3), else "v3".
# The build's own sdkconfig decides; before the first build, the app's defaults and then the shared
# ones, whichever mentions the option first.
function Get-BuildRevision {
    $files = @((Join-Path $BuildDir "sdkconfig"), (Join-Path $ProjectDir "sdkconfig.defaults"),
        (Join-Path $FirmwareDir "sdkconfig.defaults"))
    foreach ($f in $files) {
        if (-not (Test-Path $f)) { continue }
        if (Select-String -Path $f -Pattern '^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y' -Quiet) { return "v1" }
        if (Select-String -Path $f -Pattern 'CONFIG_ESP32P4_SELECTS_REV_LESS_V3' -Quiet) { return "v3" }
    }
    return "v3"
}

function Test-ChipMatchesBuild($chip) {
    $build = Get-BuildRevision
    if ($chip.Major -lt 0) {
        Write-Note "Không đọc được chip revision từ esptool."
        return ((Read-Answer "Vẫn tiếp tục? (y/N)") -eq "y")
    }
    $family = "v1"
    if ($chip.Major -ge 3) { $family = "v3" }
    if ($family -ne $build) {
        Write-Fail "Chip là revision v$($chip.Major).$($chip.Minor) nhưng firmware được build cho $build.x."
        Write-Fail "Image v1.x và v3.x không dùng chung được. Xem docs\SETUP.md mục 4. Chưa ghi gì vào board."
        return $false
    }
    Write-Done "Chip revision v$($chip.Major).$($chip.Minor) khớp với firmware (build cho $build.x)."
    if ($chip.FlashSize -ne "" -and $chip.FlashSize -ne "16MB") {
        Write-Note "Flash báo $($chip.FlashSize), kỳ vọng 16MB."
    }
    return $true
}

function Get-FactoryBackupPath($chip) {
    $id = "unknown"
    if ($chip.Mac -ne "") { $id = $chip.Mac -replace ':', '' }
    return (Join-Path (Join-Path $BackupRoot $id) "factory_full_16MB.bin")
}

function Test-Backup([string]$File) {
    return ((Test-Path $File) -and (Get-Item $File).Length -eq $FlashBytes)
}

# Returns "have", "backup", "skip" or "cancel".
function Read-BackupDecision([string]$Factory) {
    if (Test-Backup $Factory) {
        Write-Done "Đã có bản sao lưu gốc của board này: $Factory"
        return "have"
    }
    Write-Note "Chưa có bản sao lưu flash gốc của board này."
    Write-Host "  [Y] Sao lưu ngay (khuyên dùng)"
    Write-Host "  [S] Bỏ qua và nạp luôn: firmware gốc của hãng sẽ mất"
    Write-Host "  [N] Huỷ"
    $a = (Read-Answer "Chọn [Y/S/N]").ToUpper()
    if ($a -eq "" -or $a -eq "Y") { return "backup" }
    if ($a -eq "S") { return "skip" }
    return "cancel"
}

# ---------------------------------------------------------------------------------------------
# Actions: call as statements, check $script:Ok afterwards

function Invoke-Backup([string]$P, $chip, [string]$File) {
    $script:Ok = $false
    New-Item -ItemType Directory -Force (Split-Path -Parent $File) | Out-Null
    Write-Step "Sao lưu toàn bộ flash 16 MB vào $File (mất vài phút)"
    & $script:Py -m esptool --chip esp32p4 -p $P -b 921600 read-flash 0 0x1000000 $File
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $File)) {
        Write-Fail "Sao lưu thất bại."
        Remove-Item $File -ErrorAction SilentlyContinue
        return
    }
    $len = (Get-Item $File).Length
    if ($len -ne $FlashBytes) {
        Write-Fail "File sao lưu dài $len byte, không phải 16 MB."
        return
    }
    $hash = (Get-FileHash $File -Algorithm SHA256).Hash
    $note = @("Backup time: $(Get-Date -Format s)", "Port: $P", "SHA256: $hash", "") + $chip.Text
    Set-Content -Path ([IO.Path]::ChangeExtension($File, ".txt")) -Value $note -Encoding UTF8
    Write-Done "Đã sao lưu. Nên chép thư mục $(Split-Path -Parent $File) sang ổ khác hoặc cloud."
    $script:Ok = $true
}

function Invoke-Build {
    $script:Ok = $false
    Write-Step "Build app $App vào $BuildDir"
    Push-Location $ProjectDir
    & $script:Py $script:IdfPy -B $BuildDir build
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) {
        Write-Fail "Build lỗi."
        return
    }
    Write-Done "Build xong."
    $script:Ok = $true
}

function Invoke-Flash {
    $script:Ok = $false
    if (-not (Test-Path (Join-Path $BuildDir "flash_args"))) {
        Write-Fail "Chưa có bản build trong $BuildDir. Chọn Build trước."
        return
    }
    $p = Get-Port
    if ($p -eq "") { return }
    $chip = Get-ChipInfo $p
    if ($null -eq $chip) { return }
    if (-not (Test-ChipMatchesBuild $chip)) { return }

    $factory = Get-FactoryBackupPath $chip
    $decision = Read-BackupDecision $factory
    if ($decision -eq "cancel") {
        Write-Note "Đã huỷ. Chưa ghi gì vào board."
        return
    }
    if ($decision -eq "backup") {
        Invoke-Backup $p $chip $factory
        if (-not $script:Ok) {
            Write-Note "Không nạp vì chưa sao lưu được."
            return
        }
        $script:Ok = $false
    }

    Write-Step "Nạp firmware qua $p"
    Push-Location $BuildDir
    & $script:Py -m esptool --chip esp32p4 -p $p -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) {
        Write-Fail "Nạp lỗi."
        return
    }
    Write-Done "Nạp xong."
    $script:Ok = $true
}

# Sends the PC's time to the firmware console ('clock'), which also writes the RTC. The script
# waits for the board to come back after a flash.
function Invoke-SetClock {
    $script:Ok = $false
    $p = Get-Port
    if ($p -eq "") { return }
    Write-Step "Đặt đồng hồ của board (và RTC) theo giờ máy tính"
    & $script:Py (Join-Path $PSScriptRoot "set_clock.py") $p
    if ($LASTEXITCODE -ne 0) {
        Write-Note "Chưa đặt được giờ. Có thể đặt lại bằng mục 7, hoặc trên board ở Settings > Time."
        return
    }
    $script:Ok = $true
}

function Invoke-Monitor {
    $script:Ok = $false
    $p = Get-Port
    if ($p -eq "") { return }
    Write-Step "Monitor $p. Thoát bằng Ctrl+]. Gõ lệnh ở dấu nhắc ecat> (panel, topo, scan...)"
    Push-Location $ProjectDir
    & $script:Py $script:IdfPy -B $BuildDir -p $p monitor
    Pop-Location
    $script:Ok = $true
}

function Invoke-Info {
    $script:Ok = $false
    $p = Get-Port
    if ($p -eq "") { return }
    $chip = Get-ChipInfo $p
    if ($null -eq $chip) { return }
    $match = Test-ChipMatchesBuild $chip
    $factory = Get-FactoryBackupPath $chip
    if (Test-Backup $factory) {
        Write-Done "Bản sao lưu gốc: $factory"
    } else {
        Write-Note "Board này chưa có bản sao lưu gốc."
    }
    $script:Ok = $match
}

function Invoke-ManualBackup {
    $script:Ok = $false
    $p = Get-Port
    if ($p -eq "") { return }
    $chip = Get-ChipInfo $p
    if ($null -eq $chip) { return }
    $file = Get-FactoryBackupPath $chip
    if (Test-Path $file) {
        $file = Join-Path (Split-Path -Parent $file) ("full_16MB_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".bin")
    }
    Invoke-Backup $p $chip $file
}

function Invoke-Action([string]$Name) {
    switch ($Name) {
        "build" { Invoke-Build }
        "flash" {
            Invoke-Build
            if ($script:Ok) { Invoke-Flash }
            if ($script:Ok) { Invoke-SetClock; Invoke-Monitor } # a clock failure only warns
        }
        "flash-only" {
            Invoke-Flash
            if ($script:Ok) { Invoke-SetClock; Invoke-Monitor }
        }
        "monitor" { Invoke-Monitor }
        "info" { Invoke-Info }
        "backup" { Invoke-ManualBackup }
        "clock" { Invoke-SetClock }
    }
}

function Show-Menu {
    $built = "chưa có"
    $fa = Join-Path $BuildDir "flash_args"
    if (Test-Path $fa) { $built = (Get-Item $fa).LastWriteTime.ToString("yyyy-MM-dd HH:mm") }
    $portText = $script:Port
    if ($portText -eq "") { $portText = "tự tìm" }
    Write-Host ""
    Write-Host "EthercatHMI - nạp firmware JC1060P470C (ESP32-P4)" -ForegroundColor Cyan
    Write-Host "App: $App    Cổng: $portText    Bản build: $built"
    Write-Host "  1  Build + nạp + monitor"
    Write-Host "  2  Nạp bản đã build (không build lại) + monitor"
    Write-Host "  3  Chỉ monitor"
    Write-Host "  4  Chỉ build"
    Write-Host "  5  Thông tin chip (revision, flash, MAC, bản sao lưu)"
    Write-Host "  6  Sao lưu toàn bộ flash 16 MB"
    Write-Host "  7  Đặt giờ board (và RTC) theo giờ máy tính"
    if (@(Get-Apps).Count -gt 1) { Write-Host "  8  Đổi app" }
    Write-Host "  0  Thoát"
}

# ---------------------------------------------------------------------------------------------

$script:Port = $Port.ToUpper()
if (-not (Initialize-Idf)) { exit 1 }
if (-not (Select-App $App)) { exit 1 }

if ($Action -ne "") {
    Invoke-Action $Action
    if ($script:Ok) { exit 0 }
    exit 1
}

$menu = @{ "1" = "flash"; "2" = "flash-only"; "3" = "monitor"; "4" = "build"; "5" = "info"; "6" = "backup"; "7" = "clock" }
while ($true) {
    Show-Menu
    $choice = Read-Answer "Chọn"
    if ($choice -eq "0" -or $choice -eq "") { break }
    if ($choice -eq "8" -and @(Get-Apps).Count -gt 1) {
        Write-Host "Các app: $(@(Get-Apps) -join ', ')"
        $name = Read-Answer "Chọn app (Enter = giữ $App)"
        if ($name -ne "") { [void](Select-App $name) }
    } elseif ($menu.ContainsKey($choice)) {
        Invoke-Action $menu[$choice]
    } else {
        Write-Note "Không có lựa chọn '$choice'."
    }
}
exit 0
