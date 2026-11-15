use std::{
    collections::HashMap,
    path::PathBuf,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail, ensure};
use serde_json::{Value, json};
use uuid::Uuid;

use crate::{
    archive::{self, ARCHIVE, Library},
    catalog::Catalog,
    cloud::{self, Cloud},
    dav::Remote,
    game, migration,
    storage::{self, Record, Settings, Trigger},
};

pub struct Worker {
    root: PathBuf,
    catalog: Catalog,
    schedules: HashMap<(String, String), Schedule>,
    cloud: Cloud,
    cloud_status: String,
    next_cloud: Instant,
    remote: Vec<Record>,
    preview: Option<migration::Preview>,
    automatic_status: String,
}

impl Worker {
    pub fn load(root: PathBuf) -> Result<Self> {
        let catalog = Catalog::load(&root)?;
        let cloud = Cloud::load(&root)?;
        Ok(Self {
            root,
            catalog,
            cloud,
            schedules: HashMap::new(),
            cloud_status: String::new(),
            next_cloud: Instant::now(),
            remote: Vec::new(),
            preview: None,
            automatic_status: String::new(),
        })
    }

    pub fn hotkey(&self) -> &str {
        &self.catalog.builtin.hotkey
    }

    pub fn request(&mut self, input: &Value) -> Result<Value> {
        let command = string(input, "command")?;
        match command {
            "saves.status" => Ok(json!({
                "enabled": self.catalog.enabled, "hotkey": self.hotkey(),
                "automatic_status": self.automatic_status,
                "cloud": {
                    "url": self.cloud.config.url, "user": self.cloud.config.user,
                    "automatic": self.cloud.config.automatic, "status": self.cloud_status,
                    "credential_target": self.cloud.config.credential_target(),
                },
            })),
            "saves.games.list" => {
                let names = self.catalog.names();
                let slots = game::discover();
                let games: Vec<_> = self.catalog.games().iter().zip(names).enumerate()
                    .map(|(index, (target, name))| {
                        let (game, profile) = target.identity();
                        json!({"name":name,"game_id":game,"profile":profile,"selected":index==self.catalog.selected,
                            "executable":target.custom.as_ref().map(|game| &game.executable),"settings":target})
                    }).collect();
                Ok(json!({"games":games,"slots":slots}))
            }
            "saves.games.select" => {
                let index = self.target_index(input)?;
                self.change_catalog(|catalog| {
                    catalog.selected = index;
                    Ok(())
                })
            }
            "saves.games.add" => {
                let id = input["app_id"]
                    .as_u64()
                    .context("需要 Steam App ID")?
                    .try_into()?;
                let executable = string(input, "executable")?;
                let save = PathBuf::from(string(input, "save_dir")?);
                let result = self.change_catalog(|catalog| {
                    ensure!(
                        catalog.additional.len() < 40,
                        "最多可添加 40 款 Steam 游戏。"
                    );
                    catalog.add(id, executable, save)
                })?;
                Ok(result)
            }
            "saves.configure" => self.configure(input),
            "saves.backup.enqueue" => {
                let target = if input["shortcut"] == true {
                    match self.shortcut_target()? {
                        Some(target) => target,
                        None => {
                            return Ok(
                                json!({"choose_target":true,"message":"多款游戏正在运行，请选择档案后备份。"}),
                            );
                        }
                    }
                } else {
                    self.target(input)?
                };
                self.backup(&target, Trigger::Manual)
            }
            "saves.records.list" => {
                let target = self.target(input)?;
                let (game, profile) = target.identity();
                let records = if profile.is_empty() || target.save_dir.as_os_str().is_empty() {
                    Vec::new()
                } else {
                    let directory = target
                        .backup_dir
                        .join(storage::CATALOG)
                        .join(&game)
                        .join(&profile);
                    if directory.try_exists()? {
                        game::plain_path(&directory)?;
                        storage::list(&directory, &game, &profile)?
                    } else {
                        Vec::new()
                    }
                };
                Ok(page(&records, input))
            }
            "saves.records.detail"
            | "saves.records.verify"
            | "saves.records.edit"
            | "saves.records.protect"
            | "saves.records.delete"
            | "saves.restore.enqueue"
            | "saves.cloud.upload.enqueue" => self.record_action(command, input),
            "saves.migration.preview" => {
                let preview =
                    migration::Preview::inspect(&storage::app_dir()?, &self.root, &self.catalog)?;
                let summary = preview.summary();
                self.preview = Some(preview);
                Ok(summary)
            }
            "saves.migration.apply" => {
                ensure!(
                    !game::processes()?.iter().any(|name| name == "savekeep.exe"),
                    "请退出留档 SaveKeep，再复制旧库。"
                );
                let token = Uuid::parse_str(string(input, "token")?)?;
                ensure!(
                    self.preview.as_ref().is_some_and(|p| p.token == token),
                    "请先预览本次迁移。"
                );
                let result = self
                    .preview
                    .take()
                    .unwrap()
                    .apply(&self.root, &mut self.catalog)?;
                self.cloud = Cloud::load(&self.root)?;
                Ok(result)
            }
            "saves.cloud.configure" => {
                self.cloud.configure(
                    string(input, "url")?,
                    string(input, "user")?,
                    boolean(input, "automatic")?,
                )?;
                self.remote.clear();
                self.next_cloud = Instant::now();
                Ok(
                    json!({"message":"WebDAV 设置已保存","credential_target":self.cloud.config.credential_target()}),
                )
            }
            "saves.cloud.test.enqueue" => {
                Remote::new(&self.cloud.config)?.test()?;
                Ok(json!({"message":"WebDAV 连接成功"}))
            }
            "saves.cloud.retry.enqueue" => {
                self.cloud.retry();
                self.next_cloud = Instant::now();
                Ok(json!({"message":"已重新排队"}))
            }
            "saves.cloud.list.enqueue" => {
                let (game, profile) = self.target(input)?.identity();
                let remote = Remote::new(&self.cloud.config)?;
                self.remote = if profile.is_empty() {
                    remote.list_accounts(&game)?
                } else {
                    remote.list(&game, &profile)?
                };
                Ok(page(&self.remote, input))
            }
            "saves.cloud.page" => {
                let (game, profile) = self.target(input)?.identity();
                let records: Vec<_> = self
                    .remote
                    .iter()
                    .filter(|rec| {
                        rec.game_id == game && (profile.is_empty() || rec.profile == profile)
                    })
                    .cloned()
                    .collect();
                Ok(page(&records, input))
            }
            "saves.cloud.download.enqueue" => {
                let target = self.target(input)?;
                let library = Library::open(&target)?;
                let id = record_id(input)?;
                let record = self
                    .remote
                    .iter()
                    .find(|rec| {
                        rec.id == id
                            && rec.game_id == library.game_id
                            && rec.profile == library.profile
                    })
                    .context("请刷新云端列表并选择当前档案中的备份")?;
                Remote::new(&self.cloud.config)?.download(&library, record)?;
                cloud::write_receipt(&library.root, record, &self.cloud.config)?;
                Ok(json!({"message":"云端备份已校验并导入本地库"}))
            }
            _ => bail!("未知存档命令"),
        }
    }

