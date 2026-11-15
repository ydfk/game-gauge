use crate::{
    archive::{ARCHIVE, Library},
    dav::{Config, Remote, metadata_hash},
    storage::{self, Record, Settings},
};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
use uuid::Uuid;

pub struct Cloud {
    path: PathBuf,
    pub config: Config,
    retry: HashMap<Uuid, (u32, Instant, String)>,
}

impl Cloud {
    pub fn load(root: &Path) -> Result<Self> {
        let path = root.join("webdav.json");
        let config = if path.try_exists()? {
            let stored: Config = storage::read_json(&path)?;
            Config::from_input(&stored.url, &stored.user, stored.automatic)?
        } else {
            Config::default()
        };
        Ok(Self {
            path,
            config,
            retry: HashMap::new(),
        })
    }

    pub fn configure(&mut self, url: &str, user: &str, automatic: bool) -> Result<()> {
        let next = Config::from_input(url, user, automatic)?;
        storage::write_json(&self.path, &next)?;
        self.config = next;
        self.retry.clear();
        Ok(())
    }

    pub fn retry(&mut self) {
        self.retry.clear();
    }

    pub fn may_prune(&self, library: &Library, record: &Record) -> bool {
        self.config.url.is_empty()
            || current_receipt(&library.root, record, &self.config)
                .is_ok_and(|(_, current)| current)
    }

    pub fn upload(&self, library: &Library, record: &Record) -> Result<()> {
        let (has_archive, _) = current_receipt(&library.root, record, &self.config)?;
        Remote::new(&self.config)?.upload(
            record,
            &library.root.join(record.id.to_string()).join(ARCHIVE),
            has_archive,
        )?;
        write_receipt(&library.root, record, &self.config)
    }
    pub fn upload_next(&mut self, targets: &[Settings]) -> Result<String> {
        let config = &self.config;
        if config.url.is_empty() {
            return Ok("未配置 WebDAV".into());
        }
        if !config.automatic {
            return Ok("已暂停自动上传".into());
        }

        let mut pending = Vec::new();
        let mut ready = 0;
        let mut unavailable = 0;
        for target in targets {
            if target.save_dir.as_os_str().is_empty() {
                continue;
            }
            let Ok(library) = Library::open(target) else {
                unavailable += 1;
                continue;
            };
            let Ok(records) = library.records() else {
                unavailable += 1;
                continue;
            };
            for record in records {
                let (has_archive, current) = current_receipt(&library.root, &record, config)?;
                if current {
                    ready += 1;
                } else {
                    pending.push((library.root.clone(), record, has_archive));
                }
            }
        }
        pending.sort_by_key(|(_, record, _)| (record.at, record.id));
        self.retry
            .retain(|id, _| pending.iter().any(|(_, record, _)| record.id == *id));
        let mut waiting = pending.len();
        if let Some((root, record, metadata_only)) = pending.into_iter().find(|(_, record, _)| {
            self.retry
                .get(&record.id)
                .is_none_or(|(_, due, _)| Instant::now() >= *due)
        }) {
            let archive = root.join(record.id.to_string()).join(ARCHIVE);
            let result = Remote::new(config)
                .and_then(|remote| remote.upload(&record, &archive, metadata_only))
                .and_then(|()| write_receipt(&root, &record, config));
            match result {
                Ok(()) => {
                    self.retry.remove(&record.id);
                    ready += 1;
                    waiting -= 1;
                }
                Err(error) => {
                    let attempt = self
                        .retry
                        .get(&record.id)
                        .map_or(1, |(attempt, _, _)| attempt.saturating_add(1));
                    let due = Instant::now()
                        + Duration::from_secs((5 * 2u64.pow(attempt.min(6))).min(300));
                    self.retry
                        .insert(record.id, (attempt, due, format!("{error:#}")));
                }
            }
        }
        let status = format!("本地库已上传 {ready} 份 · 待上传 {waiting} 份");
        if let Some((_, _, message)) = self.retry.values().next() {
            return Ok(format!("{status} · 将自动重试：{message}"));
        }
        if unavailable > 0 {
            return Ok(format!("{status} · {unavailable} 个备份库暂不可用"));
        }
        Ok(status)
    }
}

#[derive(Deserialize, Serialize)]
struct Receipt {
    destination: String,
    sha256: String,
    metadata: String,
}

fn receipt(root: &Path, record: &Record) -> PathBuf {
    root.join(record.id.to_string()).join("webdav-receipt.json")
}

fn current_receipt(root: &Path, record: &Record, config: &Config) -> Result<(bool, bool)> {
    let Ok(receipt) = storage::read_json::<Receipt>(&receipt(root, record)) else {
        return Ok((false, false));
    };
    let archive = receipt.destination == config.destination() && receipt.sha256 == record.sha256;
    Ok((
        archive,
        archive && receipt.metadata == metadata_hash(record)?,
    ))
}

pub fn write_receipt(root: &Path, record: &Record, config: &Config) -> Result<()> {
    ensure!(!config.url.is_empty(), "WebDAV 尚未配置。");
    storage::write_json(
        &receipt(root, record),
        &Receipt {
            destination: config.destination(),
            sha256: record.sha256.clone(),
            metadata: metadata_hash(record)?,
        },
    )
    .context("无法保存上传回执")
}
