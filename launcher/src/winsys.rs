//! Windows-only system calls: processes, free disk space, the console, message boxes, the Steam and prismlauncher://
//! registry entries and the Ctrl-C/close handler. The Linux counterparts live next to the callers (util.rs, prism.rs,
//! autoexec.rs) behind cfg(unix).
use crate::util::Proc;
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;
use winreg::enums::*;
use winreg::RegKey;

const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const DETACHED_PROCESS: u32 = 0x0000_0008;
const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;

fn wide(s: &OsStr) -> Vec<u16> { s.encode_wide().chain(std::iter::once(0)).collect() }

/// A console program started from this GUI program must not flash a console window.
pub fn hidden(c: &mut Command) -> &mut Command { c.creation_flags(CREATE_NO_WINDOW) }

/// For Prism: its own process, not tied to this launcher's console or Ctrl-C.
pub fn detached(c: &mut Command) -> &mut Command { c.creation_flags(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP) }

// ---------------------------------------------------------------- time

pub fn local_time() -> (i32, u32, u32, u32, u32, u32) {
    use windows_sys::Win32::System::SystemInformation::GetLocalTime;
    let mut t: windows_sys::Win32::Foundation::SYSTEMTIME = unsafe { std::mem::zeroed() };
    unsafe { GetLocalTime(&mut t) };
    (t.wYear as i32, t.wMonth as u32, t.wDay as u32, t.wHour as u32, t.wMinute as u32, t.wSecond as u32)
}

// ---------------------------------------------------------------- disk

pub fn free_bytes(p: &Path) -> Option<u64> {
    use windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW;
    // the data folder may not exist yet: ask about the nearest folder that does
    let mut q = p;
    while !q.exists() { q = q.parent()?; }
    let w = wide(q.as_os_str());
    let mut avail = 0u64;
    let (mut total, mut free) = (0u64, 0u64);
    (unsafe { GetDiskFreeSpaceExW(w.as_ptr(), &mut avail, &mut total, &mut free) } != 0).then_some(avail)
}

// ---------------------------------------------------------------- processes

fn refresh(cpu: bool) -> sysinfo::System {
    use sysinfo::{ProcessRefreshKind, ProcessesToUpdate, UpdateKind};
    let mut sys = sysinfo::System::new();
    let mut kind = ProcessRefreshKind::nothing().with_cmd(UpdateKind::Always).with_exe(UpdateKind::Always);
    if cpu { kind = kind.with_cpu(); }
    sys.refresh_processes_specifics(ProcessesToUpdate::All, true, kind);
    sys
}

pub fn list_procs() -> Vec<Proc> {
    refresh(false).processes().values().map(|p| Proc {
        pid: p.pid().as_u32(),
        comm: p.name().to_string_lossy().to_string(),
        cmdline: p.cmd().iter().map(|a| a.to_string_lossy().to_string()).collect(),
        starttime: p.start_time(),
        exe: p.exe().map(|e| e.to_path_buf()),
    }).collect()
}

pub fn proc_starttime(pid: u32) -> Option<u64> { list_procs().into_iter().find(|p| p.pid == pid).map(|p| p.starttime) }

pub fn pid_alive(pid: u32) -> bool {
    use windows_sys::Win32::Foundation::CloseHandle;
    use windows_sys::Win32::System::Threading::{GetExitCodeProcess, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION};
    unsafe {
        let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() {
            // access denied still means "there is such a process"
            return std::io::Error::last_os_error().raw_os_error() == Some(5);
        }
        let mut code = 0u32;
        let ok = GetExitCodeProcess(h, &mut code) != 0;
        CloseHandle(h);
        ok && code == 259 // STILL_ACTIVE
    }
}

/// Total processor time (milliseconds) the process has used so far, to tell a working Minecraft from a frozen one.
pub fn cpu_ms(pid: u32) -> u64 {
    refresh(true).processes().values().find(|p| p.pid().as_u32() == pid).map(|p| p.accumulated_cpu_time()).unwrap_or(0)
}