    fn target_index(&self, input: &Value) -> Result<usize> {
        let id = string(input, "game_id")?;
        self.catalog
            .games()
            .iter()
            .position(|target| target.identity().0 == id)
            .context("游戏未登记")
    }

    fn target(&self, input: &Value) -> Result<Settings> {
        let target = self.catalog.games().swap_remove(self.target_index(input)?);
        ensure!(
            target.identity().1 == string(input, "profile")?,
            "档案已经变化，请刷新再操作。"
        );
        Ok(target)
    }

    fn change_catalog(&mut self, change: impl FnOnce(&mut Catalog) -> Result<()>) -> Result<Value> {
        let mut next = self.catalog.clone();
        change(&mut next)?;
        next.save(&self.root)?;
        self.catalog = next;
        self.preview = None;
        Ok(json!({"message":"设置已保存"}))
    }

    fn configure(&mut self, input: &Value) -> Result<Value> {
        let index = self.target_index(input)?;
        let mut target = self.target(input)?;
        let next_dir = PathBuf::from(string(input, "save_dir")?);
        let next_root = PathBuf::from(string(input, "backup_dir")?);
        target.interval_minutes = input["interval_minutes"].as_u64().context("需要检查间隔")?;
        target.keep_auto = input["keep_auto"]
            .as_u64()
            .context("需要保留数量")?
            .try_into()?;
        target.auto_enabled = boolean(input, "auto_enabled")?;
        target.hotkey = string(input, "hotkey")?.to_owned();
        ensure!(
            (1..=1440).contains(&target.interval_minutes),
            "检查间隔须为 1–1440 分钟。"
        );
        ensure!(
            (1..=9999).contains(&target.keep_auto),
            "保留数量须为 1–9999 份。"
        );
        ensure!(target.hotkey.len() <= 30, "快捷键过长。");
        target.save_dir = next_dir;
        target.backup_dir = if next_root.as_os_str().is_empty() {
            self.root.join("Backups")
        } else {
            next_root
        };
        if !target.save_dir.as_os_str().is_empty() {
            let save_dir = game::resolve_save(&target)?;
            let backup_dir = storage::backup_location(&save_dir, &target.backup_dir)?;
            self.catalog
                .check_overlap(&save_dir, &backup_dir, Some(index))?;
            target.save_dir = save_dir;
            target.backup_dir = backup_dir;
        }
        let enabled = boolean(input, "enabled")?;
        self.schedules.clear();
        self.change_catalog(|catalog| {
            catalog.selected = index;
            catalog.enabled = enabled;
            catalog.update_current(target);
            Ok(())
        })
        .map(|mut result| {
            result["hotkey"] = json!(self.hotkey());
            result
        })
    }

