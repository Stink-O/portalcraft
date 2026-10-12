# Native-Windows twin of run_game_proton.sh: starts the developer Portal 2 copy with the plugin, optionally runs a pcb_script,
# collects the logs and pcb_* screenshots. Developer tool only; players start the game through the launcher.
#   powershell -File tools\run_game_win.ps1 [-Script <file>|-] [-Map <map>|-] [-Watchdog 150] [-Sub run] [-NoPlugin] [-Hold 60]
#   -Script -      no pcb_script line; the run ends after -Hold seconds (0 = when the player quits the game)
#   -NoPlugin      vanilla run (no plugin_load line)
# Environment passed on to the plugin: PCB_WIN_UNVERIFIED, PCB_SLOTS, PCB_KEEP_ACTIVE (default 0: with 1 the game keeps reading the mouse while
# another window has focus, so a user working beside the run spins the view; a scripted run then needs its window kept in front),
# PCB_GAME_ARGS (extra engine args), PCB_DLL (default bridge\build\win\portalcraft_bridge.dll), PCB_WINGAME (default D:\Portal2-Windows).
# Writes <copy>\portal2\cfg\autoexec.cfg (refuses if one exists), removes it once the plugin reported "[bridge] loaded" or on exit.
# Stops ONLY the portal2.exe this run started (by pid); refuses to start while any Portal 2 runs.
# Output: cache\bridge\shots\<Sub>\{bridge.log,console.log} and the run's pcb_* screenshots.
param([string]$Script = '-', [string]$Map = '-', [int]$Watchdog = 150, [string]$Sub = 'run', [switch]$NoPlugin, [int]$Hold = 60)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Game = if ($env:PCB_WINGAME) { $env:PCB_WINGAME } else { 'D:\Portal2-Windows' }
$P2 = Join-Path $Game 'portal2'
$Auto = Join-Path $P2 'cfg\autoexec.cfg'
$Log = Join-Path $Root 'cache\bridge\bridge.log'
$Dll = if ($env:PCB_DLL) { $env:PCB_DLL } else { Join-Path $Root 'bridge\build\win\portalcraft_bridge.dll' }
$Out = Join-Path $Root "cache\bridge\shots\$Sub"

if (-not (Test-Path "$Game\portal2.exe")) { Write-Error "no Windows Portal 2 at $Game"; exit 3 }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { Write-Error 'Steam is not running'; exit 3 }
if (Get-Process portal2 -ErrorAction SilentlyContinue) { Write-Error 'a Portal 2 is already running; not launching'; exit 2 }
if (Test-Path $Auto) { Write-Error "$Auto exists (someone else's run?); refusing"; exit 2 }
if (-not $NoPlugin -and -not (Test-Path $Dll)) { Write-Error "no plugin dll at $Dll"; exit 3 }
if ($Script -ne '-') { $Script = (Resolve-Path $Script).Path }
New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null

function LogTail { if (Test-Path $Log) { Get-Content $Log -Encoding UTF8 | Select-Object -Skip $script:LogStart } }
$script:LogStart = if (Test-Path $Log) { (Get-Content $Log | Measure-Object -Line).Lines } else { 0 }
Remove-Item "$P2\console.log" -ErrorAction SilentlyContinue
$shotsBefore = @(Get-ChildItem "$P2\screenshots\pcb_*" -ErrorAction SilentlyContinue | ForEach-Object Name)

