use std::{
    fs::{self, File},
    io::{self, BufReader, Read, Seek, Write},
    path::{Path, PathBuf},
    thread,
    time::Duration,
};

use anyhow::{Context, Result, bail, ensure};
use chrono::Utc;
use sha2::{Digest, Sha256};
use tempfile::{NamedTempFile, tempdir_in};
use uuid::Uuid;
use walkdir::WalkDir;
use zip::{CompressionMethod, ZipArchive, ZipWriter, write::SimpleFileOptions};

use crate::{
    game,
    storage::{self, Record, Trigger},
};

pub const ARCHIVE: &str = "save.zip";
const MAX_FILES: usize = 10_000;
const MAX_BYTES: u64 = 4 * 1024 * 1024 * 1024;

pub struct Library {
    pub save: PathBuf,
    pub root: PathBuf,
    pub profile: String,
    pub game_id: String,
    custom: Option<game::CustomGame>,
}

impl Library {
    pub fn open(settings: &storage::Settings) -> Result<Self> {
        if let Ok(original) = rollback_path(&settings.save_dir)
            && original.exists()
            && !settings.save_dir.exists()
        {
            bail!(
                "上次恢复被中断，原始存档在 {}。请先关闭游戏，将该目录改回原 Steam 账号目录名。",
                original.display()
            );
        }
        let save = game::resolve_save(settings)?;
        let (game_id, profile) = settings.identity();
        let location = storage::backup_location(&save, &settings.backup_dir)?;
        fs::create_dir_all(&location).context("无法创建备份位置")?;
        let root = storage::profile_dir(&location, &game_id, &profile)?;
        Ok(Self {
            save,
            root,
            profile,
            game_id,
            custom: settings.custom.clone(),
        })
    }

    pub fn records(&self) -> Result<Vec<Record>> {
        storage::list(&self.root, &self.game_id, &self.profile)
    }

    pub fn backup(&self, trigger: Trigger) -> Result<Option<Record>> {
        let custom = self.custom.is_some();
        if !custom {
            game::validate_slot(&self.save)?;
        }
        let prior = if trigger.automatic() {
            self.records()?
                .into_iter()
                .next()
                .filter(|r| r.trigger != Trigger::BeforeRestore)
        } else {
            None
        };
        if let Some(previous) = &prior
            && fingerprint(&self.save, custom)? == previous.content_sha256
        {
            return Ok(None);
        }
        let work = tempdir_in(&self.root).context("无法创建临时备份目录")?;
        let path = work.path().join(ARCHIVE);
        let mut content_sha256 = None;
        for _ in 0..3 {
            let before = inventory(&self.save, custom)?;
            ensure!(!before.is_empty(), "存档文件夹内没有可备份的文件。");
            thread::sleep(Duration::from_millis(450));
            if before != inventory(&self.save, custom)? {
                continue;
            }
            let attempt = pack(&self.save, &path, &before).and_then(|captured| {
                Ok((fingerprint(&self.save, custom)? == captured).then_some(captured))
            });
            match attempt {
                Ok(Some(captured)) => {
                    content_sha256 = Some(captured);
                    break;
                }
                Ok(None) => {}
                Err(error) if busy_io(&error) => {}
                Err(error) => return Err(error),
            }
        }
        let content_sha256 = content_sha256.context("存档仍在写入或被占用，请稍后重试")?;
        if prior.is_some_and(|previous| previous.content_sha256 == content_sha256) {
            return Ok(None);
        }
        File::options().write(true).open(&path)?.sync_all()?;
        let file = File::open(&path)?;
        let bytes = file.metadata()?.len();
        let sha256 = digest(BufReader::new(file))?;
        ensure!(
            read_snapshot(&path, None, custom)? == content_sha256,
            "备份内容校验失败。"
        );
        let record = Record {
            format: 1,
            id: Uuid::new_v4(),
            game_id: self.game_id.clone(),
            profile: self.profile.clone(),
            at: Utc::now(),
            trigger,
            title: format!("{}备份", trigger.label()),
            note: String::new(),
            locked: trigger == Trigger::BeforeRestore,
            bytes,
            sha256,
            content_sha256,
        };
        storage::write_json(&work.path().join("record.json"), &record)?;
        fs::rename(work.path(), self.root.join(record.id.to_string()))
            .context("无法提交已校验的备份")?;
        Ok(Some(record))
    }