    fn record_action(&mut self, action: &str, input: &Value) -> Result<Value> {
        let library = Library::open(&self.target(input)?)?;
        let id = record_id(input)?;
        let dir = library.root.join(id.to_string());
        let mut record = storage::read_record(&dir, &library.game_id, &library.profile, id)?;
        let message = match action {
            "saves.records.detail" => return Ok(json!({"record": record})),
            "saves.records.verify" => {
                archive::verify_record(&dir.join(ARCHIVE), &record)?;
                "备份校验通过".into()
            }
            "saves.records.edit" => {
                let title = string(input, "title")?.trim();
                let note = string(input, "note")?;
                ensure!(
                    !title.is_empty()
                        && title.chars().count() <= 100
                        && note.chars().count() <= 2000,
                    "名称须为 1–100 个字符，备注最多 2000 个字符。"
                );
                record.title = title.into();
                record.note = note.into();
                storage::write_json(&dir.join("record.json"), &record)?;
                "已保存名称与备注".into()
            }
            "saves.records.protect" => {
                record.locked = boolean(input, "locked")?;
                storage::write_json(&dir.join("record.json"), &record)?;
                "已更新保护状态".into()
            }
            "saves.records.delete" => {
                ensure!(boolean(input, "confirmed")?, "需要确认删除。");
                storage::delete(&library.root, &record)?;
                "已删除本地备份".into()
            }
            "saves.restore.enqueue" => {
                ensure!(boolean(input, "confirmed")?, "请关闭游戏并确认恢复。");
                library
                    .restore(&record)?
                    .unwrap_or_else(|| "已恢复，原存档另有保护备份".into())
            }
            "saves.cloud.upload.enqueue" => {
                self.cloud.upload(&library, &record)?;
                "WebDAV 上传完成".into()
            }
            _ => unreachable!(),
        };
        Ok(json!({"message":message}))
    }

    fn shortcut_target(&self) -> Result<Option<Settings>> {
        let processes = game::processes()?;
        let games = self.catalog.games();
        let mut running = games.iter().filter(|target| {
            !target.save_dir.as_os_str().is_empty()
                && game::matches_process(target.custom.as_ref(), &processes)
        });
        let first = running.next();
        if running.next().is_some() {
            return Ok(None);
        }
        Ok(Some(
            first.cloned().unwrap_or_else(|| self.catalog.current()),
        ))
    }

    fn backup(&self, target: &Settings, trigger: Trigger) -> Result<Value> {
        ensure!(
            !target.save_dir.as_os_str().is_empty(),
            "请先选择存档目录。"
        );
        let library = Library::open(target)?;
        let Some(record) = library.backup(trigger)? else {
            return Ok(json!({"message":"存档内容没有变化"}));
        };
        let message = match self.prune(target, &library) {
            Ok(()) => format!("{}备份已保存并校验", trigger.label()),
            Err(error) => format!("备份成功，自动清理失败：{error:#}"),
        };
        Ok(json!({"message":message,"record_id":record.id}))
    }

    fn prune(&self, target: &Settings, library: &Library) -> Result<()> {
        library.prune_uploaded(target.keep_auto, |record| {
            self.cloud.may_prune(library, record)
        })
    }

