use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::{BufRead, BufReader},
    net::{TcpListener, TcpStream},
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread,
};

use super::*;
use crate::{
    game::MAIN_SAVE,
    storage::{Settings, Trigger},
};

struct Fixture {
    address: String,
    data: Arc<Mutex<Server>>,
    stop: Arc<AtomicBool>,
    worker: Option<thread::JoinHandle<()>>,
}

impl Fixture {
    fn start() -> Result<Self> {
        let listener = TcpListener::bind("127.0.0.1:0")?;
        listener.set_nonblocking(true)?;
        let address = listener.local_addr()?.to_string();
        let data = Arc::new(Mutex::new(Server {
            collections: BTreeSet::from(["/dav/".to_owned()]),
            reject_manifest_once: true,
            ..Default::default()
        }));
        let stop = Arc::new(AtomicBool::new(false));
        let worker = {
            let state = data.clone();
            let stop = stop.clone();
            thread::spawn(move || {
                while !stop.load(Ordering::Relaxed) {
                    match listener.accept() {
                        Ok((stream, _)) => {
                            let _ = serve(stream, &state);
                        }
                        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                            thread::sleep(Duration::from_millis(5))
                        }
                        Err(_) => break,
                    }
                }
            })
        };
        Ok(Self {
            address,
            data,
            stop,
            worker: Some(worker),
        })
    }

    fn url(&self) -> String {
        format!("http://{}/dav/", self.address)
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[derive(Default)]
struct Server {
    collections: BTreeSet<String>,
    files: BTreeMap<String, Vec<u8>>,
    reject_manifest_once: bool,
    zip_writes: usize,
}

fn serve(mut stream: TcpStream, shared: &Mutex<Server>) -> Result<()> {
    stream.set_nonblocking(false)?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    let mut reader = BufReader::new(&mut stream);
    let mut line = String::new();
    reader.read_line(&mut line)?;
    let mut request = line.split_whitespace();
    let method = request.next().context("method")?.to_owned();
    let path = request.next().context("path")?.to_owned();
    let mut headers = BTreeMap::new();
    loop {
        line.clear();
        if reader.read_line(&mut line)? == 0 || line == "\r\n" {
            break;
        }
        if let Some((name, value)) = line.split_once(':') {
            headers.insert(name.to_ascii_lowercase(), value.trim().to_owned());
        }
    }
    let mut body = vec![
        0;
        headers
            .get("content-length")
            .map_or(0, |value| value.parse().unwrap())
    ];
    reader.read_exact(&mut body)?;
    let (status, response) = dispatch(&mut shared.lock().unwrap(), &method, &path, &headers, body);
    write!(
        stream,
        "HTTP/1.1 {status}\r\nConnection: close\r\nContent-Length: {}\r\n\r\n",
        response.len()
    )?;
    stream.write_all(&response)?;
    Ok(())
}

fn dispatch(
    server: &mut Server,
    method: &str,
    path: &str,
    headers: &BTreeMap<String, String>,
    body: Vec<u8>,
) -> (&'static str, Vec<u8>) {
    let empty = || Vec::new();
    match method {
        "PROPFIND" if server.collections.contains(path) => {
            let mut xml = String::from(r#"<D:multistatus xmlns:D="DAV:">"#);
            if headers.get("depth").is_some_and(|depth| depth == "1") {
                for url in server.collections.iter().filter(|url| {
                    url.strip_prefix(path).is_some_and(|tail| {
                        !tail.is_empty() && !tail.trim_end_matches('/').contains('/')
                    })
                }) {
                    xml.push_str(&format!("<D:response><D:href>{url}</D:href></D:response>"));
                }
                for url in server.files.keys().filter(|name| {
                    name.strip_prefix(path)
                        .is_some_and(|tail| !tail.contains('/'))
                }) {
                    xml.push_str(&format!("<D:response><D:href>{url}</D:href></D:response>"));
                }
            }
            xml.push_str("</D:multistatus>");
            ("207 Multi-Status", xml.into_bytes())
        }
        "MKCOL" if server.collections.insert(path.into()) => ("201 Created", empty()),
        "MKCOL" => ("405 Method Not Allowed", empty()),
        "PUT" => {
            server.files.insert(path.into(), body);
            ("201 Created", empty())
        }
        "MOVE" => {
            let destination = Url::parse(&headers["destination"])
                .unwrap()
                .path()
                .to_owned();
            if destination.ends_with(".json") && std::mem::take(&mut server.reject_manifest_once) {
                return ("503 Service Unavailable", empty());
            }
            if headers["overwrite"] == "F" && server.files.contains_key(&destination) {
                return ("412 Precondition Failed", empty());
            }
            if let Some(bytes) = server.files.remove(path) {
                if destination.ends_with(".zip") {
                    server.zip_writes += 1;
                }
                server.files.insert(destination, bytes);
                ("201 Created", empty())
            } else {
                ("404 Not Found", empty())
            }
        }
        "DELETE" => {
            server.files.remove(path);
            ("204 No Content", empty())
        }
        "HEAD" if server.files.contains_key(path) => ("200 OK", empty()),
        "GET" if server.files.contains_key(path) => ("200 OK", server.files[path].clone()),
        _ => ("404 Not Found", empty()),
    }
}

fn library(path: &Path, name: &str) -> Result<Library> {
    let save = path.join(name).join("76561198000000000");
    fs::create_dir_all(&save)?;
    fs::write(save.join(MAIN_SAVE), name.as_bytes())?;
    Library::open(&Settings {
        save_dir: save,
        backup_dir: path.join(format!("{name}-library")),
        ..Settings::default()
    })
}

#[test]
fn webdav_retries_commit_and_downloads_after_local_library_loss() -> Result<()> {
    let server = Fixture::start()?;
    let config = Config {
        url: server.url(),
        ..Config::default()
    };
    let remote = Remote::new(&config)?;
    remote.test()?;
    let root = tempfile::tempdir()?;
    let source = library(root.path(), "original checkpoint")?;
    let mut record = source.backup(Trigger::Manual)?.unwrap();
    let archive = source
        .root
        .join(record.id.to_string())
        .join(archive::ARCHIVE);
    assert!(remote.upload(&record, &archive, false).is_err());
    assert!(remote.list(&source.game_id, &source.profile)?.is_empty());
    remote.upload(&record, &archive, false)?;
    assert_eq!(
        remote.list_accounts(&source.game_id)?[0].profile,
        source.profile
    );
    record.title = "决战前".into();
    record.note = "保留这份进度".into();
    record.locked = true;
    remote.upload(&record, &archive, true)?;
    let fresh = library(root.path(), "fresh local library")?;
    assert!(fresh.records()?.is_empty());
    let listed = remote.list(&fresh.game_id, &fresh.profile)?;
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].note, record.note);
    assert!(listed[0].locked);
    remote.download(&fresh, &listed[0])?;
    assert_eq!(fresh.records()?[0].title, "决战前");
    fresh.restore(&fresh.records()?[0])?;
    assert_eq!(
        fs::read(fresh.save.join(MAIN_SAVE))?,
        b"original checkpoint"
    );
    let stored = server.data.lock().unwrap();
    assert_eq!(stored.zip_writes, 1);
    assert!(stored.files.keys().all(|path| !path.ends_with(".part")));
    Ok(())
}

