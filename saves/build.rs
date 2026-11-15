fn main() {
    println!("cargo:rerun-if-env-changed=GAMEGAUGE_VERSION");
    println!("cargo:rerun-if-changed=../VERSION");
    let version = std::env::var("GAMEGAUGE_VERSION")
        .unwrap_or_else(|_| include_str!("../VERSION").trim().to_owned());
    let version_number = version
        .split('.')
        .enumerate()
        .map(|(index, part)| u64::from(part.parse::<u16>().unwrap()) << ((3 - index) * 16))
        .sum();
    println!("cargo:rustc-env=GAMEGAUGE_SAVES_VERSION={version}");
    winresource::WindowsResource::new()
        .set("ProductName", "GameGauge")
        .set("FileDescription", "GameGauge Saves")
        .set("OriginalFilename", "GameGauge.Saves.exe")
        .set("FileVersion", &version)
        .set("ProductVersion", &version)
        .set_manifest(include_str!("worker.manifest"))
        .set_version_info(winresource::VersionInfo::FILEVERSION, version_number)
        .set_version_info(winresource::VersionInfo::PRODUCTVERSION, version_number)
        .compile()
        .expect("无法编译工作进程的版本资源");
}
