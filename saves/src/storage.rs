use std::{
    fs,
    io::{BufReader, Write},
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail, ensure};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use tempfile::NamedTempFile;
use uuid::Uuid;

pub const DEFAULT_HOTKEY: &str = "Ctrl+Alt+F8";
pub const CATALOG: &str = "SaveKeep Library";

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub save_dir: PathBuf,
    pub backup_dir: PathBuf,
    pub interval_minutes: u64,
    pub keep_auto: usize,
    pub auto_enabled: bool,
    pub hotkey: String,
    pub dark: bool,
    #[serde(skip)]
    pub custom: Option<crate::game::CustomGame>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            save_dir: PathBuf::new(),
            backup_dir: PathBuf::new(),
            interval_minutes: 10,
            keep_auto: 100,
            auto_enabled: true,
            hotkey: DEFAULT_HOTKEY.into(),
            dark: false,
            custom: None,
        }
    }
}

impl Settings {
    pub fn identity(&self) -> (String, String) {
        if let Some(game) = &self.custom {
            (game.key(), "default".into())
        } else {
            let name = self
                .save_dir
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or_default();
            let profile = if name.len() == 17 && name.bytes().all(|byte| byte.is_ascii_digit()) {
                name
            } else {
                ""
            };
            (crate::game::GAME_ID.into(), profile.into())
        }
    }
}

#[derive(Debug, Copy, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Trigger {
    Manual,
    Scheduled,
    Exit,
    BeforeRestore,
}

impl Trigger {
    pub fn label(self) -> &'static str {
        match self {
            Self::Manual => "手动",
            Self::Scheduled => "定时",
            Self::Exit => "退出游戏",
            Self::BeforeRestore => "恢复前保护",
        }
    }

    pub fn automatic(self) -> bool {
        matches!(self, Self::Scheduled | Self::Exit)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Record {
    pub format: u32,
    pub id: Uuid,
    pub game_id: String,
    pub profile: String,
    pub at: DateTime<Utc>,
    pub trigger: Trigger,
    pub title: String,
    pub note: String,
    pub locked: bool,
    pub bytes: u64,
    pub sha256: String,
    pub content_sha256: String,
}

pub fn app_dir() -> Result<PathBuf> {
    let root = std::env::var_os("LOCALAPPDATA").context("无法找到 Windows 本地应用数据目录")?;
    Ok(PathBuf::from(root).join("SaveKeep"))
}

pub fn settings_file(root: &Path) -> PathBuf {
    root.join("settings.json")
}

pub fn write_json(path: &Path, value: &impl Serialize) -> Result<()> {
    let parent = path.parent().context("找不到数据文件所在目录")?;
    let mut temporary = NamedTempFile::new_in(parent).context("无法创建临时文件")?;
    serde_json::to_writer_pretty(&mut temporary, value)?;
    temporary.write_all(b"\n")?;
    temporary.as_file().sync_all()?;
    temporary.persist(path).context("无法保存数据文件")?;
    Ok(())
}

pub fn read_json<T: DeserializeOwned>(path: &Path) -> Result<T> {
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_file() && !crate::game::is_reparse(&metadata) && metadata.len() <= 1024 * 1024,
        "清单不是普通小文件。"
    );
    let file = fs::File::open(path)?;
    Ok(serde_json::from_reader(BufReader::new(file))?)
}

pub fn backup_location(save: &Path, requested: &Path) -> Result<PathBuf> {
    ensure!(requested.is_absolute(), "请选择绝对路径作为备份位置。");
    let mut ancestor = requested;
    while !ancestor.exists() {
        ancestor = ancestor.parent().context("找不到备份路径所在磁盘")?;
    }
    let existing = ancestor.canonicalize()?;
    crate::game::plain_path(ancestor)?;
    ensure!(!within(&existing, save), "备份位置不能在存档文件夹内。");
    let suffix = requested.strip_prefix(ancestor)?;
    ensure!(
        suffix
            .components()
            .all(|part| matches!(part, std::path::Component::Normal(_))),
        "备份位置包含无效路径。"
    );
    let resolved = if suffix.as_os_str().is_empty() {
        existing
    } else {
        existing.join(suffix)
    };
    ensure!(
        !within(&resolved.join(CATALOG), save) && !within(save, &resolved.join(CATALOG)),
        "存档位置和备份库不能重叠。"
    );
    Ok(resolved)
}

pub fn within(path: &Path, root: &Path) -> bool {
    let mut path_parts = path.components();
    root.components().all(|part| {
        path_parts.next().is_some_and(|other| {
            other.as_os_str().to_string_lossy().to_lowercase()
                == part.as_os_str().to_string_lossy().to_lowercase()
        })
    })
}

pub fn profile_dir(root: &Path, game: &str, profile: &str) -> Result<PathBuf> {
    ensure!(valid_identity(game, profile), "备份库身份不合法。");
    let mut path = root.to_path_buf();
    for part in [CATALOG, game, profile] {
        path.push(part);
        match fs::create_dir(&path) {
            Ok(()) => {}
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {}
            Err(error) => return Err(error).context("无法创建备份库目录"),
        }
        let meta = fs::symlink_metadata(&path)?;
        ensure!(
            meta.is_dir() && !crate::game::is_reparse(&meta),
            "备份库包含重定向目录。"
        );
    }
    Ok(path)
}

pub fn valid_identity(game: &str, profile: &str) -> bool {
    game.strip_prefix("steam-")
        .and_then(|id| {
            id.parse::<u32>()
                .ok()
                .filter(|number| number.to_string() == id)
        })
        .is_some_and(|number| number > 0)
        && if game == crate::game::GAME_ID {
            profile.len() == 17 && profile.bytes().all(|byte| byte.is_ascii_digit())
        } else {
            profile == "default"
        }
}

pub fn read_record(directory: &Path, game: &str, profile: &str, id: Uuid) -> Result<Record> {
    crate::game::plain_path(directory)?;
    let record: Record = read_json(&directory.join("record.json"))
        .with_context(|| format!("无法读取备份 {id} 的记录"))?;
    ensure!(
        record.id == id
            && record.format == 1
            && record.game_id == game
            && record.profile == profile
            && record.title.chars().count() <= 100
            && record.note.chars().count() <= 2000,
        "备份 {id} 的记录无效。"
    );
    Ok(record)
}

pub fn list(profile_dir: &Path, game: &str, profile: &str) -> Result<Vec<Record>> {
    let mut records = Vec::new();
    for entry in fs::read_dir(profile_dir).context("无法读取备份库")? {
        let entry = entry?;
        if !entry.file_type()?.is_dir() || crate::game::is_reparse(&entry.metadata()?) {
            continue;
        }
        let Ok(id) = Uuid::parse_str(&entry.file_name().to_string_lossy()) else {
            continue;
        };
        records.push(read_record(&entry.path(), game, profile, id)?);
    }
    records.sort_by_key(|record| std::cmp::Reverse((record.at, record.id)));
    Ok(records)
}

pub fn delete(profile_dir: &Path, record: &Record) -> Result<()> {
    if record.locked {
        bail!("请先取消保护，再删除这份备份。");
    }
    let source = profile_dir.join(record.id.to_string());
    let tombstone = profile_dir.join(format!(".deleted-{}", record.id));
    fs::rename(&source, &tombstone).context("无法移除备份")?;
    fs::remove_dir_all(tombstone).context("备份已移出列表，磁盘清理未完成")?;
    Ok(())
}