#[test]
fn endpoint_and_password_handling() -> Result<()> {
    assert!(endpoint("http://example.com/dav/").is_err());
    assert!(endpoint("https://user:pass@example.com/dav/").is_err());
    assert!(endpoint("https://example.com/dav/?auth=token").is_err());
    assert_eq!(endpoint("https://example.com/dav")?.path(), "/dav/");
    let config = Config::from_input("https://example.com/dav", "user", true)?;
    let same = Config::from_input("https://example.com/dav/", "user", false)?;
    let different = Config::from_input("https://other.example.com/dav/", "user", false)?;
    assert_eq!(same.credential_target(), config.credential_target());
    assert_ne!(different.credential_target(), config.credential_target());
    assert!(!serde_json::to_string(&config)?.contains("secret"));
    Ok(())
}

#[test]
fn pending_items_survive_restart_and_are_released_only_after_receipt() -> Result<()> {
    use crate::{cloud::Cloud, storage};

    let server = Fixture::start()?;
    let app = tempfile::tempdir()?;
    let source = library(app.path(), "upload queue checkpoint")?;
    let record = source.backup(Trigger::Scheduled)?.unwrap();
    let targets = [Settings {
        save_dir: source.save.clone(),
        backup_dir: app.path().join("upload queue checkpoint-library"),
        ..Settings::default()
    }];
    storage::write_json(
        &app.path().join("webdav.json"),
        &Config {
            url: server.url(),
            automatic: true,
            ..Config::default()
        },
    )?;
    let mut first = Cloud::load(app.path())?;
    assert!(!first.may_prune(&source, &record));
    assert!(first.upload_next(&targets)?.contains("将自动重试"));
    assert!(!first.may_prune(&source, &record));
    drop(first);
    let mut restarted = Cloud::load(app.path())?;
    restarted.upload_next(&targets)?;
    assert!(restarted.may_prune(&source, &record));
    Ok(())
}