    pub fn prune_uploaded(&self, limit: usize, eligible: impl Fn(&Record) -> bool) -> Result<()> {
        for record in self
            .records()?
            .iter()
            .filter(|r| r.trigger.automatic() && !r.locked)
            .skip(limit)
            .filter(|record| eligible(record))
        {
            storage::delete(&self.root, record)?;
        }
        Ok(())
    }

    pub fn import(&self, record: &Record, source: &mut impl Read) -> Result<()> {
        ensure!(
            record.format == 1 && record.game_id == self.game_id && record.profile == self.profile,
            "云端备份与当前游戏或账号不一致。"
        );
        import_record(&self.root, record, source)
    }

    pub fn restore(&self, record: &Record) -> Result<Option<String>> {
        let custom = self.custom.is_some();
        ensure!(
            !game::is_running(self.custom.as_ref())?,
            "请关闭游戏后再恢复。"
        );
        let parent = self.save.parent().context("无法定位存档父目录")?;
        let rollback = rollback_path(&self.save)?;
        ensure!(
            !rollback.exists(),
            "上次恢复的原始目录仍存在，请先检查目录：{}",
            rollback.display()
        );
        let archive = self.root.join(record.id.to_string()).join(ARCHIVE);
        verify_record(&archive, record)?;

        let has_directory = self.save.try_exists()?;
        let before = if custom {
            if has_directory && !inventory(&self.save, true)?.is_empty() {
                self.backup(Trigger::BeforeRestore)?
            } else {
                None
            }
        } else {
            match fs::symlink_metadata(self.save.join(game::MAIN_SAVE)) {
                Ok(meta) if meta.is_file() && !game::is_reparse(&meta) && meta.len() == 0 => None,
                Ok(_) => Some(
                    self.backup(Trigger::BeforeRestore)?
                        .context("未能保护当前存档")?,
                ),
                Err(error) if error.kind() == io::ErrorKind::NotFound => None,
                Err(error) => return Err(error).context("无法检查当前主存档"),
            }
        };
        let staged = tempdir_in(parent).context("无法在存档磁盘上暂存恢复文件")?;
        if has_directory {
            copy_unmanaged(&self.save, staged.path(), custom)?;
        }
        ensure!(
            read_snapshot(&archive, Some(staged.path()), custom)? == record.content_sha256,
            "待恢复的存档未通过校验。"
        );
        ensure!(
            !game::is_running(self.custom.as_ref())?,
            "游戏已启动，恢复已取消。"
        );
        if let Some(before) = &before {
            ensure!(
                fingerprint(&self.save, custom)? == before.content_sha256,
                "当前存档已变化，恢复已取消。"
            );
        } else if custom {
            ensure!(
                !self.save.try_exists()? || inventory(&self.save, true)?.is_empty(),
                "当前存档已变化，恢复已取消。"
            );
        } else {
            ensure!(
                self.save
                    .join(game::MAIN_SAVE)
                    .symlink_metadata()
                    .is_ok_and(|m| m.len() == 0)
                    || !self.save.join(game::MAIN_SAVE).try_exists()?,
                "当前存档已变化，恢复已取消。"
            );
        }
        ensure!(
            !game::is_running(self.custom.as_ref())?,
            "游戏已启动，恢复已取消。"
        );

        if !has_directory {
            fs::rename(staged.path(), &self.save).context("无法重新创建存档目录")?;
            return Ok(Some("已从备份重建丢失的存档目录。".into()));
        }
        commit_restore(&self.save, staged.path(), &rollback)?;
        let preserved = parent.join(format!(".savekeep-preserved-{}", Uuid::new_v4()));
        let original = match fs::rename(&rollback, &preserved) {
            Ok(()) => preserved,
            Err(_) => rollback,
        };
        Ok(Some(format!(
            "恢复完成。原始目录保留在 {}。",
            original.display()
        )))
    }
}