$lines = @()
if (-not $NoPlugin) { $lines += "plugin_load `"$($Dll -replace '\.dll$','' -replace '\\','/')`"" }
# (2026-10-12) a test has no Minecraft linked: started as the Minecraft player, the camera followed a Minecraft that is not there and the
# view spun for the whole run. Chell unless PCB_START_MODE=mc (a test that starts its own Minecraft).
if ($Script -ne '-' -or $Map -ne '-') { $lines += "pc_start_mode $(if ($env:PCB_START_MODE) { $env:PCB_START_MODE } else { 'chell' })" }
if ($env:PCB_PRE) { $lines += $env:PCB_PRE -split ';' }
if ($Script -ne '-') { $lines += "pcb_script `"$($Script -replace '\\','/')`" $Watchdog" }
if ($Map -ne '-') { $lines += "map $Map" }
$myAuto = ($lines + "// run $PID") -join "`n"
[IO.File]::WriteAllText($Auto, "$myAuto`n")
function RemoveOwnAuto { if ((Test-Path $Auto) -and ((Get-Content $Auto -Raw).TrimEnd() -eq $myAuto)) { Remove-Item $Auto } }

# Test scripts change archived settings (crosshair 0 for clean shots ...) and Portal 2 saves them into config.cfg when it quits: the
# player's copy then had no crosshair (2026-10-12). The file is put back exactly as it was once our game has exited.
$Cfg = Join-Path $P2 'cfg\config.cfg'
$cfgSaved = if (Test-Path $Cfg) { [IO.File]::ReadAllBytes($Cfg) } else { $null }

$interactive = ($Script -eq '-' -and $Hold -eq 0)
if (-not $env:PCB_KEEP_ACTIVE) { $env:PCB_KEEP_ACTIVE = '0' }
$env:PORTALCRAFT_ROOT = $Root
$gameArgs = "-game portal2 -steam -novid -condebug $env:PCB_GAME_ARGS"
$proc = $null
try {
	Write-Host "[run] launching Windows Portal 2 ($(Get-Date -Format T)): $gameArgs"
	$proc = Start-Process "$Game\portal2.exe" -ArgumentList $gameArgs -WorkingDirectory $Game -PassThru
	Write-Host "[run] pid $($proc.Id)"
	if (-not $NoPlugin) {
		for ($i = 0; $i -lt 90 -and -not $proc.HasExited; $i++) { if (LogTail | Select-String -Quiet '\[bridge\] loaded') { break }; Start-Sleep 1 }
	} else { Start-Sleep 20 }
	RemoveOwnAuto; Write-Host '[run] autoexec removed'
	if (-not $NoPlugin -and -not (LogTail | Select-String -Quiet '\[bridge\] loaded')) { throw 'the plugin never loaded' }
	$t = 0; $ended = -1
	while (-not $proc.HasExited) {
		Start-Sleep 2; $t += 2
		if ($ended -lt 0 -and (LogTail | Select-String -Quiet '\[bridge\] (script: done|unloading)|^CRASH signal|EXCEPTION 0x')) { $ended = $t }
		if ($ended -ge 0 -and $t - $ended -ge 20) { Write-Host '[run] no exit 20 s after the script ended or crashed'; break }
		if ($Script -eq '-' -and $Hold -gt 0 -and $t -ge $Hold) { Write-Host '[run] hold time over'; break }
		if (-not $interactive -and $t -ge $Watchdog + 60) { Write-Host "[run] still running after ${t}s (watchdog ${Watchdog}s)"; break }
	}
	Write-Host "[run] game ran ~${t}s"
} finally {
	RemoveOwnAuto
	if ($proc -and -not $proc.HasExited) {
		Write-Host "[run] stopping our portal2.exe (pid $($proc.Id))"
		$proc.CloseMainWindow() | Out-Null
		if (-not $proc.WaitForExit(10000)) { Stop-Process -Id $proc.Id -Force }
	}
	if ($proc) { [void]$proc.WaitForExit(15000) }
	if ($cfgSaved -and -not $interactive) { [IO.File]::WriteAllBytes($Cfg, $cfgSaved); Write-Host '[run] config.cfg restored' }
	New-Item -ItemType Directory -Force $Out | Out-Null
	LogTail | Set-Content (Join-Path $Out 'bridge.log') -Encoding UTF8
	if (Test-Path "$P2\console.log") { Copy-Item "$P2\console.log" $Out }
	Get-ChildItem "$P2\screenshots\pcb_*" -ErrorAction SilentlyContinue | Where-Object { $shotsBefore -notcontains $_.Name } | ForEach-Object {
		Move-Item $_.FullName $Out -Force; Write-Host "[run] shot $Out\$($_.Name)"
	}
	Write-Host "[run] logs in $Out"
}