/// Asks the process to close (WM_CLOSE through taskkill), waits `wait_s`, then ends it. Callers only pass a pid that
/// they have checked against the start time they recorded, so this never touches a process the launcher did not start.
pub fn stop_pid(pid: u32, wait_s: u64) -> bool {
    let _ = hidden(Command::new("taskkill").args(["/PID", &pid.to_string()])).stdin(std::process::Stdio::null()).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null()).status();
    for _ in 0..wait_s {
        if !pid_alive(pid) { return true; }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
    let sys = refresh(false);
    if let Some(p) = sys.process(sysinfo::Pid::from_u32(pid)) { p.kill(); }
    std::thread::sleep(std::time::Duration::from_secs(2));
    !pid_alive(pid)
}

// ---------------------------------------------------------------- windows and console

pub fn message_box(title: &str, text: &str) {
    use windows_sys::Win32::UI::WindowsAndMessaging::{MessageBoxW, MB_ICONINFORMATION, MB_OK};
    let (t, c) = (wide(OsStr::new(title)), wide(OsStr::new(text)));
    unsafe { MessageBoxW(std::ptr::null_mut(), c.as_ptr(), t.as_ptr(), MB_OK | MB_ICONINFORMATION) };
}

/// The release program has no console of its own (`windows_subsystem = "windows"`), so --cli/--help would print
/// into nothing. Attach to the console of the program that started us (cmd.exe, PowerShell, Windows Terminal) and, if
/// output is not redirected, point stdout/stderr at it. Returns true when there is a console to print to.
pub fn attach_parent_console() -> bool {
    use std::os::windows::io::IntoRawHandle;
    use windows_sys::Win32::System::Console::{AttachConsole, GetStdHandle, SetStdHandle, ATTACH_PARENT_PROCESS, STD_ERROR_HANDLE, STD_OUTPUT_HANDLE};
    unsafe {
        let attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
        if !attached && std::io::Error::last_os_error().raw_os_error() != Some(5) {
            // no parent console (started by double-click) and no console of ours either
            return false;
        }
        for std_id in [STD_OUTPUT_HANDLE, STD_ERROR_HANDLE] {
            let cur = GetStdHandle(std_id);
            if cur.is_null() || cur as isize == -1 {
                if let Ok(f) = std::fs::OpenOptions::new().write(true).open("CONOUT$") {
                    SetStdHandle(std_id, f.into_raw_handle());
                }
            }
        }
    }
    true
}

// ---------------------------------------------------------------- Steam (registry)

/// Where the player's Steam client says it lives. HKCU\Software\Valve\Steam "SteamPath" is the one Steam itself keeps
/// current (forward slashes, e.g. "c:/program files (x86)/steam"); the machine-wide key and the default folder are
/// fallbacks. Order = priority; duplicates are removed by the caller.
pub fn steam_install_dirs() -> Vec<String> {
    let mut v = Vec::new();
    if let Ok(k) = RegKey::predef(HKEY_CURRENT_USER).open_subkey(r"Software\Valve\Steam") {
        if let Ok(s) = k.get_value::<String, _>("SteamPath") { v.push(s); }
    }
    for sub in [r"SOFTWARE\WOW6432Node\Valve\Steam", r"SOFTWARE\Valve\Steam"] {
        if let Ok(k) = RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey(sub) {
            if let Ok(s) = k.get_value::<String, _>("InstallPath") { v.push(s); }
        }
    }
    v.push(r"C:\Program Files (x86)\Steam".into());
    v
}

/// The steam.exe the registry names, if it is there.
pub fn steam_exe() -> Option<PathBuf> {
    if let Ok(k) = RegKey::predef(HKEY_CURRENT_USER).open_subkey(r"Software\Valve\Steam") {
        if let Ok(s) = k.get_value::<String, _>("SteamExe") {
            let p = PathBuf::from(s.replace('/', "\\"));
            if p.is_file() { return Some(p); }
        }
    }
    steam_install_dirs().into_iter().map(|d| PathBuf::from(d.replace('/', "\\")).join("steam.exe")).find(|p| p.is_file())
}

// ---------------------------------------------------------------- prismlauncher:// (registry)

const URL_KEY: &str = r"Software\Classes\prismlauncher";

