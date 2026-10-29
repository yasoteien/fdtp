#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Loopback-only LocalSend acceptance checks; Python standard library only.

Run `make localsend-check` or `python3 -B tests/check_localsend.py`.
Fixtures and separate helper identities stay under build/localsend-check.
No test contacts a LAN interface or an external service.
"""
import argparse
import contextlib
import hashlib
import html
from html.parser import HTMLParser
import http.client as http_client
import json
import os
import traceback
from pathlib import Path
import queue
import re
import signal
import socket
import ssl
import subprocess
import tempfile
import threading
import time
from urllib.parse import quote, parse_qs, unquote, urlencode, urlsplit
import zipfile

APP = Path(__file__).resolve().parent.parent
PIN = "123456"
CTX = ssl._create_unverified_context()


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def until(predicate, message, seconds=6):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return
        threading.Event().wait(0.05)
    raise AssertionError(message)


class Helper:
    def __init__(self, suite, command, *args, home=None, port=None):
        self.port = free_port() if port is None else port
        self.home = home or suite.directory("home")
        env = os.environ.copy()
        env.update(HOME=str(self.home), FDTP_LOCALSEND_LOOPBACK="1")
        self.args = [str(suite.exe), command, "--port", str(self.port), *map(str, args)]
        self.process = subprocess.Popen(self.args, env=env, cwd=suite.root, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, encoding="utf-8", errors="replace", bufsize=1)
        self.lines = []
        self.events = queue.Queue()
        def read():
            for line in self.process.stdout:
                line = line.rstrip("\r\n")
                self.lines.append(line)
                self.events.put(line)
            self.events.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        suite.processes.append(self)

    def wait_line(self, prefix, seconds=12):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                line = self.events.get(timeout=max(0.01, end - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                break
            if line == prefix or line.startswith(prefix + "\t"):
                return line.split("\t")
        raise AssertionError(f"waiting for {prefix!r}: {self.args}: {self.lines}")

    def finish(self, seconds=20):
        try:
            code = self.process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            raise AssertionError(f"helper did not finish: {self.args}: {self.lines}") from None
        self.reader.join(timeout=2)
        return code, "\n".join(self.lines)

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout):
            stream.close()


class Suite:
    def __init__(self, exe, build):
        self.exe = exe.resolve()
        base = build / "localsend-check"
        base.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix="run-", dir=base))
        self.count = 0
        self.processes = []

    def directory(self, name):
        self.count += 1
        p = self.root / f"{self.count:02d}-{name}"
        p.mkdir()
        return p

    @contextlib.contextmanager
    def receiver(self, ask=False, home=None, port=None):
        dest = self.directory("received")
        args = ["--alias", "受取試験", "--dir", dest, "--pin", PIN]
        if ask:
            args.append("--ask")
        h = Helper(self, "receive", *args, home=home, port=port)
        try:
            ready = h.wait_line("ready")
            check(int(ready[1]) == h.port, f"wrong ready port: {ready}")
            yield h, dest
        finally:
            h.stop()

    def close(self):
        for h in self.processes:
            h.stop()

    @contextlib.contextmanager
    def browser_share(self, *files, alias="ブラウザ試験", port=None, extra=()):
        h = Helper(self, "serve", "--alias", alias, "--pin", PIN, *extra, *files, port=port)
        try:
            ready = h.wait_line("ready")
            check(ready[1:] == [str(h.port), f"http://127.0.0.1:{h.port}"], f"bad HTTP ready: {ready}")
            yield h
        finally:
            h.stop()


def http(port, method, path, body=None, chunked=False):
    conn = http_client.HTTPSConnection("127.0.0.1", port, context=CTX, timeout=8)
    if isinstance(body, dict):
        body = json.dumps(body, ensure_ascii=False).encode("utf-8")
    headers = {"Content-Type": "application/json" if body is not None else "application/octet-stream"}
    if chunked:
        conn.request(method, path, [body], headers, encode_chunked=True)
    else:
        conn.request(method, path, body, headers)
    try:
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def browser_http(port, method, path, headers=None, source=None):
    conn = http_client.HTTPConnection("127.0.0.1", port, timeout=8,
                                      source_address=(source, 0) if source else None)
    try:
        conn.request(method, path, headers=headers or {})
        response = conn.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        conn.close()


class BrowserPage(HTMLParser):
    def __init__(self, body):
        super().__init__(convert_charrefs=True)
        self.tags = []
        self.links = []
        self.feed(body.decode("utf-8"))

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.tags.append((tag, attrs))
        if tag == "a" and "href" in attrs:
            self.links.append(attrs["href"])


def browser_prepare(port, **query):
    return browser_http(port, "POST", "/api/localsend/v2/prepare-download?" + urlencode(query))


def browser_session(port):
    status, headers, body = browser_prepare(port, pin=PIN)
    check(status == 200, f"prepare-download: {status}: {body}")
    check("application/json" in headers.get("Content-Type", ""), f"prepare type: {headers}")
    result = json.loads(body)
    sid = result.get("sessionId")
    check(isinstance(sid, str) and len(sid) >= 32 and sid != "127.0.0.1" and PIN not in sid,
          f"session is not a long opaque token: {sid!r}")
    return result


def download_path(session_id, file_id):
    return "/api/localsend/v2/download?" + urlencode({"sessionId": session_id, "fileId": file_id})


def info(port):
    status, data = http(port, "GET", "/api/localsend/v2/info")
    check(status == 200, f"info: {status} {data}")
    return json.loads(data)


def metadata(files, fingerprint="A" * 64):
    return {"info": {"alias": "Python試験", "version": "2.1", "deviceModel": "Python",
                     "deviceType": "desktop", "fingerprint": fingerprint, "port": 53317,
                     "protocol": "https", "download": False}, "files": files}


def filemeta(name="test.txt", data=b"abc", size=None, digest=None):
    result = {"id": "f", "fileName": name, "size": len(data) if size is None else size,
              "fileType": "application/octet-stream"}
    if digest is not None:
        result["sha256"] = digest
    return result


def prepare(port, files=None, pin=PIN, chunked=False):
    path = "/api/localsend/v2/prepare-upload"
    if pin is not None:
        path += "?" + urlencode({"pin": pin})
    return http(port, "POST", path, metadata(files or {"f": filemeta()}), chunked=chunked)


def session(port, files=None, chunked=False):
    status, body = prepare(port, files, chunked=chunked)
    check(status == 200, f"prepare: {status} {body}")
    return json.loads(body)


def upload_path(s, file_id="f", token=None):
    return "/api/localsend/v2/upload?" + urlencode({"sessionId": s["sessionId"], "fileId": file_id,
                                                   "token": s["files"][file_id] if token is None else token})


def test_send(suite):
    source = suite.directory("source")
    files = {"日本語.txt": "日本語の転送\n".encode(), "empty": b"", "large.bin": bytes(range(256)) * 5000}
    for name, data in files.items():
        (source / name).write_bytes(data)
    folder = source / "資料"
    (folder / "内側").mkdir(parents=True)
    (folder / "内側" / "内容.txt").write_bytes(b"zip content\n")
    with suite.receiver() as (receiver, dest):
        sender = Helper(suite, "send", "--alias", "送信試験", "--to", f"127.0.0.1:{receiver.port}",
                        "--pin", PIN, *(source / n for n in files), folder)
        code, output = sender.finish()
        check(code == 0 and "done" in sender.lines, f"send: {code}: {output}")
        receiver.wait_line("done")
        for name, data in files.items():
            check((dest / name).read_bytes() == data, f"bytes changed: {name}")
        archives = list(dest.glob("*.zip"))
        check(len(archives) == 1, f"directory archive missing: {list(dest.iterdir())}")
        with zipfile.ZipFile(archives[0]) as z:
            matches = [n for n in z.namelist() if n.endswith("内側/内容.txt")]
            check(len(matches) == 1 and z.read(matches[0]) == b"zip content\n", "directory ZIP content changed")


def test_pin(suite):
    source = suite.root / "pin-file"
    source.write_bytes(b"pin")
    with suite.receiver() as (receiver, _):
        check(prepare(receiver.port, pin=None)[0] == 401, "missing PIN must return 401")
        for pin, line in ((None, "pin\trequired"), ("000000", "pin\twrong")):
            args = ["--to", f"127.0.0.1:{receiver.port}"]
            if pin:
                args += ["--pin", pin]
            sender = Helper(suite, "send", *args, source)
            code, output = sender.finish()
            check(code != 0 and line in sender.lines, f"sender PIN event missing: {output}")
    with suite.receiver() as (receiver, _):
        for _ in range(5):
            check(prepare(receiver.port, pin="000000")[0] == 401, "wrong PIN must return 401")
        receiver.finish(seconds=8)
        check(receiver.process.poll() is not None, "five wrong PINs must stop receive")


def test_names(suite):
    with suite.receiver() as (receiver, dest):
        outside = suite.root / "symlink-outside"
        outside.write_bytes(b"outside remains unchanged")
        (dest / "link.txt").symlink_to(outside)
        for name in ("../x", "/abs", "con\x01trol.txt", "c1\u0085clean.txt", "same.txt", "same.txt", "link.txt"):
            data = name.encode()
            ss = session(receiver.port, {"f": filemeta(name, data)})
            status, body = http(receiver.port, "POST", upload_path(ss), data)
            check(status == 200, f"upload {name!r}: {status} {body}")
            receiver.wait_line("done")
        names = {p.name for p in dest.iterdir()}
        check({"x", "abs", "control.txt", "c1clean.txt", "same.txt", "same (2).txt", "link.txt", "link (2).txt"}
              == names, f"sanitized names: {names}")
        check(outside.read_bytes() == b"outside remains unchanged" and (dest / "link.txt").is_symlink(),
              "existing symlink or its outside target changed")
        check((dest / "link (2).txt").read_bytes() == b"link.txt", "symlink collision did not receive a new name")
        long_name = "日本語" * 100 + ".txt"
        ss = session(receiver.port, {"f": filemeta(long_name, b"long UTF-8")})
        check(http(receiver.port, "POST", upload_path(ss), b"long UTF-8")[0] == 200, "long UTF-8 name rejected")
        receiver.wait_line("done")
        added = {p.name for p in dest.iterdir()} - names
        check(len(added) == 1, f"long filename missing: {added}")
        saved_name = added.pop()
        check(saved_name.startswith("日本語") and len(saved_name.encode("utf-8")) <= 255,
              f"long UTF-8 filename damaged: {saved_name!r}")
        check((dest / saved_name).read_bytes() == b"long UTF-8", "long filename bytes changed")
        for p in dest.iterdir():
            if p.name == "link.txt":
                continue  # The preexisting link is deliberately preserved.
            check(p.resolve().parent == dest.resolve() and not p.is_symlink(), f"escaped destination: {p}")
        check(not (dest.parent / "x").exists(), "traversal wrote outside receiver directory")


def raw_upload(port, path, declared, data, truncated=False):
    with socket.create_connection(("127.0.0.1", port), timeout=8) as sock:
        with CTX.wrap_socket(sock, server_hostname="localhost") as tls:
            header = (f"POST {path} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {declared}\r\n"
                      "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n").encode()
            tls.sendall(header + data)
            if truncated:
                return
            response = http_client.HTTPResponse(tls)
            response.begin()
            return response.status, response.read()


def test_failures(suite):
    # Separate sessions avoid assuming that a failed sender is allowed to retry.
    with suite.receiver() as (receiver, dest):
        ss = session(receiver.port, {"f": filemeta(size=3)})
        status, _ = raw_upload(receiver.port, upload_path(ss), 4, b"abcd")
        check(status >= 400, f"oversized upload accepted: {status}")
        until(lambda: not list(dest.iterdir()), "oversized upload left a partial file")
    with suite.receiver() as (receiver, dest):
        ss = session(receiver.port, {"f": filemeta(size=3)})
        status, _ = http(receiver.port, "POST", upload_path(ss), b"abcd", chunked=True)
        check(status == 413, f"oversized chunked upload accepted: {status}")
        until(lambda: not list(dest.iterdir()), "oversized chunked upload left a partial file")
    with suite.receiver() as (receiver, dest):
        ss = session(receiver.port, {"f": filemeta(size=100)})
        raw_upload(receiver.port, upload_path(ss), 100, b"cut", truncated=True)
        # Confirm the worker entered upload before testing for removed partials.
        receiver.wait_line("file")
        info(receiver.port)
        until(lambda: not list(dest.iterdir()), "truncated upload left a partial file")
    with suite.receiver() as (receiver, dest):
        ss = session(receiver.port, {"f": filemeta(digest="0" * 64)})
        status, _ = http(receiver.port, "POST", upload_path(ss), b"abc")
        check(status == 422, f"SHA mismatch must return 422: {status}")
        until(lambda: not list(dest.iterdir()), "SHA mismatch left a partial file")


def test_protocol(suite):
    with suite.receiver() as (receiver, dest):
        identity = info(receiver.port)
        check(identity.get("protocol") == "https" and identity.get("port") == receiver.port,
              f"bad info: {identity}")
        status, body = http(receiver.port, "POST", "/api/localsend/v2/register", metadata({})["info"])
        check(status == 200 and json.loads(body)["fingerprint"] == identity["fingerprint"],
              f"register: {status} {body}")
        ss = session(receiver.port, chunked=True)
        check(prepare(receiver.port)[0] == 409, "concurrent sender was not refused with 409")
        check(http(receiver.port, "POST", upload_path(ss, token="wrong"), b"abc")[0] in (401, 403),
              "wrong upload token accepted")
        status, body = http(receiver.port, "POST", upload_path(ss), b"abc", chunked=True)
        check(status == 200, f"chunked upload failed: {status} {body}")
        receiver.wait_line("done")
        check((dest / "test.txt").read_bytes() == b"abc", "chunked upload bytes changed")


def test_invalid_requests(suite):
    with suite.receiver() as (receiver, dest):
        for size in (-1, 1.5, "3", None):
            item = filemeta()
            item["size"] = size
            status, body = prepare(receiver.port, {"f": item})
            check(status == 400, f"invalid size {size!r}: {status} {body}")
        status, _ = http(receiver.port, "POST", "/api/localsend/v2/prepare-upload?pin=" + PIN, b"{broken")
        check(status == 400, f"malformed JSON was accepted: {status}")
        check(http(receiver.port, "POST", "/api/localsend/v2/upload?sessionId=bad&fileId=f&token=bad", b"abc")[0]
              in (400, 403, 404), "unknown session accepted")
        check(not list(dest.iterdir()), "invalid request created files")


def test_ask(suite):
    for decision in ("accept", "reject"):
        with suite.receiver(ask=True) as (receiver, dest):
            outcome = queue.Queue()
            def request():
                try:
                    outcome.put(prepare(receiver.port))
                except Exception as exc:
                    outcome.put(exc)
            worker = threading.Thread(target=request, daemon=True)
            worker.start()
            fields = receiver.wait_line("request")
            check(fields[1:] == ["Python試験", "1", "3"], f"request event: {fields}")
            check(prepare(receiver.port)[0] == 409, "pending ask must return 409 to another sender")
            receiver.process.stdin.write(decision + "\n")
            receiver.process.stdin.flush()
            worker.join(timeout=10)
            check(not worker.is_alive(), f"{decision} did not answer pending request")
            result = outcome.get_nowait()
            if isinstance(result, Exception):
                raise result
            status, body = result
            if decision == "accept":
                check(status == 200, f"accept: {status} {body}")
                ss = json.loads(body)
                check(http(receiver.port, "POST", upload_path(ss), b"abc")[0] == 200, "accepted upload failed")
                receiver.wait_line("done")
                check((dest / "test.txt").read_bytes() == b"abc", "accepted bytes changed")
            else:
                check(status == 403 and not list(dest.iterdir()), f"reject: {status} {body}")


def test_gone(suite):
    # The sender closes while the receiver asks (an app quit after the PIN):
    # the request is dropped and the next sender is asked, not refused.
    with suite.receiver(ask=True) as (receiver, dest):
        body = json.dumps(metadata({"f": filemeta()}), ensure_ascii=False).encode("utf-8")
        sock = socket.create_connection(("127.0.0.1", receiver.port), timeout=8)
        tls = CTX.wrap_socket(sock, server_hostname="localhost")
        tls.sendall(("POST /api/localsend/v2/prepare-upload?pin=%s HTTP/1.1\r\nHost: x\r\n"
                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n" % (PIN, len(body))).encode() + body)
        receiver.wait_line("request")
        tls.close()
        fields = receiver.wait_line("cancelled")
        check(fields[1:] == ["peer"], f"left while asked: {fields}")
        outcome = queue.Queue()
        worker = threading.Thread(target=lambda: outcome.put(prepare(receiver.port)), daemon=True)
        worker.start()
        receiver.wait_line("request")
        receiver.process.stdin.write("reject\n"); receiver.process.stdin.flush()
        worker.join(timeout=10)
        check(outcome.get_nowait()[0] == 403, "the next sender was asked")
    # Accepted, then nothing comes: given up after FDTP_LOCALSEND_IDLE.
    os.environ["FDTP_LOCALSEND_IDLE"] = "2"
    try:
        with suite.receiver() as (receiver, dest):
            session(receiver.port)
            started = time.monotonic()
            fields = receiver.wait_line("cancelled", seconds=10)
            check(fields[1:] == ["timeout"] and time.monotonic() - started >= 1.5, f"idle sender: {fields}")
            check(prepare(receiver.port)[0] == 200, "a new sender after the idle one")
    finally:
        del os.environ["FDTP_LOCALSEND_IDLE"]


def test_text_pin(suite):
    # The official apps let the receiver choose any PIN (FxDR7m): send must
    # pass it through, URL-encoded; only our own receive PIN is six digits.
    with suite.receiver() as (receiver, dest):
        source = suite.directory("text-pin")
        (source / "p.txt").write_bytes(b"pin")
        h = Helper(suite, "send", "--alias", "送り手", "--to", f"127.0.0.1:{receiver.port}", "--pin", "FxDR7m",
                   str(source / "p.txt"))
        line = h.wait_line("pin")
        check(line[1:] == ["wrong"], f"a text PIN reached the receiver and was judged: {line}")
        h.finish()


def test_identity(suite):
    home = suite.directory("persistent-home")
    fingerprints = []
    for _ in range(2):
        with suite.receiver(home=home) as (receiver, _):
            fp = info(receiver.port)["fingerprint"]
            check(re.fullmatch(r"[0-9A-F]{64}", fp) is not None, f"fingerprint format: {fp}")
            with socket.create_connection(("127.0.0.1", receiver.port), timeout=8) as sock:
                with CTX.wrap_socket(sock, server_hostname="localhost") as tls:
                    actual = hashlib.sha256(tls.getpeercert(binary_form=True)).hexdigest().upper()
            check(fp == actual, f"fingerprint differs from certificate DER: {fp} != {actual}")
            fingerprints.append(fp)
        for name in ("cert.pem", "key.pem"):
            path = home / ".fdtp" / "localsend" / name
            check(path.is_file() and path.stat().st_mode & 0o777 == 0o600, f"certificate permission: {path}")
    check(fingerprints[0] == fingerprints[1], "certificate identity changed after restart")


def test_tls_identity(suite):
    contexts = []
    identities = []
    for _ in range(2):
        with suite.receiver() as (receiver, _):
            identities.append(info(receiver.port))
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(receiver.home / ".fdtp/localsend/cert.pem",
                                    receiver.home / ".fdtp/localsend/key.pem")
            contexts.append(context)
    source = suite.root / "tls-identity.txt"
    source.write_bytes(b"must not reach a changed certificate")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        listener.settimeout(8)
        port = listener.getsockname()[1]
        outcome = queue.Queue()
        def peer():
            try:
                sock, _ = listener.accept()
                with contexts[0].wrap_socket(sock, server_side=True) as tls:
                    tls.settimeout(8)
                    head = bytearray()
                    while not head.endswith(b"\r\n\r\n") and len(head) < 32768:
                        piece = tls.recv(1)
                        check(piece, "TLS mock register closed early")
                        head.extend(piece)
                    check(head.startswith(b"POST /api/localsend/v2/register "), "TLS mock expected register")
                    length = next(int(line.split(b":", 1)[1]) for line in head.split(b"\r\n")
                                  if line.lower().startswith(b"content-length:"))
                    while length:
                        piece = tls.recv(min(length, 65536))
                        check(piece, "TLS mock register body closed early")
                        length -= len(piece)
                    body = json.dumps(identities[0]).encode()
                    tls.sendall((f"HTTP/1.1 200 OK\r\nContent-Length: {len(body)}\r\n"
                                 "Content-Type: application/json\r\nConnection: close\r\n\r\n").encode() + body)
                sock, _ = listener.accept()
                # The next connection presents a different, individually valid
                # certificate. No request metadata may cross this connection.
                try:
                    with contexts[1].wrap_socket(sock, server_side=True) as tls:
                        tls.settimeout(8)
                        application_data = tls.recv(1)
                except ssl.SSLError:
                    application_data = b""
                finally:
                    sock.close()
                outcome.put(application_data)
            except Exception as exc:
                outcome.put(exc)
        worker = threading.Thread(target=peer, daemon=True)
        worker.start()
        sender = Helper(suite, "send", "--to", f"127.0.0.1:{port}", "--pin", PIN, source)
        code, output = sender.finish()
        worker.join(timeout=10)
        check(not worker.is_alive(), "TLS mock did not finish")
        result = outcome.get_nowait()
        if isinstance(result, Exception):
            raise result
        check(code != 0 and "done" not in sender.lines and result == b"",
              f"changed certificate received HTTP data: {result!r}: {output}")


def test_shutdown(suite):
    with suite.receiver() as (receiver, dest):
        ss = session(receiver.port, {"f": filemeta(size=1000000)})
        with socket.create_connection(("127.0.0.1", receiver.port), timeout=8) as sock:
            with CTX.wrap_socket(sock, server_hostname="localhost") as tls:
                head = (f"POST {upload_path(ss)} HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Length: 1000000\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
                tls.sendall(head + b"partial")
                receiver.wait_line("file")
                receiver.process.terminate()
                receiver.finish(seconds=5)
                check(not list(dest.iterdir()), "receiver shutdown left a partial upload")
    with suite.receiver(ask=True) as (receiver, dest):
        outcome = queue.Queue()
        def request():
            try:
                outcome.put(prepare(receiver.port))
            except Exception as exc:
                outcome.put(exc)
        worker = threading.Thread(target=request, daemon=True)
        worker.start()
        receiver.wait_line("request")
        check(outcome.empty(), "ask request answered before a decision")
        receiver.process.terminate()
        receiver.finish(seconds=5)
        worker.join(timeout=5)
        check(not worker.is_alive(), "ask request remained open after receiver shutdown")
        check(not list(dest.iterdir()), "pending ask shutdown wrote a file")


def loopback_multicast_available():
    # Establish whether this host routes multicast on loopback independently
    # of the helper, so an implementation failure cannot silently become SKIP.
    group = "224.0.0.167"
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
            listener.bind(("", 0))
            port = listener.getsockname()[1]
            listener.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                                socket.inet_aton(group) + socket.inet_aton("127.0.0.1"))
            listener.settimeout(2)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
                sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
                sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
                sender.sendto(b"fdtp-loopback-probe", (group, port))
                packet, address = listener.recvfrom(256)
                check(packet == b"fdtp-loopback-probe" and address[0] == "127.0.0.1", "invalid loopback probe")
    except OSError as exc:
        print(f"  SKIP: host loopback multicast unavailable: {exc}")
        return False
    return True


def test_scan(suite):
    if not loopback_multicast_available():
        return
    with suite.receiver() as (receiver, dest):
        fp = info(receiver.port)["fingerprint"]
        scanner = Helper(suite, "scan", "--alias", "探索試験", "--seconds", "3", port=receiver.port)
        code, output = scanner.finish(seconds=12)
        check(code == 0 and "done" in scanner.lines, f"scan failed: {code}: {output}")
        peers = [line.split("\t") for line in scanner.lines if line.startswith("peer\t")]
        check(all(p[3] == "127.0.0.1" for p in peers), f"non-loopback discovery: {output}")
        check(any(len(p) >= 8 and p[1] == fp and int(p[4]) == receiver.port for p in peers),
              f"receiver missing from loopback discovery: {output}")
        source = suite.root / "fingerprint-send.txt"
        source.write_bytes("指紋指定の実転送\n".encode())
        sender = Helper(suite, "send", "--to", fp, "--pin", PIN, source, port=receiver.port)
        code, output = sender.finish(seconds=15)
        check(code == 0 and "done" in sender.lines, f"fingerprint send failed: {code}: {output}")
        receiver.wait_line("done")
        check((dest / source.name).read_bytes() == source.read_bytes(), "fingerprint send bytes changed")


def test_cli(suite):
    for pin in ("", "12345", "1234567", "12a456"):
        h = Helper(suite, "receive", "--dir", suite.directory("invalid-pin"), "--pin", pin)
        code, output = h.finish(seconds=4)
        check(code != 0 and not any(s.startswith("ready\t") for s in h.lines),
              f"invalid PIN accepted: {pin!r}: {output}")


def test_scan_until_signal(suite):
    for sig in (signal.SIGINT, signal.SIGTERM):
        scanner = Helper(suite, "scan", "--seconds", "0")
        time.sleep(4)  # Longer than the default scan, including identity setup.
        check(scanner.process.poll() is None, f"continuous scan exited early: {scanner.lines}")
        scanner.process.send_signal(sig)
        code, output = scanner.finish(seconds=8)
        check(code == 0 and scanner.lines.count("done") == 1,
              f"continuous scan did not stop cleanly on {sig}: {code}: {output}")


def test_serve_browser(suite):
    source = suite.directory("browser-files")
    name = '<tag>&"日本語\'.txt'
    alias = '<b>共有&"\'機器</b>'
    file = source / name
    file.write_bytes("ブラウザから受け取る\n".encode())
    with suite.browser_share(file, alias=alias) as server:
        for language, lang in (("ja-JP,en;q=0.8", "ja"), ("en-US,ja;q=0.8", "en")):
            status, headers, body = browser_http(server.port, "GET", "/", {"Accept-Language": language})
            page = BrowserPage(body)
            check(status == 200 and "text/html" in headers.get("Content-Type", "")
                  and "utf-8" in headers.get("Content-Type", "").lower(), "PIN page must be UTF-8 HTML")
            check(any(tag == "html" and attrs.get("lang") == lang for tag, attrs in page.tags),
                  f"Accept-Language not reflected: {language}: {body}")
            check(any(tag == "meta" and attrs.get("name", "").lower() == "viewport"
                      and "width=device-width" in attrs.get("content", "") for tag, attrs in page.tags),
                  "mobile viewport missing")
            check(any(tag == "form" and attrs.get("method", "get").lower() == "get" for tag, attrs in page.tags)
                  and any(tag == "input" and attrs.get("name") == "pin" for tag, attrs in page.tags),
                  "PIN must work as a GET form without JavaScript")
            check(not any(tag == "script" or any(key.startswith("on") for key in attrs)
                          for tag, attrs in page.tags), "browser page requires JavaScript")
            check(not page.links and name.encode() not in body and b"sessionId=" not in body,
                  "unauthenticated HTML exposed downloads")
        status, _, body = browser_http(server.port, "GET", "/?pin=000000")
        check(status == 401 and not BrowserPage(body).links, "wrong browser PIN accepted")
        check(server.wait_line("pin")[1:] == ["wrong", "127.0.0.1"], "wrong PIN event lacks peer IP")
        # A Japanese keyboard types full-width digits, maybe with spaces.
        wide = "".join(chr(0xFF10 + int(d)) for d in PIN)
        status, _, body = browser_http(server.port, "GET", "/?pin=" + quote(wide[:3] + "\u3000" + wide[3:]))
        check(status == 200 and BrowserPage(body).links, f"full-width PIN with a space refused: {status}")
        status, _, body = browser_http(server.port, "GET", "/?pin=" + PIN)
        page = BrowserPage(body)
        check(status == 200 and len(page.links) == 1, f"authenticated file list: {status}: {body}")
        text = body.decode()
        check(name in html.unescape(text) and "<tag>" not in text and "<b>共有" not in text,
              "file name or alias is not HTML escaped")
        for link in page.links:
            parsed = urlsplit(link)
            query = parse_qs(parsed.query)
            check(parsed.path == "/api/localsend/v2/download" and not parsed.netloc
                  and "pin" not in query and PIN not in link
                  and set(query) == {"sessionId", "fileId"}, f"unsafe download link: {link}")
            check(len(query["sessionId"][0]) >= 32, "HTML link does not contain opaque session")
            status, headers, data = browser_http(server.port, "GET", link)
            check(status == 200 and data == file.read_bytes(), f"HTML link download: {status}: {data}")
            disposition = headers.get("Content-Disposition", "")
            extended = re.search(r"filename\*=UTF-8''([^;]+)", disposition, re.I)
            fallback = re.search(r'(?:^|;)\s*filename="([^"\r\n]*)"', disposition)
            check(disposition.startswith("attachment;") and extended and unquote(extended[1]) == name,
                  f"UTF-8 disposition: {disposition}")
            check(extended[1].isascii() and "%" in extended[1] and fallback and fallback[1].isascii(),
                  f"missing ASCII fallback / percent encoding: {disposition}")
            check(headers.get("Content-Length") == str(file.stat().st_size), "download length missing")
        check(server.wait_line("download")[1:] == ["127.0.0.1", name], "download event wrong")
        check(server.wait_line("downloaded")[1:] == ["127.0.0.1", name], "downloaded event wrong")


def test_serve_files(suite):
    source = suite.directory("browser-data")
    expected = {"日本語.txt": "日本語の転送\n".encode(), "empty": b"",
                "large.bin": bytes(range(256)) * 32768}
    for name, content in expected.items():
        (source / name).write_bytes(content)
    folder = source / "資料"
    (folder / "内側").mkdir(parents=True)
    (folder / "内側" / "内容.txt").write_bytes(b"browser zip content\n")
    with suite.browser_share(*(source / n for n in expected), folder) as server:
        status, headers, body = browser_http(server.port, "GET", "/api/localsend/v2/info")
        identity = json.loads(body)
        check(status == 200 and identity.get("download") is True and identity.get("protocol") == "http"
              and identity.get("port") == server.port, f"serve info: {identity}")
        ss = browser_session(server.port)
        check(ss["info"].get("download") is True and ss["info"].get("alias") == "ブラウザ試験"
              and ss["info"].get("fingerprint") == identity["fingerprint"]
              and isinstance(ss["info"].get("version"), str), f"prepare info: {ss}")
        files = ss.get("files")
        check(isinstance(files, dict) and len(files) == len(expected) + 1, f"prepare files: {files}")
        names = set()
        for file_id, item in files.items():
            check(item.get("id") == file_id and isinstance(item.get("size"), int)
                  and item["size"] >= 0 and isinstance(item.get("fileType"), str), f"file metadata: {item}")
            name = item["fileName"]
            names.add(name)
            conn = http_client.HTTPConnection("127.0.0.1", server.port, timeout=8)
            try:
                conn.request("GET", download_path(ss["sessionId"], file_id))
                response = conn.getresponse()
                check(response.status == 200 and response.getheader("Content-Length") == str(item["size"]),
                      f"file response: {name}: {response.status}")
                digest, total, archive = hashlib.sha256(), 0, bytearray()
                while True:
                    piece = response.read(32768)
                    if not piece:
                        break
                    total += len(piece)
                    digest.update(piece)
                    if name.endswith(".zip"):
                        archive.extend(piece)
                check(total == item["size"], f"stream length: {name}: {total}")
                if name in expected:
                    check(digest.digest() == hashlib.sha256(expected[name]).digest(), f"stream bytes: {name}")
                else:
                    path = source / "downloaded.zip"
                    path.write_bytes(archive)
                    with zipfile.ZipFile(path) as z:
                        inner = [n for n in z.namelist() if n.endswith("内側/内容.txt")]
                        check(len(inner) == 1 and z.read(inner[0]) == b"browser zip content\n", "browser zip bytes")
            finally:
                conn.close()
            check(server.wait_line("download")[1:] == ["127.0.0.1", name], "stream download event")
            check(server.wait_line("downloaded")[1:] == ["127.0.0.1", name], "stream downloaded event")
        check(set(expected) <= names and sum(n.endswith(".zip") for n in names) == 1, f"file names: {names}")


def test_serve_sessions(suite):
    file = suite.root / "browser-session.txt"
    file.write_bytes(b"session protected")
    with suite.browser_share(file) as server:
        check(browser_prepare(server.port)[0] == 401, "prepare-download without PIN accepted")
        check(browser_prepare(server.port, pin="000000")[0] == 401, "prepare-download wrong PIN accepted")
        first = browser_session(server.port)
        sid, file_id = first["sessionId"], next(iter(first["files"]))
        status, _, body = browser_prepare(server.port, sessionId=sid)
        check(status == 200 and json.loads(body) == first, "same-IP session cannot reload file list")
        check(browser_http(server.port, "GET", download_path(sid, file_id))[2] == file.read_bytes(),
              "session download failed")
        for path, expected_status in (("/missing", 404), ("/api/localsend/v2/download/extra", 404),
                                      ("/api/localsend/v2/download", 400),
                                      ("/api/localsend/v2/download?sessionId=" + sid, 400),
                                      (download_path("not-a-session", file_id), 403),
                                      (download_path(sid, "not-a-file"), 403),
                                      (download_path(sid, str(file)), 403),
                                      (download_path(sid, "../../etc/passwd"), 403),
                                      ("/../../etc/passwd", 404), ("/%2e%2e/%2e%2e/etc/passwd", 404)):
            check(browser_http(server.port, "GET", path)[0] == expected_status,
                  f"download boundary {expected_status}: {path}")
        try:
            with socket.socket() as probe:
                probe.bind(("127.0.0.2", 0))
        except OSError as exc:
            print(f"  SKIP: second loopback source unavailable for session IP check: {exc}")
        else:
            check(browser_http(server.port, "GET", download_path(sid, file_id), source="127.0.0.2")[0] == 403,
                  "session accepted from a different IP")
            path = "/api/localsend/v2/prepare-download?" + urlencode({"sessionId": sid})
            check(browser_http(server.port, "POST", path, source="127.0.0.2")[0] == 401,
                  "prepare-download reused another IP's session")
        second = browser_session(server.port)
        check(second["sessionId"] != sid, "same-IP reauthentication did not rotate random session")
        check(browser_http(server.port, "GET", download_path(sid, file_id))[0] == 403,
              "old same-IP session survived replacement")
        check(browser_prepare(server.port, sessionId=second["sessionId"])[0] == 200,
              "replacement session cannot reload")


def test_serve_pin_limit(suite):
    file = suite.root / "browser-pin.txt"
    file.write_bytes(b"pin limit")
    for via_page in (True, False):
        with suite.browser_share(file) as server:
            for _ in range(6):
                check(browser_prepare(server.port)[0] == 401, "missing PIN accepted or exhausted retries")
            for attempt in range(5):
                result = (browser_http(server.port, "GET", "/?pin=000000") if via_page
                          else browser_prepare(server.port, pin="000000"))
                check(result[0] == 401, f"wrong PIN attempt {attempt + 1}: {result}")
                check(server.wait_line("pin")[1:] == ["wrong", "127.0.0.1"], "PIN failure peer missing")
                if attempt < 4:
                    check(server.process.poll() is None, "serve stopped before five wrong PINs")
            server.finish(seconds=8)
            check(server.process.poll() is not None, "five wrong PINs did not close serve")


def test_serve_parallel_shutdown(suite):
    source = suite.directory("browser-stream")
    large = source / "cancelled-large.bin"
    with large.open("wb") as stream:
        stream.truncate(128 * 1024 * 1024)
    small = source / "parallel.txt"
    small.write_bytes(b"another download while large is blocked\n")
    with suite.browser_share(large, small) as server:
        ss = browser_session(server.port)
        ids = {item["fileName"]: key for key, item in ss["files"].items()}
        def blocked_download():
            conn = http_client.HTTPConnection("127.0.0.1", server.port, timeout=8)
            conn.request("GET", download_path(ss["sessionId"], ids[large.name]))
            response = conn.getresponse()
            check(response.status == 200 and response.read(1) == b"\0", "large stream did not start")
            return conn, response
        conn, response = blocked_download()
        try:
            server.wait_line("download")
            status, _, data = browser_http(server.port, "GET", download_path(ss["sessionId"], ids[small.name]))
            check(status == 200 and data == small.read_bytes(), "parallel download blocked by a slow client")
        finally:
            response.close()
            conn.close()
        # A cancelled HTTP stream must not prevent the next request or create
        # a false completion event for the 128 MiB file.
        check(browser_http(server.port, "GET", "/api/localsend/v2/info")[0] == 200,
              "server stopped accepting after cancellation")
        check(not any(line == f"downloaded\t127.0.0.1\t{large.name}" for line in server.lines),
              "cancelled stream reported as downloaded")
        conn, response = blocked_download()
        try:
            server.process.terminate()
            code, output = server.finish(seconds=5)
            check(code == 0, f"serve shutdown while streaming: {code}: {output}")
        finally:
            response.close()
            conn.close()


def test_serve_port_busy_cli(suite):
    file = suite.root / "browser-cli.txt"
    file.write_bytes(b"port busy")
    for args in ((file,), ("--pin", PIN), ("--pin", PIN, "--no-pin", file),
                 *(("--pin", pin, file) for pin in ("", "12345", "1234567", "12a456"))):
        h = Helper(suite, "serve", *args)
        code, output = h.finish(seconds=4)
        check(code != 0 and not any(line.startswith("ready\t") for line in h.lines),
              f"invalid serve arguments accepted: {args}: {output}")
    with suite.receiver() as (receiver, _):
        h = Helper(suite, "serve", "--pin", PIN, file, port=receiver.port)
        code, output = h.finish(seconds=5)
        check(code != 0 and any(line.startswith("error\tport_busy\t") for line in h.lines),
              f"serve did not report occupied receive port: {output}")
    with suite.browser_share(file) as server:
        h = Helper(suite, "receive", "--pin", PIN, "--dir", suite.directory("busy-receive"), port=server.port)
        code, output = h.finish(seconds=5)
        check(code != 0 and any(line.startswith("error\tport_busy\t") for line in h.lines),
              f"receive did not report occupied serve port: {output}")


def test_serve_announce(suite):
    if not loopback_multicast_available():
        return
    file = suite.root / "browser-announce.txt"
    file.write_bytes(b"multicast")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        listener.bind(("", 0))
        port = listener.getsockname()[1]
        listener.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                            socket.inet_aton("224.0.0.167") + socket.inet_aton("127.0.0.1"))
        listener.settimeout(5)
        with suite.browser_share(file, extra=("--multicast-port", str(port))) as server:
            packet, address = listener.recvfrom(65536)
            data = json.loads(packet)
            check(address[0] == "127.0.0.1" and data.get("download") is True
                  and data.get("announce") is True and data.get("protocol") == "http"
                  and data.get("port") == server.port and data.get("alias") == "ブラウザ試験",
                  f"serve loopback announcement: {address}: {data}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=APP / "build/fdtp-localsend")
    parser.add_argument("--build", type=Path, default=APP / "build")
    args = parser.parse_args()
    check(args.executable.is_file(), f"helper missing: {args.executable}; run make localsend")
    suite = Suite(args.executable, args.build.resolve())
    failures = []
    try:
        for test in (test_cli, test_send, test_pin, test_names, test_failures, test_protocol, test_invalid_requests,
                     test_ask, test_gone, test_text_pin, test_identity, test_tls_identity, test_shutdown, test_scan, test_scan_until_signal,
                     test_serve_browser, test_serve_files, test_serve_sessions, test_serve_pin_limit,
                     test_serve_parallel_shutdown, test_serve_port_busy_cli, test_serve_announce):
            try:
                test(suite)
                print(f"PASS {test.__name__}", flush=True)
            except Exception as exc:
                failures.append((test.__name__, str(exc)))
                print(f"FAIL {test.__name__}: {exc}", flush=True)
                if os.environ.get("LOCALSEND_TRACE"):
                    traceback.print_exc()
    finally:
        suite.close()
    print(f"fixtures: {suite.root}")
    if failures:
        raise SystemExit(f"LocalSend: {len(failures)} failed")
    print("LocalSend: all checks passed")


if __name__ == "__main__":
    main()
