use std::{
    ffi::OsStr,
    fs,
    path::{Component, Path, PathBuf},
};

use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use winreg::{RegKey, enums::*};

pub const GAME_ID: &str = "steam-4115450";
pub const MAIN_SAVE: &str = "PBZSaveGame.sav";
pub const BACKUP_DIRS: [&str; 3] = ["1RealTime", "2Hourly", "3Daily"];
const SAVE_TREE: &str = r"ProjectPBZ\Saved\SaveGames\Steam";

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CustomGame {
    pub app_id: u32,
    pub title: String,
    pub executable: String,
}

impl CustomGame {
    pub fn find(app_id: u32, executable: &str) -> Result<Self> {
        ensure!(
            app_id > 0 && app_id != 4115450,
            "请输入另一款 Steam 游戏的 App ID。"
        );
        ensure!(
            executable.len() > 4
                && executable.len() <= 150
                && executable.to_ascii_lowercase().ends_with(".exe")
                && !executable
                    .chars()
                    .any(|c| c.is_control() || "\\/:*?\"<>|".contains(c)),
            "请输入游戏进程的文件名，例如 Game.exe。"
        );
        let title = manifest(app_id)
            .map(|(_, title)| title)
            .context("没有在 Steam 游戏库中找到这个 App ID。请先安装游戏。")?;
        Ok(Self {
            app_id,
            title,
            executable: executable.to_lowercase(),
        })
    }

    pub fn key(&self) -> String {
        format!("steam-{}", self.app_id)
    }
}

fn libraries() -> Vec<PathBuf> {
    let mut libraries = Vec::new();
    for (hive, key, value) in [
        (HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
        (
            HKEY_LOCAL_MACHINE,
            r"Software\WOW6432Node\Valve\Steam",
            "InstallPath",
        ),
        (HKEY_LOCAL_MACHINE, r"Software\Valve\Steam", "InstallPath"),
    ] {
        let Ok(key) = RegKey::predef(hive).open_subkey(key) else {
            continue;
        };
        let Ok(path) = key.get_value::<String, _>(value) else {
            continue;
        };
        let steam = PathBuf::from(path);
        if let Ok(vdf) = fs::read_to_string(steam.join(r"steamapps\libraryfolders.vdf")) {
            libraries.extend(
                vdf.lines()
                    .filter_map(|line| vdf_field(line, "path"))
                    .map(PathBuf::from),
            );
        }
        libraries.push(steam);
    }

    libraries.sort();
    libraries.dedup();
    libraries
}

fn manifest(app_id: u32) -> Option<(PathBuf, String)> {
    for library in libraries() {
        let apps = library.join("steamapps");
        let Ok(manifest) = fs::read_to_string(apps.join(format!("appmanifest_{app_id}.acf")))
        else {
            continue;
        };
        let Some(dir) = manifest
            .lines()
            .find_map(|line| vdf_field(line, "installdir"))
        else {
            continue;
        };
        if !matches!(
            Path::new(&dir).components().collect::<Vec<_>>().as_slice(),
            [Component::Normal(_)]
        ) {
            continue;
        }
        let name = manifest.lines().find_map(|line| vdf_field(line, "name"))?;
        return Some((apps.join("common").join(dir), name));
    }
    None
}

pub fn discover() -> Vec<PathBuf> {
    let mut candidates: Vec<_> = manifest(4115450)
        .and_then(|(directory, _)| fs::read_dir(directory.join(SAVE_TREE)).ok())
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| validate_slot(&entry.path()).ok())
        .collect();
    candidates.sort();
    candidates.dedup();
    candidates
}

pub fn resolve_custom(path: &Path, game: &CustomGame) -> Result<PathBuf> {
    ensure!(path.is_absolute(), "请选择游戏专用的存档文件夹。");
    let resolved = resolve_directory(path)?;
    let mut protected: Vec<PathBuf> = [
        "USERPROFILE",
        "SystemRoot",
        "ProgramFiles",
        "ProgramFiles(x86)",
        "ProgramData",
        "APPDATA",
        "LOCALAPPDATA",
    ]
    .into_iter()
    .filter_map(std::env::var_os)
    .map(PathBuf::from)
    .collect();
    protected.extend(libraries());
    if let Some((install, _)) = manifest(game.app_id) {
        protected.push(install);
    }
    let blocked = protected
        .into_iter()
        .filter_map(|p| p.canonicalize().ok())
        .any(|p| crate::storage::within(&p, &resolved));
    ensure!(
        !blocked && resolved.components().count() >= 3,
        "请选择单独的游戏存档目录，不能使用系统、用户或 Steam 游戏库的根目录。"
    );
    if resolved.is_dir() {
        ensure!(
            !resolved.join(&game.executable).exists(),
            "请选择存档文件夹，不能选择游戏程序所在目录。"
        );
    }
    Ok(resolved)
}