    pub fn tick(&mut self) {
        let targets = self.catalog.games();
        if self.catalog.enabled
            && let Ok(processes) = game::processes()
        {
            let now = Instant::now();
            for target in &targets {
                if target.save_dir.as_os_str().is_empty() {
                    continue;
                }
                let running = game::matches_process(target.custom.as_ref(), &processes);
                let interval = Duration::from_secs(target.interval_minutes.clamp(1, 1440) * 60);
                if let Some(trigger) = self
                    .schedules
                    .entry(target.identity())
                    .or_default()
                    .observe(running, target.auto_enabled, interval, now)
                {
                    self.automatic_status = match self.backup(target, trigger) {
                        Ok(value) => value["message"].as_str().unwrap_or_default().into(),
                        Err(error) => format!("{error:#}"),
                    };
                }
            }
        }
        if self.cloud.config.automatic && Instant::now() >= self.next_cloud {
            self.cloud_status = self
                .cloud
                .upload_next(&targets)
                .unwrap_or_else(|error| format!("{error:#}"));
            self.next_cloud = Instant::now() + Duration::from_secs(10);
        }
    }
}

fn page(records: &[Record], input: &Value) -> Value {
    let offset = input["offset"]
        .as_u64()
        .unwrap_or(0)
        .min(records.len() as u64) as usize;
    let limit = input["limit"].as_u64().unwrap_or(10).clamp(1, 16) as usize;
    let end = (offset + limit).min(records.len());
    let rows: Vec<_> = records[offset..end]
        .iter()
        .map(|record| {
            json!({"id":record.id, "at":record.at, "title":record.title, "locked":record.locked,
            "profile":record.profile, "trigger":record.trigger, "bytes":record.bytes})
        })
        .collect();
    json!({"records":rows,"total":records.len(),"offset":offset,
        "next":(end < records.len()).then_some(end)})
}

fn string<'a>(input: &'a Value, key: &str) -> Result<&'a str> {
    input[key]
        .as_str()
        .with_context(|| format!("缺少文本参数 {key}"))
}

fn boolean(input: &Value, key: &str) -> Result<bool> {
    input[key]
        .as_bool()
        .with_context(|| format!("缺少开关参数 {key}"))
}

fn record_id(input: &Value) -> Result<Uuid> {
    Ok(Uuid::parse_str(string(input, "record_id")?)?)
}

#[derive(Default)]
struct Schedule {
    was_running: bool,
    due: Option<Instant>,
}

impl Schedule {
    fn observe(
        &mut self,
        running: bool,
        enabled: bool,
        period: Duration,
        now: Instant,
    ) -> Option<Trigger> {
        let previous = std::mem::replace(&mut self.was_running, running);
        if !enabled {
            self.due = None;
            return None;
        }
        if previous != running || (running && self.due.is_none()) {
            self.due = Some(
                now + if running {
                    period
                } else {
                    Duration::from_secs(6)
                },
            );
            return None;
        }
        let due = self.due?;
        if now < due {
            return None;
        }
        if running {
            self.due = Some(now + period);
            Some(Trigger::Scheduled)
        } else {
            self.due = None;
            Some(Trigger::Exit)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn large_libraries_return_bounded_summaries() {
        let records: Vec<_> = (0..1500)
            .map(|_| Record {
                format: 1,
                id: Uuid::new_v4(),
                game_id: game::GAME_ID.into(),
                profile: "76561198000000001".into(),
                at: chrono::Utc::now(),
                trigger: Trigger::Manual,
                title: "中文".repeat(40),
                note: "大".repeat(2000),
                locked: false,
                bytes: 1,
                sha256: String::new(),
                content_sha256: String::new(),
            })
            .collect();
        let last = page(&records, &json!({"offset":1494,"limit":6}));
        assert_eq!(last["records"].as_array().unwrap().len(), 6);
        assert_eq!(last["total"], 1500);
        assert!(last["next"].is_null());
        assert!(last["records"][0].get("note").is_none());
    }

    #[test]
    fn schedule_coalesces_missed_checks_and_runs_once_after_exit() {
        let mut schedule = Schedule::default();
        let now = Instant::now();
        let period = Duration::from_secs(600);
        assert_eq!(schedule.observe(false, true, period, now), None);
        assert_eq!(schedule.observe(true, true, period, now), None);
        assert_eq!(
            schedule.observe(true, true, period, now + period * 3),
            Some(Trigger::Scheduled)
        );
        assert_eq!(schedule.observe(true, true, period, now + period * 3), None);
        assert_eq!(
            schedule.observe(false, true, period, now + period * 3),
            None
        );
        let after_exit = now + period * 3 + Duration::from_secs(6);
        assert_eq!(
            schedule.observe(false, true, period, after_exit),
            Some(Trigger::Exit)
        );
        assert_eq!(schedule.observe(false, true, period, after_exit), None);
    }
}
