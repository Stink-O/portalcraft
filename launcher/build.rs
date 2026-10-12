// Windows: embed the Portalcraft icon and version information in the .exe (Explorer, the taskbar and shortcuts use the
// icon; Properties > Details, SmartScreen and antivirus heuristics read the version information).
fn main() {
    println!("cargo:rerun-if-changed=assets/branding/icon.ico");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        let mut res = winresource::WindowsResource::new();
        res.set_icon("assets/branding/icon.ico");
        res.set("ProductName", "Portalcraft");
        res.set("FileDescription", "Portalcraft launcher");
        res.set("CompanyName", "Portalcraft contributors");
        res.set("LegalCopyright", "MIT License. https://github.com/Stink-O/portalcraft");
        res.set("OriginalFilename", "Portalcraft.exe");
        res.set("InternalName", "portalcraft-launcher");
        if let Err(e) = res.compile() { println!("cargo:warning=no icon in the .exe: {e}"); }
    }
}