fn commit_restore(save: &Path, staged: &Path, rollback: &Path) -> Result<()> {
    fs::rename(save, rollback).context("无法暂存原始存档目录")?;
    if let Err(error) = fs::rename(staged, save) {
        return match fs::rename(rollback, save) {
            Ok(()) => Err(error).context("替换失败，原始存档已恢复"),
            Err(rollback_error) => bail!(
                "替换失败：{error}。原始存档保存在 {}，自动回滚失败：{rollback_error}",
                rollback.display()
            ),
        };
    }
    Ok(())
}

pub fn verify_record(path: &Path, record: &Record) -> Result<()> {
    verify_archive(path, record)?;
    ensure!(
        read_snapshot(path, None, record.game_id != game::GAME_ID)? == record.content_sha256,
        "备份内容校验失败。"
    );
    Ok(())
}

pub fn import_record(root: &Path, record: &Record, source: &mut impl Read) -> Result<()> {
    ensure!(
        record.bytes > 0 && record.bytes <= MAX_BYTES + 16 * 1024 * 1024,
        "备份大小超限。"
    );
    let dest = root.join(record.id.to_string());
    if dest.try_exists()? {
        let existing = storage::read_record(&dest, &record.game_id, &record.profile, record.id)?;
        ensure!(
            existing.sha256 == record.sha256,
            "本地同 ID 备份内容不同，已停止覆盖。"
        );
        return verify_record(&dest.join(ARCHIVE), record);
    }
    let work = tempfile::Builder::new()
        .prefix(".incoming-")
        .tempdir_in(root)?;
    let path = work.path().join(ARCHIVE);
    let mut file = File::create(&path)?;
    let copied = io::copy(&mut source.take(record.bytes + 1), &mut file)?;
    ensure!(copied == record.bytes, "备份大小与清单不一致。");
    file.sync_all()?;
    drop(file);
    verify_record(&path, record)?;
    storage::write_json(&work.path().join("record.json"), record)?;
    fs::rename(work.path(), dest).context("无法提交已校验的备份")?;
    Ok(())
}

pub fn rollback_path(save: &Path) -> Result<PathBuf> {
    let profile = save.file_name().context("存档目录无效")?.to_string_lossy();
    Ok(save.with_file_name(format!(".savekeep-original-{profile}")))
}

pub fn verify_archive(archive: &Path, record: &Record) -> Result<()> {
    let metadata = fs::symlink_metadata(archive).context("找不到备份文件")?;
    ensure!(
        metadata.is_file() && !game::is_reparse(&metadata),
        "备份文件不是普通文件。"
    );
    ensure!(
        metadata.len() == record.bytes
            && digest(BufReader::new(File::open(archive)?))? == record.sha256,
        "备份未通过完整性校验。"
    );
    Ok(())
}

fn busy_io(error: &anyhow::Error) -> bool {
    error
        .chain()
        .filter_map(|cause| cause.downcast_ref::<io::Error>())
        .any(|error| matches!(error.raw_os_error(), Some(2 | 3 | 32 | 33)))
}

#[derive(PartialEq, Eq)]
struct Entry {
    relative: String,
    bytes: u64,
    modified: std::time::SystemTime,
}

fn archive_name(path: &Path, custom: bool) -> Result<String> {
    let name = path
        .to_str()
        .context("存档文件名不是有效的 Unicode")?
        .replace('\\', "/");
    ensure!(managed_name(&name, custom), "存档文件名不安全。");
    Ok(name)
}

fn managed_name(name: &str, custom: bool) -> bool {
    let parts: Vec<_> = name.split('/').collect();
    let safe = parts.iter().all(|part| {
        !part.is_empty()
            && !part.ends_with(['.', ' '])
            && !part
                .chars()
                .any(|ch| ch.is_control() || "\\:*?\"<>|".contains(ch))
            && !reserved(part)
    });
    safe && if custom {
        !parts
            .last()
            .is_some_and(|part| part.eq_ignore_ascii_case("steam_autocloud.vdf"))
    } else {
        (parts.len() == 1 || game::BACKUP_DIRS.contains(&parts[0]))
            && game::is_save(Path::new(name))
    }
}

