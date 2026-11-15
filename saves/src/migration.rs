use std::{
    collections::{BTreeSet, HashMap, HashSet},
    fs::{self, File},
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use uuid::Uuid;
use walkdir::WalkDir;

use crate::{
    archive::{self, ARCHIVE},
    catalog::Catalog,
    dav, game,
    storage::{self, CATALOG, Record},
};

pub struct Preview {
    pub token: Uuid,
    catalog: Catalog,
    cloud: Option<dav::Config>,
    entries: Vec<Entry>,
    summary: Value,
}

struct Entry {
    source: PathBuf,
    record: Record,
    metadata: String,
    ready: bool,
}

impl Preview {
    pub fn inspect(legacy: &Path, root: &Path, current: &Catalog) -> Result<Self> {
        game::plain_path(legacy).context("找不到原留档数据目录")?;
        let catalog = Catalog::load(legacy)?;
        let mut roots = BTreeSet::from([legacy.join("Backups")]);
        roots.extend(
            catalog
                .games()
                .into_iter()
                .map(|target| target.backup_dir)
                .filter(|path| path.is_absolute()),
        );

        let mut entries = Vec::new();
        let mut errors = 0;
        let mut conflicts = 0;
        let mut duplicates = 0;
        let mut total_bytes = 0u64;
        let mut target_bytes = 0u64;
        let mut seen: HashMap<PathBuf, String> = HashMap::new();
        let mut blocked = HashSet::new();
        let mut profiles = HashSet::new();
        let mut locations = Vec::new();
        for source in &roots {
            let library = source.join(CATALOG);
            if source != &legacy.join("Backups") && !source.try_exists().unwrap_or(false) {
                errors += 1;
            }
            if !library.try_exists().unwrap_or(true) {
                continue;
            }
            if locations.len() < 12 {
                locations.push(
                    source
                        .to_string_lossy()
                        .chars()
                        .take(300)
                        .collect::<String>(),
                );
            }
            if game::plain_path(&library).is_err() {
                errors += 1;
                continue;
            }
            for entry in WalkDir::new(&library)
                .min_depth(1)
                .max_depth(3)
                .into_iter()
                .filter_entry(|entry| {
                    entry
                        .metadata()
                        .is_ok_and(|metadata| metadata.is_dir() && !game::is_reparse(&metadata))
                })
            {
                let Ok(entry) = entry else {
                    errors += 1;
                    continue;
                };
                if entry.depth() != 3 {
                    continue;
                }
                let path = entry.path();
                let Ok(id) = Uuid::parse_str(&entry.file_name().to_string_lossy()) else {
                    continue;
                };
                if entry.file_name() != id.to_string().as_str() {
                    continue;
                }
                let profile = path
                    .parent()
                    .and_then(Path::file_name)
                    .unwrap()
                    .to_string_lossy();
                let game = path
                    .parent()
                    .and_then(Path::parent)
                    .and_then(Path::file_name)
                    .unwrap()
                    .to_string_lossy();
                if !storage::valid_identity(&game, &profile) {
                    continue;
                }
                let Ok(record) = storage::read_record(path, &game, &profile, id) else {
                    errors += 1;
                    continue;
                };
                if !fs::symlink_metadata(path.join(ARCHIVE)).is_ok_and(|metadata| {
                    metadata.is_file()
                        && !game::is_reparse(&metadata)
                        && metadata.len() == record.bytes
                }) {
                    errors += 1;
                    continue;
                }
                total_bytes = total_bytes.saturating_add(record.bytes);
                profiles.insert((record.game_id.clone(), record.profile.clone()));
                let target = destination(root, current, &record);
                let key = target.join(id.to_string());
                let known = match seen.get(&key) {
                    Some(hash) => {
                        if hash != &record.sha256 {
                            blocked.insert(key.clone());
                        }
                        Some(Ok(hash == &record.sha256))
                    }
                    None => match key.try_exists() {
                        Ok(false) => None,
                        Ok(true) => {
                            target_bytes = target_bytes.saturating_add(
                                fs::symlink_metadata(key.join(ARCHIVE))
                                    .ok()
                                    .map_or(0, |metadata| metadata.len()),
                            );
                            Some(
                                storage::read_record(&key, &game, &profile, id)
                                    .and_then(|existing| {
                                        ensure!(existing.sha256 == record.sha256);
                                        archive::verify_record(&key.join(ARCHIVE), &record)
                                    })
                                    .map(|()| true),
                            )
                        }
                        Err(error) => Some(Err(error.into())),
                    },
                };
                let ready = match known {
                    Some(Ok(true)) => {
                        duplicates += 1;
                        false
                    }
                    Some(_) => {
                        conflicts += 1;
                        false
                    }
                    None => true,
                };
                seen.entry(key).or_insert_with(|| record.sha256.clone());
                entries.push(Entry {
                    source: path.to_owned(),
                    metadata: dav::metadata_hash(&record)?,
                    record,
                    ready,
                });
            }
        }
        let mut bytes = 0u64;
        for entry in &mut entries {
            if blocked.contains(
                &destination(root, current, &entry.record).join(entry.record.id.to_string()),
            ) {
                entry.ready = false;
            }
            if entry.ready {
                bytes = bytes.saturating_add(entry.record.bytes);
            }
        }
        let cloud_path = legacy.join("webdav.json");
        let cloud = if cloud_path.try_exists()? {
            #[derive(serde::Deserialize)]
            struct LegacyCloud {
                url: String,
                user: String,
            }
            match storage::read_json::<LegacyCloud>(&cloud_path)
                .and_then(|old| dav::Config::from_input(&old.url, &old.user, false))
            {
                Ok(config) if !config.url.is_empty() => Some(config),
                Ok(_) => None,
                Err(_) => {
                    errors += 1;
                    None
                }
            }
        } else {
            None
        };
        let token = Uuid::new_v4();
        let summary = json!({
            "token": token, "sources": locations, "records": entries.len(),
            "profiles": profiles.len(), "bytes_to_copy": bytes, "total_bytes":total_bytes,
            "existing_target_bytes":target_bytes, "duplicates": duplicates,
            "conflicts": conflicts, "unreadable": errors, "webdav": cloud.is_some(),
        });
        Ok(Self {
            token,
            catalog,
            cloud,
            entries,
            summary,
        })
    }

    pub fn summary(&self) -> Value {
        self.summary.clone()
    }

    pub fn apply(self, root: &Path, current: &mut Catalog) -> Result<Value> {
        let mut failures = Vec::new();
        let mut failed = 0;
        let mut copied = 0;
        for entry in self.entries.iter().filter(|entry| entry.ready) {
            let result = copy_entry(root, current, entry);
            match result {
                Ok(()) => copied += 1,
                Err(error) => {
                    failed += 1;
                    if failures.len() < 12 {
                        failures.push(json!({"id":entry.record.id, "error": error.to_string()}));
                    }
                }
            }
        }
        let mut next = current.clone();
        if next.builtin.save_dir.as_os_str().is_empty() {
            let backup = next.builtin.backup_dir.clone();
            next.builtin = self.catalog.builtin;
            next.builtin.backup_dir = backup;
        }
        for mut game in self.catalog.additional {
            if next
                .additional
                .iter()
                .any(|old| old.game.app_id == game.game.app_id)
            {
                continue;
            }
            game.settings.backup_dir = root.join("Backups");
            next.additional.push(game);
        }
        next.save(root)?;
        *current = next;
        let cloud_path = root.join("webdav.json");
        if let Some(config) = self.cloud
            && !cloud_path.try_exists()?
        {
            storage::write_json(&cloud_path, &config)?;
        }
        Ok(
            json!({"copied": copied, "failed": failed, "failures": failures, "preview": self.summary}),
        )
    }
}

fn backup_root(root: &Path, current: &Catalog, record: &Record) -> PathBuf {
    current
        .games()
        .into_iter()
        .find(|target| target.identity().0 == record.game_id)
        .map(|target| target.backup_dir)
        .filter(|path| path.is_absolute())
        .unwrap_or_else(|| root.join("Backups"))
}

fn destination(root: &Path, current: &Catalog, record: &Record) -> PathBuf {
    backup_root(root, current, record)
        .join(CATALOG)
        .join(&record.game_id)
        .join(&record.profile)
}

fn copy_entry(root: &Path, current: &Catalog, entry: &Entry) -> Result<()> {
    let record = &entry.record;
    let latest = storage::read_record(&entry.source, &record.game_id, &record.profile, record.id)?;
    ensure!(
        dav::metadata_hash(&latest)? == entry.metadata,
        "原记录在预览后发生变化，请重新预览。"
    );
    let path = entry.source.join(ARCHIVE);
    let meta = fs::symlink_metadata(&path)?;
    ensure!(
        meta.is_file() && !game::is_reparse(&meta),
        "原 ZIP 不是普通文件。"
    );
    let base = backup_root(root, current, record);
    let source_root = entry
        .source
        .ancestors()
        .nth(4)
        .context("原备份根无效")?
        .canonicalize()?;
    let mut ancestor = base.as_path();
    while !ancestor.try_exists()? {
        ancestor = ancestor.parent().context("找不到目标磁盘")?;
    }
    game::plain_path(ancestor)?;
    let canonical_base = ancestor.canonicalize()?.join(base.strip_prefix(ancestor)?);
    ensure!(
        !storage::within(&canonical_base, &source_root)
            && !storage::within(&source_root, &canonical_base),
        "迁移目标与旧备份目录重叠。"
    );
    fs::create_dir_all(&base)?;
    let dest = storage::profile_dir(&base, &record.game_id, &record.profile)?;
    archive::import_record(&dest, record, &mut File::open(path)?)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        archive::Library,
        storage::{Settings, Trigger},
    };

    #[test]
    fn v1_preview_copy_resume_conflict_and_cloud_configuration() -> Result<()> {
        let root = tempfile::tempdir()?;
        let legacy = root.path().join("原留档");
        let destination = root.path().join("游戏仪表");
        fs::create_dir(&legacy)?;
        fs::create_dir(&destination)?;
        let slot = root.path().join("76561198000000001");
        fs::create_dir(&slot)?;
        fs::write(slot.join(game::MAIN_SAVE), b"synthetic save")?;
        let primary = Settings {
            save_dir: slot.clone(),
            backup_dir: root.path().join("自定义备份"),
            ..Settings::default()
        };
        let old = Catalog {
            builtin: primary.clone(),
            ..Catalog::default()
        };
        old.save(&legacy)?;
        storage::write_json(
            &legacy.join("webdav.json"),
            &json!({
                "url":"https://example.invalid/dav/", "user":"fixture", "automatic":true, "secret":[12,34,56]
            }),
        )?;
        let extra = Settings {
            backup_dir: legacy.join("Backups"),
            ..primary.clone()
        };
        let old_library = Library::open(&primary)?;
        let mut first = old_library.backup(Trigger::Manual)?.unwrap();
        first.note = "校验中文备注".into();
        first.locked = true;
        let first_path = old_library.root.join(first.id.to_string());
        storage::write_json(&first_path.join("record.json"), &first)?;
        let original_metadata = fs::read(first_path.join("record.json"))?;
        let original_zip = fs::read(first_path.join(ARCHIVE))?;
        let second_library = Library::open(&extra)?;
        let second = second_library.backup(Trigger::Manual)?.unwrap();
        let second_zip = second_library
            .root
            .join(second.id.to_string())
            .join(ARCHIVE);
        let second_bytes = fs::read(&second_zip)?;

        let mut current = Catalog::load(&destination)?;
        let preview = Preview::inspect(&legacy, &destination, &current)?;
        assert_eq!(preview.summary()["records"], 2);
        assert_eq!(preview.summary()["profiles"], 1);
        assert!(!destination.join("Backups").exists());
        fs::write(&second_zip, vec![0; second_bytes.len()])?;
        let first_run = preview.apply(&destination, &mut current)?;
        assert_eq!(first_run["copied"], 1);
        assert_eq!(first_run["failed"], 1);
        assert_eq!(fs::read(first_path.join("record.json"))?, original_metadata);
        assert_eq!(fs::read(first_path.join(ARCHIVE))?, original_zip);
        fs::write(&second_zip, second_bytes)?;

        let preview = Preview::inspect(&legacy, &destination, &current)?;
        assert_eq!(preview.summary()["duplicates"], 1);
        assert_eq!(preview.apply(&destination, &mut current)?["copied"], 1);
        let migrated = Library::open(&current.builtin)?;
        let records = migrated.records()?;
        assert_eq!(records.len(), 2);
        let copied = records.iter().find(|item| item.id == first.id).unwrap();
        assert_eq!(dav::metadata_hash(copied)?, dav::metadata_hash(&first)?);
        archive::verify_record(
            &migrated.root.join(copied.id.to_string()).join(ARCHIVE),
            copied,
        )?;
        let v1_bytes = fs::read(migrated.root.join(copied.id.to_string()).join(ARCHIVE))?;
        assert_eq!(v1_bytes, original_zip);
        let cloud: Value = storage::read_json(&destination.join("webdav.json"))?;
        assert_eq!(cloud["automatic"], false);
        assert!(cloud.get("secret").is_none());

        let mut conflict = first.clone();
        conflict.sha256 = "0".repeat(64);
        storage::write_json(&first_path.join("record.json"), &conflict)?;
        let preview = Preview::inspect(&legacy, &destination, &current)?;
        assert_eq!(preview.summary()["conflicts"], 1);
        assert_eq!(preview.apply(&destination, &mut current)?["copied"], 0);
        assert_eq!(migrated.records()?.len(), 2);
        assert_eq!(
            fs::read(migrated.root.join(first.id.to_string()).join(ARCHIVE))?,
            original_zip
        );
        Ok(())
    }
}
