use std::{
    fs::File,
    io::{Read, Write},
    path::Path,
    time::Duration,
};

use anyhow::{Context, Result, anyhow, ensure};
use reqwest::{
    Method, StatusCode, Url,
    blocking::{Body, Client, RequestBuilder, Response},
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use uuid::Uuid;

use crate::{
    archive::{self, Library},
    storage::Record,
};

const FOLDER: &str = "SaveKeep";
const MAX_MANIFEST: u64 = 64 * 1024;
const MAX_INDEX: u64 = 8 * 1024 * 1024;

#[derive(Clone, Default, Deserialize, Serialize)]
pub struct Config {
    pub url: String,
    pub user: String,

    pub automatic: bool,
}

impl Config {
    pub fn from_input(url: &str, user: &str, automatic: bool) -> Result<Self> {
        if url.trim().is_empty() {
            return Ok(Self::default());
        }
        let url = endpoint(url.trim())?.to_string();
        let user = user.trim().to_owned();
        ensure!(
            user.len() <= 512 && !user.contains(':') && !user.chars().any(char::is_control),
            "WebDAV 用户名无效。"
        );
        Ok(Self {
            url,
            user,

            automatic,
        })
    }

    pub fn credential_target(&self) -> String {
        if self.url.is_empty() || self.user.is_empty() {
            return String::new();
        }
        format!("GameGauge/Saves/WebDAV/{}", self.destination())
    }

    pub fn destination(&self) -> String {
        let mut hash = Sha256::new();
        hash.update(self.url.as_bytes());
        hash.update([0]);
        hash.update(self.user.as_bytes());
        format!("{:x}", hash.finalize())
    }
}

fn endpoint(input: &str) -> Result<Url> {
    let mut url = Url::parse(input).map_err(|_| anyhow!("请输入有效的 WebDAV URL。"))?;
    let loopback = matches!(url.host_str(), Some("127.0.0.1" | "localhost" | "[::1]"));
    ensure!(
        url.scheme() == "https" || (url.scheme() == "http" && loopback),
        "WebDAV 需要 HTTPS。仅本机回环地址可以用 HTTP。"
    );
    ensure!(
        url.host_str().is_some()
            && !url.cannot_be_a_base()
            && url.username().is_empty()
            && url.password().is_none()
            && url.query().is_none()
            && url.fragment().is_none(),
        "URL 中不能包含凭据、查询参数或片段。"
    );
    if !url.path().ends_with('/') {
        url.path_segments_mut()
            .map_err(|_| anyhow!("WebDAV URL 无效"))?
            .push("");
    }
    Ok(url)
}

fn stored_password(target: &str) -> Result<String> {
    use windows::{
        Win32::Security::Credentials::{CRED_TYPE_GENERIC, CREDENTIALW, CredFree, CredReadW},
        core::PCWSTR,
    };
    if target.is_empty() {
        return Ok(String::new());
    }
    let target: Vec<_> = target.encode_utf16().chain([0]).collect();
    let mut credential = std::ptr::null_mut::<CREDENTIALW>();
    unsafe {
        if let Err(error) = CredReadW(
            PCWSTR(target.as_ptr()),
            CRED_TYPE_GENERIC,
            Some(0),
            &mut credential,
        ) {
            if error.code().0 as u32 == 0x80070490 {
                return Ok(String::new());
            }
            return Err(anyhow!("无法读取 Windows WebDAV 凭据"));
        }
        let size = (*credential).CredentialBlobSize as usize;
        let mut chars = Vec::new();
        if size > 0 && size.is_multiple_of(2) && size <= 2560 {
            let bytes = std::slice::from_raw_parts((*credential).CredentialBlob, size);
            chars.extend(
                bytes
                    .chunks_exact(2)
                    .map(|pair| u16::from_le_bytes([pair[0], pair[1]])),
            );
            std::ptr::write_bytes((*credential).CredentialBlob, 0, size);
        }
        CredFree(credential.cast());
        let password = String::from_utf16(&chars).map_err(|_| anyhow!("Windows WebDAV 凭据损坏"));
        chars.fill(0);
        password
    }
}

pub struct Remote {
    client: Client,
    root: Url,
    user: String,
    password: String,
}

impl Remote {
    pub fn new(config: &Config) -> Result<Self> {
        let root = endpoint(&config.url)?;
        let mut builder = Client::builder()
            .connect_timeout(Duration::from_secs(5))
            .timeout(Duration::from_secs(60))
            .redirect(reqwest::redirect::Policy::none());
        if root.scheme() == "http" {
            builder = builder.no_proxy();
        }
        Ok(Self {
            client: builder
                .build()
                .map_err(|_| anyhow!("无法初始化 WebDAV 客户端"))?,
            root,
            user: config.user.clone(),
            password: stored_password(&config.credential_target())?,
        })
    }

    fn url(&self, parts: &[&str], collection: bool) -> Url {
        let mut url = self.root.clone();
        let mut segments = url.path_segments_mut().expect("已经验证 WebDAV URL");
        segments.pop_if_empty().extend(parts.iter().copied());
        if collection {
            segments.push("");
        }
        drop(segments);
        url
    }

    fn request(&self, method: &str, url: Url) -> RequestBuilder {
        let request = self.client.request(
            Method::from_bytes(method.as_bytes()).expect("固定 HTTP 方法"),
            url,
        );
        if self.user.is_empty() && self.password.is_empty() {
            request
        } else {
            request.basic_auth(&self.user, Some(&self.password))
        }
    }

    fn send(&self, method: &str, request: RequestBuilder) -> Result<Response> {
        request.send().map_err(|error| {
            let detail = if error.is_timeout() {
                "请求超时"
            } else if error.is_connect() {
                "无法连接，请检查地址、网络和证书"
            } else {
                "传输中断"
            };
            anyhow!("WebDAV {method}：{detail}")
        })
    }

    fn propfind(&self, url: Url, depth: &'static str) -> Result<Response> {
        self.send("PROPFIND", self.request("PROPFIND", url).header("Depth", depth)
            .header("Content-Type", "application/xml; charset=utf-8")
            .body(r#"<?xml version="1.0"?><propfind xmlns="DAV:"><prop><resourcetype/></prop></propfind>"#))
    }

    pub fn test(&self) -> Result<()> {
        let response = self.propfind(self.root.clone(), "0")?;
        check("PROPFIND", response.status(), &[StatusCode::MULTI_STATUS])
    }

    fn ensure_collection(&self, parts: &[&str]) -> Result<()> {
        let url = self.url(parts, true);
        let status = self
            .send("MKCOL", self.request("MKCOL", url.clone()))?
            .status();
        if status == StatusCode::METHOD_NOT_ALLOWED {
            return check(
                "PROPFIND",
                self.propfind(url, "0")?.status(),
                &[StatusCode::MULTI_STATUS],
            );
        }
        check("MKCOL", status, &[StatusCode::CREATED])
    }

    fn ensure_profile(&self, game: &str, profile: &str) -> Result<()> {
        self.ensure_collection(&[FOLDER])?;
        self.ensure_collection(&[FOLDER, game])?;
        self.ensure_collection(&[FOLDER, game, profile])
    }

    fn get(&self, game: &str, profile: &str, name: &str) -> Result<Response> {
        let response = self.send(
            "GET",
            self.request("GET", self.url(&[FOLDER, game, profile, name], false)),
        )?;
        check("GET", response.status(), &[StatusCode::OK])?;
        Ok(response)
    }

    fn publish(&self, record: &Record, name: &str, body: Body, overwrite: bool) -> Result<bool> {
        let pending = format!("{}.part", Uuid::new_v4());
        let context = [FOLDER, &record.game_id, &record.profile];
        let temporary = self.url(&[context[0], context[1], context[2], &pending], false);
        let destination = self.url(&[context[0], context[1], context[2], name], false);
        let result = (|| {
            let uploaded = self.send("PUT", self.request("PUT", temporary.clone()).body(body))?;
            check(
                "PUT",
                uploaded.status(),
                &[StatusCode::CREATED, StatusCode::OK, StatusCode::NO_CONTENT],
            )?;
            let moved = self.send(
                "MOVE",
                self.request("MOVE", temporary.clone())
                    .header("Destination", destination.as_str())
                    .header("Overwrite", if overwrite { "T" } else { "F" }),
            )?;
            if !overwrite && moved.status() == StatusCode::PRECONDITION_FAILED {
                return Ok(false);
            }
            check(
                "MOVE",
                moved.status(),
                &[StatusCode::CREATED, StatusCode::NO_CONTENT, StatusCode::OK],
            )?;
            Ok(true)
        })();
        if !matches!(result, Ok(true)) {
            // 只回收此次传输的随机临时对象。
            let _ = self.send("DELETE", self.request("DELETE", temporary));
        }
        result
    }

    pub fn upload(&self, record: &Record, archive: &Path, metadata_only: bool) -> Result<()> {
        self.ensure_profile(&record.game_id, &record.profile)?;
        let zip = format!("{}.zip", record.id);
        let existing = self.send(
            "GET",
            self.request(
                "GET",
                self.url(&[FOLDER, &record.game_id, &record.profile, &zip], false),
            ),
        )?;
        if existing.status() == StatusCode::NOT_FOUND {
            archive::verify_archive(archive, record)?;
            let body = Body::sized(File::open(archive)?, record.bytes);
            if !self.publish(record, &zip, body, false)? {
                self.verify_remote_zip(self.get(&record.game_id, &record.profile, &zip)?, record)?;
            }
        } else {
            check("GET", existing.status(), &[StatusCode::OK])?;
            // 已有回执代表本端曾校验过同一版本，改名时无需重新下载 ZIP。
            if !metadata_only
                || existing
                    .content_length()
                    .is_some_and(|size| size != record.bytes)
            {
                self.verify_remote_zip(existing, record)?;
            }
        }
        let marker = format!("{}.json", record.id);
        self.publish(
            record,
            &marker,
            Body::from(serde_json::to_vec(record)?),
            true,
        )?;
        Ok(())
    }

    fn verify_remote_zip(&self, response: Response, record: &Record) -> Result<()> {
        let mut hash = Sha256::new();
        let read = std::io::copy(&mut response.take(record.bytes + 1), &mut hash)?;
        ensure!(
            read == record.bytes && format!("{:x}", hash.finalize()) == record.sha256,
            "云端同 ID 的压缩包与本地不同，已停止覆盖。"
        );
        Ok(())
    }

    fn children(&self, parts: &[&str]) -> Result<Vec<String>> {
        let response = self.propfind(self.url(parts, true), "1")?;
        if response.status() == StatusCode::NOT_FOUND {
            return Ok(Vec::new());
        }
        check("PROPFIND", response.status(), &[StatusCode::MULTI_STATUS])?;
        let index = bounded_text(response, MAX_INDEX)?;
        let document =
            roxmltree::Document::parse(&index).context("WebDAV 目录响应不是有效的 XML")?;
        let names = document
            .descendants()
            .filter(|node| node.has_tag_name(("DAV:", "href")))
            .filter_map(|href| href.text())
            .filter_map(|path| path.trim_end_matches('/').rsplit('/').next())
            .map(str::to_owned)
            .collect();
        Ok(names)
    }

    pub fn list_accounts(&self, game: &str) -> Result<Vec<Record>> {
        let profiles: std::collections::BTreeSet<_> = self
            .children(&[FOLDER, game])?
            .into_iter()
            .filter(|name| name.len() == 17 && name.bytes().all(|byte| byte.is_ascii_digit()))
            .collect();
        let mut records = Vec::new();
        for profile in profiles {
            records.extend(self.list(game, &profile)?);
        }
        records.sort_by_key(|record| std::cmp::Reverse((record.at, record.id)));
        Ok(records)
    }

    pub fn list(&self, game: &str, profile: &str) -> Result<Vec<Record>> {
        let ids: std::collections::BTreeSet<_> = self
            .children(&[FOLDER, game, profile])?
            .iter()
            .filter_map(|name| name.strip_suffix(".json"))
            .filter_map(|id| Uuid::parse_str(id).ok())
            .collect();
        let mut records = Vec::with_capacity(ids.len());
        for id in ids {
            let name = format!("{id}.json");
            let record: Record = serde_json::from_str(&bounded_text(
                self.get(game, profile, &name)?,
                MAX_MANIFEST,
            )?)
            .context("云端备份清单无效")?;
            ensure!(
                record.id == id
                    && record.format == 1
                    && record.game_id == game
                    && record.profile == profile,
                "云端备份清单与游戏或账号不匹配。"
            );
            records.push(record);
        }
        records.sort_by_key(|record| std::cmp::Reverse((record.at, record.id)));
        Ok(records)
    }

    pub fn download(&self, library: &Library, record: &Record) -> Result<()> {
        let name = format!("{}.zip", record.id);
        let mut response = self.get(&library.game_id, &library.profile, &name)?;
        library.import(record, &mut response)
    }
}

fn bounded_text(response: Response, max: u64) -> Result<String> {
    let mut content = Vec::new();
    response.take(max + 1).read_to_end(&mut content)?;
    ensure!(content.len() as u64 <= max, "WebDAV 响应超过大小限制。");
    Ok(String::from_utf8(content)?)
}

fn check(method: &str, status: StatusCode, allowed: &[StatusCode]) -> Result<()> {
    ensure!(
        allowed.contains(&status),
        "WebDAV {method} 返回 HTTP {status}。"
    );
    Ok(())
}

pub fn metadata_hash(record: &Record) -> Result<String> {
    let mut hash = Sha256::new();
    hash.write_all(&serde_json::to_vec(record)?)?;
    Ok(format!("{:x}", hash.finalize()))
}

#[cfg(test)]
mod tests;
