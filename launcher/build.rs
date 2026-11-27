// Windows only: embeds the logo as the exe icon (Explorer, taskbar, Alt-Tab) and the version info.
fn main() {
    #[cfg(windows)]
    {
        println!("cargo:rerun-if-changed=assets/PersonaRM.ico");
        println!("cargo:rerun-if-env-changed=PRM_VERSION");
        let ver = std::env::var("PRM_VERSION")
            .unwrap_or_else(|_| std::env::var("CARGO_PKG_VERSION").unwrap());
        let ver = ver.trim_start_matches('v').to_string();
        let mut res = winresource::WindowsResource::new();
        res.set_icon("assets/PersonaRM.ico")
            .set("ProductName", "PersonaRM")
            .set("FileDescription", "PersonaRM")
            .set("OriginalFilename", "PersonaRM.exe")
            .set("FileVersion", &ver)
            .set("ProductVersion", &ver);
        if let Err(e) = res.compile() {
            println!("cargo:warning=could not embed the exe icon: {e}");
        }
    }
}