fn reserved(part: &str) -> bool {
    let stem = part
        .split('.')
        .next()
        .unwrap_or_default()
        .trim_end()
        .to_ascii_uppercase();
    matches!(
        stem.as_str(),
        "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$"
    ) || ["COM", "LPT"].iter().any(|prefix| {
        stem.strip_prefix(prefix).is_some_and(|number| {
            matches!(
                number,
                "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³"
            )
        })
    })
}

fn inventory(save: &Path, custom: bool) -> Result<Vec<Entry>> {
    let mut entries = Vec::new();
    if custom {
        for entry in WalkDir::new(save).min_depth(1) {
            let entry = entry?;
            ensure!(
                !entry.file_name().eq_ignore_ascii_case(storage::CATALOG),
                "存档目录中包含留档的备份库，请把两者放在不同目录。"
            );
            let meta = fs::symlink_metadata(entry.path())?;
            ensure!(!game::is_reparse(&meta), "存档目录包含链接或重定向项。");
            if meta.is_file()
                && !entry
                    .file_name()
                    .eq_ignore_ascii_case("steam_autocloud.vdf")
            {
                add_entry(&mut entries, save, entry.path(), meta, true)?;
            }
        }
    } else {
        for root_entry in fs::read_dir(save).context("无法读取存档目录")? {
            let root_entry = root_entry?;
            let path = root_entry.path();
            let is_backup_dir = root_entry
                .file_name()
                .to_str()
                .is_some_and(|name| game::BACKUP_DIRS.contains(&name));
            if is_backup_dir {
                let meta = fs::symlink_metadata(&path)?;
                ensure!(
                    meta.is_dir() && !game::is_reparse(&meta),
                    "游戏轮转备份目录包含重定向。"
                );
                for entry in WalkDir::new(&path) {
                    let entry = entry?;
                    let meta = fs::symlink_metadata(entry.path())?;
                    ensure!(!game::is_reparse(&meta), "游戏轮转备份目录包含重定向。");
                    if meta.is_file() && game::is_save(entry.path()) {
                        add_entry(&mut entries, save, entry.path(), meta, false)?;
                    }
                }
            } else if game::is_save(&path) {
                add_entry(
                    &mut entries,
                    save,
                    &path,
                    fs::symlink_metadata(&path)?,
                    false,
                )?;
            }
        }
        ensure!(
            entries.iter().any(
                |entry| entry.relative.eq_ignore_ascii_case(game::MAIN_SAVE) && entry.bytes > 0
            ),
            "找不到可用的主存档。"
        );
    }
    entries.sort_by(|a, b| a.relative.cmp(&b.relative));
    ensure!(
        entries.len() <= MAX_FILES
            && entries
                .iter()
                .try_fold(0u64, |sum, e| sum.checked_add(e.bytes))
                .is_some_and(|total| total <= MAX_BYTES),
        "存档超过单份备份限制。"
    );
    Ok(entries)
}

fn add_entry(
    entries: &mut Vec<Entry>,
    root: &Path,
    path: &Path,
    meta: fs::Metadata,
    custom: bool,
) -> Result<()> {
    ensure!(
        meta.is_file() && !game::is_reparse(&meta),
        "存档包含链接或重定向文件。"
    );
    ensure!(entries.len() < MAX_FILES, "存档文件超过单份备份限制。");
    entries.push(Entry {
        relative: archive_name(path.strip_prefix(root)?, custom)?,
        bytes: meta.len(),
        modified: meta.modified()?,
    });
    Ok(())
}

fn transfer(mut reader: impl Read, mut output: impl Write, size: u64) -> Result<[u8; 32]> {
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 65536];
    let mut read = 0u64;
    loop {
        let count = reader.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        read += count as u64;
        ensure!(read <= size, "读取期间存档大小变化。");
        hash.update(&buffer[..count]);
        output.write_all(&buffer[..count])?;
    }
    ensure!(read == size, "读取期间存档大小变化。");
    Ok(hash.finalize().into())
}

fn mix(hash: &mut Sha256, path: &str, size: u64, file: [u8; 32]) {
    hash.update((path.len() as u64).to_le_bytes());
    hash.update(path.as_bytes());
    hash.update(size.to_le_bytes());
    hash.update(file);
}

fn digest(mut reader: impl Read) -> Result<String> {
    let mut hash = Sha256::new();
    io::copy(&mut reader, &mut hash)?;
    Ok(format!("{:x}", hash.finalize()))
}

