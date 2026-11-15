use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use uuid::Uuid;

use crate::{game, storage};

pub struct Journal(PathBuf);

#[derive(Deserialize, Serialize)]
struct Entry {
    digest: String,
    outcome: Option<Outcome>,
}

#[derive(Deserialize, Serialize)]
enum Outcome {
    Completed(Value),
    Failed(String),
}

impl Journal {
    pub fn open(root: &Path) -> Result<Self> {
        let path = root.join("jobs");
        fs::create_dir_all(&path)?;
        game::plain_path(&path)?;
        Ok(Self(path))
    }

    pub fn lookup(&self, request: &Value) -> Result<Value> {
        let id = Uuid::parse_str(request["job_id"].as_str().context("缺少任务标识")?)?;
        let path = self.0.join(format!("{id}.json"));
        if !path.try_exists()? {
            return Ok(json!({"ok":false,"error":"找不到此存档任务"}));
        }
        let entry: Entry = storage::read_json(&path)?;
        Ok(match entry.outcome {
            Some(Outcome::Completed(result)) => {
                json!({"ok":true,"job_id":id,"state":"succeeded","result":result})
            }
            Some(Outcome::Failed(error)) => {
                json!({"ok":true,"job_id":id,"state":"failed","error":error})
            }
            None => json!({"ok":true,"job_id":id,"state":"interrupted",
                "error":"任务在完成前被中断，请检查存档目录与备份库。"}),
        })
    }

    pub fn execute(
        &self,
        request: &Value,
        action: impl FnOnce() -> Result<Value>,
    ) -> Result<Value> {
        if !matches!(
            request["command"].as_str(),
            Some(
                "saves.games.select"
                    | "saves.games.add"
                    | "saves.configure"
                    | "saves.backup.enqueue"
                    | "saves.records.edit"
                    | "saves.records.protect"
                    | "saves.records.delete"
                    | "saves.restore.enqueue"
                    | "saves.migration.apply"
                    | "saves.cloud.configure"
                    | "saves.cloud.download.enqueue"
                    | "saves.cloud.upload.enqueue"
            )
        ) {
            return action();
        }
        let id = Uuid::parse_str(
            request["request_id"]
                .as_str()
                .context("存档任务缺少请求标识")?,
        )?;
        let path = self.0.join(format!("{id}.json"));
        let mut operation = request.clone();
        operation.as_object_mut().unwrap().remove("id");
        let digest = format!("{:x}", Sha256::digest(serde_json::to_vec(&operation)?));
        if path.try_exists()? {
            let entry: Entry = storage::read_json(&path)?;
            ensure!(entry.digest == digest, "请求标识已用于另一个存档任务。");
            return match entry.outcome {
                Some(Outcome::Completed(result)) => Ok(result),
                Some(Outcome::Failed(error)) => bail!("{error}"),
                None => bail!("上次任务被中断。请先检查存档目录与备份库，再决定是否发起新任务。"),
            };
        }
        let mut entry = Entry {
            digest,
            outcome: None,
        };
        storage::write_json(&path, &entry)?;
        let result = action();
        entry.outcome = Some(match &result {
            Ok(value) => Outcome::Completed(value.clone()),
            Err(error) => Outcome::Failed(format!("{error:#}")),
        });
        storage::write_json(&path, &entry)
            .context("任务已执行，保存结果失败，请检查目录和备份库")?;
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn persistent_jobs_replay_results_and_do_not_repeat_interrupted_changes() -> Result<()> {
        let root = tempfile::tempdir()?;
        let journal = Journal::open(root.path())?;
        let id = Uuid::new_v4();
        let request =
            json!({"command":"saves.restore.enqueue", "request_id":id, "version":1, "id":2});
        assert_eq!(
            journal.execute(&request, || Ok(json!({"message":"restored"})))?,
            json!({"message":"restored"})
        );
        let restarted = Journal::open(root.path())?;
        let mut same = request.clone();
        same["id"] = json!(9);
        assert!(restarted.execute(&same, || panic!("不能重复恢复")).is_ok());
        let path = restarted.0.join(format!("{id}.json"));
        let mut entry: Entry = storage::read_json(&path)?;
        entry.outcome = None;
        storage::write_json(&path, &entry)?;
        assert!(
            restarted
                .execute(&same, || panic!("不能重放未知结果的恢复"))
                .is_err()
        );
        Ok(())
    }
}
