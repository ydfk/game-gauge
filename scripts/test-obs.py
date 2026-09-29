"""Isolated OBS protocol and host lifecycle integration test (Python 3 stdlib)."""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build/core/bin/Release"
DATA = ROOT / "build/runtime-test/obs"
mode = "idle"
authenticated = threading.Event()


def exact(stream, size):
    data = b""
    while len(data) < size:
        part = stream.recv(size - len(data))
        if not part:
            raise EOFError()
        data += part
    return data


def receive(stream):
    header = exact(stream, 2)
    length = header[1] & 127
    if length == 126:
        length = struct.unpack("!H", exact(stream, 2))[0]
    if length == 127:
        length = struct.unpack("!Q", exact(stream, 8))[0]
    assert length < 65536 and header[1] & 128
    mask = exact(stream, 4)
    return json.loads(bytes(c ^ mask[i % 4] for i, c in enumerate(exact(stream, length))))


def send(stream, value):
    data = json.dumps(value).encode()
    # 分片消息验证客户端组装逻辑。
    for opcode, part in ((1, data[:30]), (128, data[30:])):
        header = bytes([opcode, len(part)]) if len(part) < 126 else bytes([opcode, 126]) + struct.pack("!H", len(part))
        stream.sendall(header + part)


def serve(listener):
    stream, _ = listener.accept()
    with stream:
        request = b""
        while b"\r\n\r\n" not in request:
            request += exact(stream, 1)
        key = next(line.split(b":", 1)[1].strip() for line in request.split(b"\r\n") if line.lower().startswith(b"sec-websocket-key:"))
        accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
        stream.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        send(stream, {"op": 0, "d": {"rpcVersion": 1, "authentication": {"salt": "salt", "challenge": "challenge"}}})
        auth = receive(stream)
        digest = lambda s: base64.b64encode(hashlib.sha256(s.encode()).digest()).decode()
        assert auth["d"]["authentication"] == digest(digest("test-passwordsalt") + "challenge")
        assert auth["d"]["eventSubscriptions"] == 0
        authenticated.set()
        send(stream, {"op": 2, "d": {"negotiatedRpcVersion": 1}})
        try:
            while True:
                request = receive(stream)
                assert request["d"]["requestType"] == "GetRecordStatus"
                if mode == "hang":
                    time.sleep(5)
                    return
                send(stream, {"op": 7, "d": {"requestId": request["d"]["requestId"], "requestStatus": {"result": True},
                    "responseData": {"outputActive": mode != "idle", "outputPaused": mode == "paused"}}})
        except (EOFError, OSError):
            pass


def diagnostic(*args):
    result = subprocess.run([str(BIN / "GameGauge.Diagnostics.exe"), *args], capture_output=True, encoding="utf-8", timeout=5)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return json.loads(result.stdout)


def wait_state(expected):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        try:
            status = diagnostic("--host-status")
            if status["snapshot"]["obs_state"] == expected:
                return status
        except (RuntimeError, KeyError):
            pass
        time.sleep(.15)
    raise AssertionError(f"OBS never reached {expected}")


if __name__ == "__main__":
    try:
        diagnostic("--host-status")
    except RuntimeError:
        pass
    else:
        raise SystemExit("Exit the running GameGauge before this isolated test.")
    DATA.mkdir(parents=True, exist_ok=True)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        config = DATA / "obs-studio/plugin_config/obs-websocket/config.json"
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text(json.dumps({"server_enabled": True, "server_port": listener.getsockname()[1], "server_password": "test-password"}))
        thread = threading.Thread(target=serve, args=(listener,), daemon=True)
        thread.start()
        env = dict(os.environ, APPDATA=str(DATA))
        host = subprocess.Popen([str(BIN / "GameGauge.exe"), "--background", "--data-dir", str(DATA / "gauge")], env=env)
        try:
            for state, label in (("idle", "未录制"), ("recording", "● 录制中"), ("paused", "Ⅱ 已暂停")):
                mode = state
                status = wait_state(state)
                assert any(item["label"] == "OBS" and item["value"] == label for item in status["hud_preview"])
            assert authenticated.is_set()
            mode = "hang"
            wait_state("disconnected")
            started = time.monotonic()
            diagnostic("--request", '{"command":"quit"}')
            assert host.wait(timeout=5) == 0
            print(f"OBS authentication, fragmented messages, idle/recording/paused, timeout and exit passed ({time.monotonic() - started:.2f}s).")
        finally:
            if host.poll() is None:
                host.terminate()
                host.wait(timeout=5)