fn pack(root: &Path, path: &Path, entries: &[Entry]) -> Result<String> {
    let mut writer = ZipWriter::new(File::create(path)?);
    let mut hash = Sha256::new();
    let options = SimpleFileOptions::default()
        .compression_method(CompressionMethod::Deflated)
        .compression_level(Some(1))
        .large_file(true);
    for entry in entries {
        writer.start_file(&entry.relative, options)?;
        let file = File::open(root.join(&entry.relative))?;
        let file_hash = transfer(BufReader::new(file), &mut writer, entry.bytes)?;
        mix(&mut hash, &entry.relative, entry.bytes, file_hash);
    }
    writer.finish()?.sync_all()?;
    Ok(format!("{:x}", hash.finalize()))
}

fn fingerprint(root: &Path, custom: bool) -> Result<String> {
    let mut hash = Sha256::new();
    for entry in inventory(root, custom)? {
        let file = File::open(root.join(&entry.relative))?;
        let file_hash = transfer(BufReader::new(file), io::sink(), entry.bytes)?;
        mix(&mut hash, &entry.relative, entry.bytes, file_hash);
    }
    Ok(format!("{:x}", hash.finalize()))
}

fn read_snapshot(path: &Path, destination: Option<&Path>, custom: bool) -> Result<String> {
    read_zip(
        ZipArchive::new(BufReader::new(File::open(path)?))?,
        destination,
        custom,
    )
}

fn read_zip<R: Read + Seek>(
    mut archive: ZipArchive<R>,
    destination: Option<&Path>,
    custom: bool,
) -> Result<String> {
    ensure!(archive.len() <= MAX_FILES, "备份内的文件数量超出限制。");
    let mut items = Vec::new();
    let mut names = std::collections::HashSet::new();
    let mut total = 0u64;
    let mut has_main = false;
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index)?;
        let name = entry.name().to_owned();
        let size = entry.size();
        ensure!(
            entry.is_file()
                && entry.enclosed_name().is_some()
                && managed_name(&name, custom)
                && names.insert(name.to_lowercase()),
            "备份含有重复或不安全的文件路径。"
        );
        total = total.checked_add(size).context("备份体积超出限制")?;
        ensure!(total <= MAX_BYTES, "备份体积超出限制。");
        has_main |= name.eq_ignore_ascii_case(game::MAIN_SAVE) && size > 0;
        let checksum = if let Some(destination) = destination {
            let path = destination.join(&name);
            let parent = path.parent().unwrap();
            fs::create_dir_all(parent)?;
            let mut file = NamedTempFile::new_in(parent)?;
            let checksum = transfer(&mut entry, &mut file, size)?;
            file.as_file().sync_all()?;
            file.persist_noclobber(path)?;
            checksum
        } else {
            transfer(&mut entry, io::sink(), size)?
        };
        items.push((name, size, checksum));
    }
    ensure!(
        if custom { !items.is_empty() } else { has_main },
        "备份中没有可用的存档。"
    );
    items.sort_by(|a, b| a.0.cmp(&b.0));
    let mut hash = Sha256::new();
    for (name, size, checksum) in items {
        mix(&mut hash, &name, size, checksum);
    }
    Ok(format!("{:x}", hash.finalize()))
}

fn copy_unmanaged(source: &Path, dest: &Path, custom: bool) -> Result<()> {
    for entry in WalkDir::new(source).min_depth(1) {
        let entry = entry?;
        let meta = fs::symlink_metadata(entry.path())?;
        ensure!(!game::is_reparse(&meta), "存档目录含重定向项，已停止恢复。");
        let relative = entry.path().strip_prefix(source)?;
        let target = dest.join(relative);
        if meta.is_dir() {
            fs::create_dir_all(&target)?;
        } else if meta.is_file()
            && !relative
                .to_str()
                .is_some_and(|name| managed_name(&name.replace('\\', "/"), custom))
        {
            let mut input = File::open(entry.path()).context("无法读取存档目录内的其他文件")?;
            let mut output = File::create(&target).context("无法保留存档目录内的其他文件")?;
            io::copy(&mut input, &mut output)?;
            output.sync_all()?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;