/// What HKCU\Software\Classes\prismlauncher held before we borrowed it, as text to be saved in state.json.
/// "win:" + JSON {"existed": bool, "root": str?, "proto": str?, "cmd": str?}.
fn snapshot() -> String {
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let Ok(k) = hkcu.open_subkey(URL_KEY) else { return r#"win:{"existed":false}"#.into() };
    let cmd = k.open_subkey(r"shell\open\command").ok().and_then(|c| c.get_value::<String, _>("").ok());
    let v = serde_json::json!({
        "existed": true,
        "root": k.get_value::<String, _>("").ok(),
        "proto": k.get_value::<String, _>("URL Protocol").ok(),
        "cmd": cmd,
    });
    format!("win:{v}")
}

pub fn url_handler_command(prism_exe: &str) -> String { format!("\"{prism_exe}\" \"%1\"") }

/// Points prismlauncher:// at our Prism. Returns the previous state to restore, or None when it was already ours / failed.
pub fn claim_url_handler(prism_exe: &str) -> Result<Option<String>, String> {
    let want = url_handler_command(prism_exe);
    let prev = snapshot();
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let already = hkcu.open_subkey(format!(r"{URL_KEY}\shell\open\command")).ok().and_then(|c| c.get_value::<String, _>("").ok()).map(|c| c == want).unwrap_or(false);
    if already { return Ok(None); }
    let (k, _) = hkcu.create_subkey(URL_KEY).map_err(|e| e.to_string())?;
    k.set_value("", &"URL:Prism Launcher (Portalcraft)").map_err(|e| e.to_string())?;
    k.set_value("URL Protocol", &"").map_err(|e| e.to_string())?;
    let (c, _) = k.create_subkey(r"shell\open\command").map_err(|e| e.to_string())?;
    c.set_value("", &want).map_err(|e| e.to_string())?;
    Ok(Some(prev))
}

/// Puts back what `claim_url_handler` replaced. `prev` is the text it returned.
pub fn restore_url_handler(prev: &str) {
    let Some(json) = prev.strip_prefix("win:") else { return };
    let Ok(v) = serde_json::from_str::<serde_json::Value>(json) else { return };
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    if v["existed"].as_bool() != Some(true) {
        let _ = hkcu.delete_subkey_all(URL_KEY);
        return;
    }
    let Ok((k, _)) = hkcu.create_subkey(URL_KEY) else { return };
    let put = |key: &RegKey, name: &str, val: &serde_json::Value| match val.as_str() {
        Some(s) => { let _ = key.set_value(name, &s); }
        None => { let _ = key.delete_value(name); }
    };
    put(&k, "", &v["root"]);
    put(&k, "URL Protocol", &v["proto"]);
    if let Ok((c, _)) = k.create_subkey(r"shell\open\command") { put(&c, "", &v["cmd"]); }
}

// ---------------------------------------------------------------- Ctrl-C / console close

static CLEANUP: Mutex<Option<(PathBuf, PathBuf, bool)>> = Mutex::new(None);

unsafe extern "system" fn on_console_event(_kind: u32) -> i32 {
    // runs on a thread Windows creates for the event: ordinary file calls are fine here
    if let Ok(g) = CLEANUP.lock() {
        if let Some((autoexec, saved, had_saved)) = g.as_ref() {
            let _ = std::fs::remove_file(autoexec);
            if *had_saved { let _ = std::fs::rename(saved, autoexec); }
        }
    }
    0 // not handled: the default handler then ends the program
}

/// The temporary autoexec.cfg is removed (and the player's own put back) when a console launcher gets Ctrl-C, Ctrl-Break
/// or its console window is closed. A window launcher is covered by on_exit instead.
pub fn arm_cleanup(autoexec: PathBuf, saved: PathBuf, had_saved: bool) {
    use windows_sys::Win32::System::Console::SetConsoleCtrlHandler;
    *CLEANUP.lock().unwrap() = Some((autoexec, saved, had_saved));
    unsafe { SetConsoleCtrlHandler(Some(on_console_event), 1) };
}

pub fn disarm_cleanup() { *CLEANUP.lock().unwrap() = None; }
