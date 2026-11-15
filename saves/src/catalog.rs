use std::path::{Path, PathBuf};

use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};

use crate::{
    game::{self, CustomGame},
    storage::{self, Settings},
};

#[derive(Clone, Default, Deserialize, Serialize)]
#[serde(default)]
pub struct Catalog {
    #[serde(flatten)]
    pub builtin: Settings,
    pub additional: Vec<RegisteredGame>,
    pub selected: usize,
    pub enabled: bool,
}

#[derive(Clone, Deserialize, Serialize)]
pub struct RegisteredGame {
    pub game: CustomGame,
    pub settings: Settings,
}

impl Catalog {
    pub fn load(root: &Path) -> Result<Self> {
        let mut catalog: Self = if storage::settings_file(root).try_exists()? {
            storage::read_json(&storage::settings_file(root)).context("设置文件无法读取")?
        } else {
            Self::default()
        };
        if catalog.builtin.backup_dir.as_os_str().is_empty() {
            catalog.builtin.backup_dir = root.join("Backups");
        }
        if catalog.selected > catalog.additional.len() {
            catalog.selected = 0;
        }
        Ok(catalog)
    }

    pub fn save(&self, root: &Path) -> Result<()> {
        storage::write_json(&storage::settings_file(root), self)
    }

    pub fn games(&self) -> Vec<Settings> {
        std::iter::once(self.builtin.clone())
            .chain(self.additional.iter().map(|entry| {
                let mut settings = entry.settings.clone();
                settings.dark = self.builtin.dark;
                settings.hotkey.clone_from(&self.builtin.hotkey);
                settings.custom = Some(entry.game.clone());
                settings
            }))
            .collect()
    }

    pub fn current(&self) -> Settings {
        self.games().swap_remove(self.selected)
    }

    pub fn names(&self) -> Vec<String> {
        std::iter::once("影之刃零".to_owned())
            .chain(self.additional.iter().map(|entry| entry.game.title.clone()))
            .collect()
    }

    pub fn update_current(&mut self, mut settings: Settings) {
        self.builtin.dark = settings.dark;
        self.builtin.hotkey.clone_from(&settings.hotkey);
        settings.custom = None;
        if self.selected == 0 {
            self.builtin = settings;
        } else {
            self.additional[self.selected - 1].settings = settings;
        }
    }

    pub fn add(&mut self, app_id: u32, executable: &str, save_dir: PathBuf) -> Result<()> {
        ensure!(
            !self
                .additional
                .iter()
                .any(|item| item.game.app_id == app_id),
            "这款游戏已经添加，请在游戏列表中选择。"
        );
        let game = CustomGame::find(app_id, executable)?;
        ensure!(
            !game::matches_process(None, std::slice::from_ref(&game.executable))
                && !self
                    .additional
                    .iter()
                    .any(|item| item.game.executable == game.executable),
            "已有游戏使用相同的进程名。请填写实际运行游戏的可执行文件名。"
        );
        let mut settings = Settings {
            save_dir,
            backup_dir: self.builtin.backup_dir.clone(),
            custom: Some(game.clone()),
            ..Settings::default()
        };
        let save_dir = game::resolve_save(&settings)?;
        let backup_dir = storage::backup_location(&save_dir, &settings.backup_dir)?;
        self.check_overlap(&save_dir, &backup_dir, None)?;
        settings.save_dir = save_dir;
        settings.backup_dir = backup_dir;
        settings.custom = None;
        self.additional.push(RegisteredGame { game, settings });
        self.selected = self.additional.len();
        Ok(())
    }

    pub fn check_overlap(
        &self,
        save_dir: &Path,
        backup_dir: &Path,
        exclude: Option<usize>,
    ) -> Result<()> {
        let backup_catalog = backup_dir.join(storage::CATALOG);
        for (index, game) in self.games().iter().enumerate() {
            if exclude == Some(index) || game.save_dir.as_os_str().is_empty() {
                continue;
            }
            if let Ok(other) = crate::game::resolve_save(game) {
                ensure!(
                    !storage::within(save_dir, &other) && !storage::within(&other, save_dir),
                    "此目录与另一款游戏的存档位置重叠。"
                );
                ensure!(
                    !storage::within(&backup_catalog, &other)
                        && !storage::within(&other, &backup_catalog),
                    "备份库与另一款游戏的存档位置重叠。"
                );
                if let Ok(backup) = storage::backup_location(&other, &game.backup_dir) {
                    let catalog = backup.join(storage::CATALOG);
                    ensure!(
                        !storage::within(&catalog, save_dir)
                            && !storage::within(save_dir, &catalog),
                        "存档位置与另一款游戏的备份库重叠。"
                    );
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_legacy_settings_and_preserves_per_game_rules() -> Result<()> {
        let root = tempfile::tempdir()?;
        let root = root.path();
        let legacy = Settings {
            interval_minutes: 17,
            ..Settings::default()
        };
        storage::write_json(&storage::settings_file(root), &legacy)?;
        let mut catalog = Catalog::load(root)?;
        assert_eq!(catalog.current().interval_minutes, 17);
        catalog.additional.push(RegisteredGame {
            game: CustomGame {
                app_id: 42,
                title: "测试".into(),
                executable: "test.exe".into(),
            },
            settings: Settings {
                interval_minutes: 5,
                ..Settings::default()
            },
        });
        catalog.selected = 1;
        catalog.save(root)?;
        let loaded = Catalog::load(root)?;
        assert_eq!(loaded.current().interval_minutes, 5);
        assert_eq!(loaded.builtin.interval_minutes, 17);
        assert_eq!(loaded.current().custom.unwrap().app_id, 42);
        Ok(())
    }
}
