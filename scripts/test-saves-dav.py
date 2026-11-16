"""使用回环 WebDAV 验证 C++ 存档工作进程。"""

import argparse
import base64
import contextlib
import ctypes as c
from ctypes import wintypes as w
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from threading import Thread
from urllib.parse import urlsplit
import uuid
from xml.sax.saxutils import escape


class Worker:
    def __init__(self, exe, root, env=None):
        self.process = subprocess.Popen(
            [str(exe), "--data-dir", str(root)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            encoding="utf-8", env=env, creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.sequence = 0

    def __enter__(self):
        try:
            assert self.ask("hello")["protocol"] == 1
        except BaseException:
            self.close()
            raise
        return self

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise
        finally:
            self.process.stdout.close()

    def __exit__(self, *_):
        self.close()

    def ask(self, command, fail=False, **arguments):
        self.sequence += 1
        request = dict(version=1, id=self.sequence, command=command)
        request.update(request_id=str(uuid.uuid4()))
        request.update(arguments)
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        reply = json.loads(self.process.stdout.readline())
        assert reply["version"] == 1 and reply["id"] == self.sequence
        assert reply["ok"] != fail, reply.get("error", "预期关闭操作")
        return reply["error"] if fail else reply["result"]


class Credential(c.Structure):
    _fields_ = [
        ("flags", w.DWORD), ("type", w.DWORD), ("target", w.LPWSTR), ("comment", w.LPWSTR),
        ("last_written", w.FILETIME), ("size", w.DWORD), ("blob", c.POINTER(c.c_ubyte)),
        ("persist", w.DWORD), ("count", w.DWORD), ("attributes", c.c_void_p),
        ("alias", w.LPWSTR), ("user", w.LPWSTR),
    ]


@contextlib.contextmanager
def session_credential(target, user, password):
    advapi = c.WinDLL("Advapi32.dll", use_last_error=True)
    blob = (c.c_ubyte * (2 * len(password))).from_buffer_copy(password.encode("utf-16-le"))
    credential = Credential(type=1, target=target, size=len(blob), blob=blob, persist=1, user=user)
    advapi.CredWriteW.argtypes = [c.POINTER(Credential), w.DWORD]
    advapi.CredDeleteW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD]
    if not advapi.CredWriteW(c.byref(credential), 0):
        raise c.WinError(c.get_last_error())
    try:
        yield
    finally:
        if not advapi.CredDeleteW(target, 1, 0):
            raise c.WinError(c.get_last_error())


class Dav(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def respond(self, status, body=b""):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def authorized(self):
        if self.headers.get("Authorization") == self.server.authorization:
            return True
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="fixture"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def do_PROPFIND(self):
        if not self.authorized():
            return
        self.body()
        if self.path not in self.server.collections:
            self.respond(404)
            return
        prefix = self.path.rstrip("/") + "/"
        children = [path for path in self.server.files.keys() | self.server.collections
                    if path != prefix and path.startswith(prefix) and "/" not in path[len(prefix):].rstrip("/")]
        document = '<D:multistatus xmlns:D="DAV:">' + "".join(
            f"<D:response><D:href>{escape(path)}</D:href></D:response>" for path in [prefix, *children]
        ) + "</D:multistatus>"
        self.respond(207, document.encode())

    def do_MKCOL(self):
        if not self.authorized():
            return
        if self.path in self.server.collections:
            self.respond(405)
            return
        self.server.collections.add(self.path)
        self.respond(201)

    def do_GET(self):
        if self.authorized():
            content = self.server.files.get(self.path)
            self.respond(404 if content is None else 200, content or b"")

    do_HEAD = do_GET

    def do_PUT(self):
        if self.authorized():
            self.server.files[self.path] = self.body()
            self.respond(201)

    def do_MOVE(self):
        if not self.authorized():
            return
        destination = urlsplit(self.headers["Destination"]).path
        if destination.endswith(".json") and self.server.fail_marker:
            self.server.fail_marker = False
            self.respond(503)
            return
        if destination in self.server.files and self.headers["Overwrite"] == "F":
            self.respond(412)
            return
        self.server.files[destination] = self.server.files.pop(self.path)
        self.respond(201)

    def do_DELETE(self):
        if self.authorized():
            self.server.files.pop(self.path, None)
            self.respond(204)


def verify(exe, fixture, server, remote_path, user, password):
    target = {"game_id": "steam-4115450", "profile": "76561198000000001"}
    slot = fixture / "影之刃零" / target["profile"]
    slot.mkdir(parents=True)
    (slot / "PBZSaveGame.sav").write_bytes(b"fixture version 1")
    directory = fixture / "worker"
    url = f"http://127.0.0.1:{server.server_port}{remote_path}"
    with Worker(exe, directory) as worker:
        worker.ask("saves.configure", **(target | {"profile": "", "save_dir": str(slot), "backup_dir": "",
                   "enabled": False, "hotkey": "", "interval_minutes": 10, "keep_auto": 100, "auto_enabled": True}))
        config = worker.ask("saves.cloud.configure", url=url, user=user, automatic=False)
        assert "401" in worker.ask("saves.cloud.test.enqueue", fail=True)
    with session_credential(config["credential_target"], user, password):
        with Worker(exe, directory) as worker:
            worker.ask("saves.cloud.test.enqueue")
            record = worker.ask("saves.backup.enqueue", **target)["record_id"]
            assert "503" in worker.ask("saves.cloud.upload.enqueue", fail=True, record_id=record, **target)
            zip_name = remote_path + f"SaveKeep/{target['game_id']}/{target['profile']}/{record}.zip"
            marker = zip_name[:-4] + ".json"
            assert zip_name in server.files and marker not in server.files
            payload = server.files[zip_name]
        with Worker(exe, directory) as worker:
            worker.ask("saves.cloud.upload.enqueue", record_id=record, **target)
            assert json.loads(server.files[marker])["id"] == record
            worker.ask("saves.records.edit", record_id=record, title="中文标题", note="测试备注", **target)
            worker.ask("saves.cloud.upload.enqueue", record_id=record, **target)
            assert json.loads(server.files[marker])["title"] == "中文标题" and server.files[zip_name] == payload
            worker.ask("saves.records.delete", record_id=record, confirmed=True, **target)
            assert zip_name in server.files and marker in server.files
            assert worker.ask("saves.cloud.list.enqueue", **target)["total"] == 1
            server.files[zip_name] = b"tampered"
            worker.ask("saves.cloud.download.enqueue", fail=True, record_id=record, **target)
            server.files[zip_name] = payload
            worker.ask("saves.cloud.download.enqueue", record_id=record, **target)
            worker.ask("saves.records.verify", record_id=record, **target)
            assert password not in json.dumps(worker.ask("saves.status"))
    assert password not in (directory / "webdav.json").read_text(encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", type=Path, default=Path("build/core/bin/Release/GameGauge.Saves.exe"))
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / "build/runtime-test"
    root.mkdir(parents=True, exist_ok=True)
    fixture = Path(tempfile.mkdtemp(prefix="saves-dav-", dir=root))
    remote_path = f"/fixture-{uuid.uuid4()}/"
    user = "fixture"
    password = uuid.uuid4().hex
    with ThreadingHTTPServer(("127.0.0.1", 0), Dav) as server:
        server.files = {}
        server.collections = {remote_path}
        server.fail_marker = True
        server.authorization = "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            verify(options.worker.resolve(), fixture, server, remote_path, user, password)
        finally:
            server.shutdown()
            thread.join()
    shutil.rmtree(fixture)
    print("WebDAV auth, content/manifest ordering, restart, metadata, remote listing and verified download passed.")


if __name__ == "__main__":
    main()
