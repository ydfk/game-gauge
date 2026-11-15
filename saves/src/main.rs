#![windows_subsystem = "windows"]

mod archive;
mod catalog;
mod cloud;
mod dav;
mod game;
mod journal;
mod migration;
mod storage;
mod worker;

use std::{
    fs::OpenOptions,
    io::{self, BufRead, Write},
    os::windows::fs::OpenOptionsExt,
    path::PathBuf,
    sync::mpsc,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};

const PROTOCOL: u64 = 1;
const MAX_REQUEST: usize = 32 * 1024;
const MAX_RESPONSE: usize = 96 * 1024;
const VERSION: &str = env!("GAMEGAUGE_SAVES_VERSION");

fn main() {
    if run().is_err() {
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let mut args = std::env::args_os().skip(1);
    ensure!(args.next().is_some_and(|arg| arg == "--data-dir"));
    let root = PathBuf::from(args.next().context("缺少存档数据目录")?);
    ensure!(root.is_absolute() && args.next().is_none());
    std::fs::create_dir_all(&root)?;
    game::plain_path(&root)?;
    // 跨会话只允许一个工作进程修改同一个库。
    let _writer_lock = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(false)
        .share_mode(0)
        .open(root.join("worker.lock"))?;

    let mut output = io::stdout().lock();
    let mut input = io::stdin().lock();
    let hello = read_request(&mut input)?.context("缺少握手")?;
    ensure!(hello["command"] == "hello" && hello["version"] == PROTOCOL);
    drop(input);
    let mut service = worker::Worker::load(root.clone())?;
    let journal = journal::Journal::open(&root)?;
    respond(
        &mut output,
        hello["id"].clone(),
        Ok(json!({
            "protocol": PROTOCOL, "app_version": VERSION, "hotkey": service.hotkey(),
        })),
    )?;
    let (sender, receiver) = mpsc::sync_channel(1);
    std::thread::spawn(move || {
        let mut input = io::stdin().lock();
        while let Ok(Some(request)) = read_request(&mut input) {
            if sender.send(request).is_err() {
                break;
            }
        }
    });
    let mut next_tick = Instant::now();
    loop {
        match receiver.recv_timeout(next_tick.saturating_duration_since(Instant::now())) {
            Ok(request) => {
                ensure!(request["version"] == PROTOCOL && request["id"].is_u64());
                if request["command"] == "shutdown" {
                    break;
                }
                let result = if request["command"] == "lookup_job" {
                    journal.lookup(&request)
                } else {
                    journal.execute(&request, || service.request(&request))
                };
                respond(&mut output, request["id"].clone(), result)?;
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
        if Instant::now() >= next_tick {
            service.tick();
            next_tick = Instant::now() + Duration::from_secs(2);
        }
    }
    Ok(())
}

fn read_request(input: &mut impl BufRead) -> Result<Option<Value>> {
    use std::io::Read;
    let mut bytes = Vec::new();
    input
        .take((MAX_REQUEST + 1) as u64)
        .read_until(b'\n', &mut bytes)?;
    if bytes.is_empty() {
        return Ok(None);
    }
    ensure!(bytes.len() <= MAX_REQUEST && bytes.last() == Some(&b'\n'));
    Ok(Some(serde_json::from_slice(&bytes)?))
}

fn respond(output: &mut impl Write, id: Value, result: Result<Value>) -> Result<()> {
    let message = match result {
        Ok(value) => json!({"version": PROTOCOL, "id": id, "ok": true, "result": value}),
        Err(error) => {
            json!({"version": PROTOCOL, "id": id, "ok": false, "error": format!("{error:#}").chars().take(1000).collect::<String>()})
        }
    };
    let mut bytes = serde_json::to_vec(&message)?;
    if bytes.len() >= MAX_RESPONSE {
        bytes = serde_json::to_vec(
            &json!({"version": PROTOCOL, "id": id, "ok": false, "error": "响应过大，请缩小查询范围"}),
        )?;
    }
    output.write_all(&bytes)?;
    output.write_all(b"\n")?;
    output.flush()?;
    Ok(())
}
