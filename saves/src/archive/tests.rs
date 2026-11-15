use super::*;

#[test]
fn failed_directory_swap_recovers_current_save() -> Result<()> {
    let (root, library) = fixture()?;
    let original = fs::read(library.save.join(game::MAIN_SAVE))?;
    let rollback = rollback_path(&library.save)?;
    let error =
        commit_restore(&library.save, &root.path().join("missing-stage"), &rollback).unwrap_err();
    assert!(error.to_string().contains("原始存档已恢复"));
    assert_eq!(fs::read(library.save.join(game::MAIN_SAVE))?, original);
    assert!(!rollback.exists());
    Ok(())
}

fn fixture() -> Result<(tempfile::TempDir, Library)> {
    let work = tempfile::tempdir()?;
    let save = work.path().join("76561198000000000");
    fs::create_dir_all(save.join("1RealTime"))?;
    fs::write(save.join(game::MAIN_SAVE), b"checkpoint one")?;
    fs::write(
        save.join("1RealTime").join("older.sav"),
        b"older checkpoint",
    )?;
    fs::write(save.join("steam_autocloud.vdf"), b"first marker")?;
    let library = Library::open(&storage::Settings {
        save_dir: save,
        backup_dir: work.path().join("备份位置"),
        ..Default::default()
    })?;
    Ok((work, library))
}

#[test]
fn snapshot_restore_preserves_unmanaged_files_and_creates_undo() -> Result<()> {
    let (_work, library) = fixture()?;
    let old = library.backup(Trigger::Manual)?.unwrap();
    fs::write(library.save.join(game::MAIN_SAVE), b"checkpoint two")?;
    fs::write(library.save.join("steam_autocloud.vdf"), b"current marker")?;
    fs::write(
        library.save.join("1RealTime").join("newer.sav"),
        b"newer checkpoint",
    )?;
    assert!(
        library
            .restore(&old)?
            .unwrap()
            .contains(".savekeep-preserved-")
    );
    assert_eq!(
        fs::read(library.save.join(game::MAIN_SAVE))?,
        b"checkpoint one"
    );
    assert_eq!(
        fs::read(library.save.join("steam_autocloud.vdf"))?,
        b"current marker"
    );
    assert!(library.save.join("1RealTime").join("older.sav").exists());
    assert!(!library.save.join("1RealTime").join("newer.sav").exists());
    let undo = library
        .records()?
        .into_iter()
        .find(|item| item.trigger == Trigger::BeforeRestore)
        .unwrap();
    assert!(undo.locked);
    library.restore(&undo)?;
    assert_eq!(
        fs::read(library.save.join(game::MAIN_SAVE))?,
        b"checkpoint two"
    );
    assert!(library.save.join("1RealTime").join("newer.sav").exists());
    Ok(())
}

#[test]
fn unchanged_auto_backup_is_skipped_and_pruning_keeps_manual_and_locked() -> Result<()> {
    let (_work, library) = fixture()?;
    let first = library.backup(Trigger::Scheduled)?.unwrap();
    assert!(library.backup(Trigger::Exit)?.is_none());
    assert!(library.backup(Trigger::Manual)?.is_some());
    for checkpoint in [b"next", b"last"] {
        fs::write(library.save.join(game::MAIN_SAVE), checkpoint)?;
        assert!(library.backup(Trigger::Scheduled)?.is_some());
    }
    let mut first = first;
    first.locked = true;
    storage::write_json(
        &library.root.join(first.id.to_string()).join("record.json"),
        &first,
    )?;
    library.prune_uploaded(1, |_| true)?;
    let records = library.records()?;
    assert_eq!(records.len(), 3);
    assert!(
        records
            .iter()
            .any(|record| record.id == first.id && record.locked)
    );
    assert!(
        records
            .iter()
            .any(|record| record.trigger == Trigger::Manual)
    );
    Ok(())
}

#[test]
fn altered_zip_cannot_replace_the_current_save() -> Result<()> {
    let (_work, library) = fixture()?;
    let backup = library.backup(Trigger::Manual)?.unwrap();
    let path = library.root.join(backup.id.to_string()).join(ARCHIVE);
    let mut payload = fs::read(&path)?;
    payload[8] ^= 1;
    fs::write(path, payload)?;
    fs::write(library.save.join(game::MAIN_SAVE), b"do not overwrite")?;
    assert!(library.restore(&backup).is_err());
    assert_eq!(
        fs::read(library.save.join(game::MAIN_SAVE))?,
        b"do not overwrite"
    );
    assert_eq!(library.records()?.len(), 1);
    Ok(())
}

#[test]
fn missing_save_directory_can_be_recreated_from_the_library() -> Result<()> {
    let (work, library) = fixture()?;
    let backup = library.backup(Trigger::Manual)?.unwrap();
    let original = work.path().join("lost-directory");
    fs::rename(&library.save, original)?;
    let library = Library::open(&storage::Settings {
        save_dir: library.save,
        backup_dir: work.path().join("备份位置"),
        ..Default::default()
    })?;
    assert!(library.restore(&backup)?.is_some());
    assert_eq!(
        fs::read(library.save.join(game::MAIN_SAVE))?,
        b"checkpoint one"
    );
    Ok(())
}

#[test]
fn archive_names_cannot_escape_or_target_windows_devices() {
    for invalid in [
        "../bad.sav",
        "1RealTime/../../bad.sav",
        "1RealTime/CON.sav",
        "1RealTime/file.sav:stream",
        "2Hourly/folder./bad.sav",
        "/bad.sav",
        "folder/bad.sav",
    ] {
        assert!(!managed_name(invalid, false), "{invalid}");
    }
    assert!(managed_name(game::MAIN_SAVE, false));
    assert!(managed_name("1RealTime/日期.sav", false));
}

#[test]
fn backup_storage_cannot_be_inside_the_save() -> Result<()> {
    let (_work, library) = fixture()?;
    assert!(storage::backup_location(&library.save, &library.save.join("nested")).is_err());
    assert!(!library.save.join("nested").exists());
    Ok(())
}

#[test]
fn custom_games_have_independent_libraries_and_restore_nested_files() -> Result<()> {
    let (work, builtin) = fixture()?;
    let save = work.path().join("custom slot");
    fs::create_dir_all(save.join("slots"))?;
    fs::write(save.join("slots/game.dat"), b"first")?;
    fs::write(save.join("steam_autocloud.vdf"), b"marker")?;
    let settings = storage::Settings {
        save_dir: save.clone(),
        backup_dir: work.path().join("备份位置"),
        custom: Some(game::CustomGame {
            app_id: 42,
            title: "隔离测试游戏".into(),
            executable: "savekeep-test-unrunning.exe".into(),
        }),
        ..Default::default()
    };
    let custom = Library::open(&settings)?;
    let first = custom.backup(Trigger::Scheduled)?.unwrap();
    fs::write(save.join("slots/game.dat"), b"next")?;
    custom.backup(Trigger::Scheduled)?.unwrap();
    custom.prune_uploaded(1, |_| false)?;
    assert_eq!(custom.records()?.len(), 2);
    assert!(builtin.records()?.is_empty());
    custom.restore(&first)?;
    assert_eq!(fs::read(save.join("slots/game.dat"))?, b"first");
    assert_eq!(fs::read(save.join("steam_autocloud.vdf"))?, b"marker");
    Ok(())
}