fn vdf_field(line: &str, key: &str) -> Option<String> {
    let line = line.trim_start().strip_prefix('"')?;
    let (field, value) = line.split_once('"')?;
    if field != key {
        return None;
    }
    let value = value.trim().strip_prefix('"')?.strip_suffix('"')?;
    Some(value.replace(r"\\", r"\"))
}

pub fn validate_slot(path: &Path) -> Result<PathBuf> {
    let path = resolve_slot(path)?;
    let meta = fs::symlink_metadata(path.join(MAIN_SAVE))
        .context("找不到 PBZSaveGame.sav。请先在游戏内保存一次")?;
    ensure!(
        meta.is_file() && !is_reparse(&meta) && meta.len() > 0,
        "主存档为空或不是普通文件。"
    );
    Ok(path)
}

pub fn resolve_slot(path: &Path) -> Result<PathBuf> {
    ensure!(path.is_absolute(), "请选择绝对存档路径。");
    let slot = path.file_name().and_then(OsStr::to_str).unwrap_or_default();
    ensure!(
        slot.len() == 17 && slot.bytes().all(|byte| byte.is_ascii_digit()),
        "请选择包含 PBZSaveGame.sav 的 17 位 Steam 账号文件夹。"
    );
    resolve_directory(path)
}

pub fn resolve_save(settings: &crate::storage::Settings) -> Result<PathBuf> {
    match &settings.custom {
        Some(game) => resolve_custom(&settings.save_dir, game),
        None => resolve_slot(&settings.save_dir),
    }
}

fn resolve_directory(path: &Path) -> Result<PathBuf> {
    plain_path(path.parent().context("无法定位存档父目录")?)?;
    match fs::symlink_metadata(path) {
        Ok(meta) => {
            ensure!(
                meta.is_dir() && !is_reparse(&meta),
                "存档目录必须是普通文件夹。"
            );
            path.canonicalize().context("无法打开存档目录")
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            let parent = path
                .parent()
                .context("无法定位存档父目录")?
                .canonicalize()?;
            Ok(parent.join(path.file_name().context("没有存档目录名")?))
        }
        Err(error) => Err(error).context("无法读取存档目录"),
    }
}

pub fn plain_path(path: &Path) -> Result<()> {
    for ancestor in path.ancestors().filter(|part| part.has_root()) {
        let metadata = fs::symlink_metadata(ancestor)?;
        ensure!(
            metadata.is_dir() && !is_reparse(&metadata),
            "目录包含重定向路径。"
        );
    }
    Ok(())
}

pub fn is_reparse(meta: &fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    meta.file_attributes() & 0x400 != 0
}

pub fn is_save(name: &Path) -> bool {
    name.extension()
        .and_then(OsStr::to_str)
        .is_some_and(|ext| ext.eq_ignore_ascii_case("sav"))
}

pub fn is_running(game: Option<&CustomGame>) -> Result<bool> {
    Ok(matches_process(game, &processes()?))
}

pub fn matches_process(game: Option<&CustomGame>, processes: &[String]) -> bool {
    processes.iter().any(|name| {
        if let Some(game) = game {
            name == &game.executable
        } else {
            matches!(
                name.as_str(),
                "pbz.exe"
                    | "projectpbz.exe"
                    | "projectpbz-win64-shipping.exe"
                    | "pbz-win64-shipping.exe"
            )
        }
    })
}

pub fn processes() -> Result<Vec<String>> {
    use windows::Win32::{
        Foundation::CloseHandle,
        System::{
            Diagnostics::ToolHelp::*, RemoteDesktop::ProcessIdToSessionId,
            Threading::GetCurrentProcessId,
        },
    };
    let mut own_session = 0;
    unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut own_session)? };
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) }?;
    let mut entry = PROCESSENTRY32W {
        dwSize: size_of::<PROCESSENTRY32W>() as u32,
        ..Default::default()
    };
    let mut names = Vec::new();
    let result = unsafe {
        let mut next = Process32FirstW(snapshot, &mut entry);
        loop {
            if let Err(error) = next {
                break if error.code().0 as u32 == 0x80070012 {
                    Ok(())
                } else {
                    Err(error)
                };
            }
            let mut session = 0;
            if ProcessIdToSessionId(entry.th32ProcessID, &mut session).is_ok()
                && session == own_session
            {
                let len = entry
                    .szExeFile
                    .iter()
                    .position(|&ch| ch == 0)
                    .unwrap_or(entry.szExeFile.len());
                let name = String::from_utf16_lossy(&entry.szExeFile[..len]).to_ascii_lowercase();
                names.push(name);
            }
            next = Process32NextW(snapshot, &mut entry);
        }
    };
    unsafe {
        let _ = CloseHandle(snapshot);
    }
    result?;
    Ok(names)
}
