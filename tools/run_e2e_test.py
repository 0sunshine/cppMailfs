#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import math
import os
import random
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path


def md5_file(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def build_seed_message(subject: str, file_md5: str, block_md5: str, file_size: int, block_size: int,
                       owner: str, local_path: str, mailbox: str, block_seq: int, block_count: int) -> str:
    metadata = {
        "subject": subject,
        "file_md5": file_md5,
        "block_md5": block_md5,
        "file_size": file_size,
        "block_size": block_size,
        "create_time": "2026-04-01T00:00:00Z",
        "owner": owner,
        "local_path": local_path,
        "mail_folder": mailbox,
        "block_seq": block_seq,
        "block_count": block_count,
        "encrypted": False,
    }
    metadata_b64 = base64.b64encode(json.dumps(metadata, separators=(",", ":")).encode("utf-8")).decode("ascii")
    attachment_b64 = base64.b64encode(f"seed-block-{block_seq}".encode("utf-8")).decode("ascii")
    boundary = f"seed-boundary-{abs(hash((subject, block_seq, block_count))) & 0xffffffff:x}"
    return (
        f"Date: Tue, 01 Apr 2026 00:00:00 +0000\r\n"
        f'From: "seed" <seed@example.com>\r\n'
        f'Subject: {subject}\r\n'
        f'To: "seed" <seed@example.com>\r\n'
        f"MIME-Version: 1.0\r\n"
        f'Content-Type: multipart/mixed; boundary="{boundary}"\r\n'
        f"\r\n"
        f"--{boundary}\r\n"
        f"Content-Transfer-Encoding: base64\r\n"
        f"Content-Type: application/json; charset=utf-8\r\n"
        f"\r\n"
        f"{metadata_b64}\r\n"
        f"--{boundary}\r\n"
        f'Content-Disposition: attachment; filename="block-{block_seq}.bin"\r\n'
        f"Content-Transfer-Encoding: base64\r\n"
        f"Content-Type: application/octet-stream\r\n"
        f"\r\n"
        f"{attachment_b64}\r\n"
        f"--{boundary}--\r\n"
    )


def seed_large_file_records(store_path: Path):
    scenarios = [
        {"path": "/vault/archive-1g.iso", "size": 1073741824, "block_size": 134217728},
        {"path": "/vault/archive-1p1g.iso", "size": 1181116006, "block_size": 134217728},
        {"path": "/vault/archive-1p3g.iso", "size": 1395864371, "block_size": 268435456},
    ]

    store_data = {"users": {}, "mailboxes": {"Archive": []}, "next_uid": 1}
    uid = 1
    for scenario in scenarios:
        block_count = math.ceil(scenario["size"] / scenario["block_size"])
        for block_seq in range(1, block_count + 1):
            subject = f'{Path(scenario["path"]).name}/plain/{block_seq}-{block_count}'
            store_data["mailboxes"]["Archive"].append(
                {
                    "uid": uid,
                    "raw_message": build_seed_message(
                        subject=subject,
                        file_md5=f"large-file-{uid}",
                        block_md5=f"large-block-{uid}",
                        file_size=scenario["size"],
                        block_size=scenario["block_size"] if block_seq < block_count else (
                            scenario["size"] - scenario["block_size"] * (block_count - 1)
                        ),
                        owner="seed@example.com",
                        local_path=scenario["path"],
                        mailbox="Archive",
                        block_seq=block_seq,
                        block_count=block_count,
                    ),
                    "deleted": False,
                }
            )
            uid += 1
    store_data["next_uid"] = uid
    store_path.write_text(json.dumps(store_data, indent=2), encoding="utf-8")
    return scenarios


def run(args, cwd=None):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed: {' '.join(args)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def to_windows_path(path: Path) -> str:
    text = str(path)
    if os.name == "nt":
        return text
    result = subprocess.run(["wslpath", "-w", text], text=True, capture_output=True, check=True)
    return result.stdout.strip()


def run_client(client_exe: str, config_path: Path, repo_root: Path, command_args):
    windows_client = to_windows_path(Path(client_exe))
    windows_config = to_windows_path(config_path)
    converted = []
    for arg in command_args:
        if isinstance(arg, Path):
            converted.append(to_windows_path(arg))
        else:
            converted.append(str(arg))
    return run(
        ["cmd.exe", "/c", windows_client, "--config", windows_config, *converted],
        cwd=repo_root,
    )


def start_http_server(client_exe: str, config_path: Path, repo_root: Path, listen_addr: str):
    windows_client = to_windows_path(Path(client_exe))
    windows_config = to_windows_path(config_path)
    return subprocess.Popen(
        [
            "cmd.exe",
            "/c",
            windows_client,
            "--config",
            windows_config,
            "serve-http",
            "--listen",
            listen_addr,
        ],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def wait_for_port(port: int, timeout: float = 10.0):
    import socket

    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket() as sock:
            sock.settimeout(0.5)
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.2)
    raise RuntimeError(f"server port {port} did not open in time")


def wait_for_windows_http_server(port: int, timeout: float = 15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = subprocess.run(
            [
                "curl.exe",
                "--silent",
                "--show-error",
                "--output",
                "NUL",
                "--write-out",
                "%{http_code}",
                f"http://127.0.0.1:{port}/",
            ],
            text=True,
            capture_output=True,
        )
        if result.returncode == 0 and result.stdout.strip() in {"200", "404", "405"}:
            return
        time.sleep(0.3)
    raise RuntimeError(f"HTTP server port {port} did not open in time")


def remove_tree_with_retries(path: Path, attempts: int = 10, delay: float = 0.5):
    if not path.exists():
        return
    last_error = None
    for _ in range(attempts):
        try:
            shutil.rmtree(path)
            return
        except OSError as exc:
            last_error = exc
            time.sleep(delay)
    raise last_error


def resolve_download_path(download_root: Path, local_path: str) -> Path:
    normalized = local_path.replace("\\", "/")
    if len(normalized) >= 2 and normalized[1] == ":":
        normalized = normalized[0] + normalized[2:]
    normalized = normalized.lstrip("/")
    parts = [part for part in normalized.split("/") if part]
    return download_root.joinpath(*parts)


def build_http_stream_url(http_port: int, mailbox: str, local_path: str) -> str:
    mailbox_b64 = base64.urlsafe_b64encode(mailbox.encode("utf-8")).decode("ascii").rstrip("=")
    path_b64 = base64.urlsafe_b64encode(local_path.encode("utf-8")).decode("ascii").rstrip("=")
    return f"http://127.0.0.1:{http_port}/httptoimap?imapdir={mailbox_b64}&localpath={path_b64}"


def http_get_bytes_via_windows(url: str, output_path: Path, range_header: str | None = None):
    windows_output = to_windows_path(output_path)
    header_path = output_path.with_suffix(output_path.suffix + ".headers.txt")
    windows_header = to_windows_path(header_path)
    cmd = [
        "curl.exe",
        "--silent",
        "--show-error",
        "--location",
        "--dump-header",
        windows_header,
        "--output",
        windows_output,
        "--write-out",
        "%{http_code}",
    ]
    if range_header:
        cmd.extend(["-H", f"Range: {range_header}"])
    cmd.append(url)
    result = subprocess.run(cmd, text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(f"curl failed\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")

    raw_headers = header_path.read_text(encoding="utf-8", errors="replace")
    header_blocks = [block for block in raw_headers.split("\r\n\r\n") if block.strip()]
    last_block = header_blocks[-1] if header_blocks else raw_headers
    headers = {}
    for line in last_block.splitlines()[1:]:
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        headers[key.strip()] = value.strip()

    return {
        "status": int(result.stdout.strip()),
        "headers": headers,
        "body": output_path.read_bytes(),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server-script", required=True)
    parser.add_argument("--client-exe", required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--port", type=int, default=1993)
    parser.add_argument("--username", default="tester@example.com")
    parser.add_argument("--password", default="secret-pass")
    parser.add_argument("--http-port", type=int, default=29888)
    args = parser.parse_args()

    repo_root = Path(args.repo_root)
    work_dir = repo_root / "test-output"
    if work_dir.exists():
        remove_tree_with_retries(work_dir)
    work_dir.mkdir(parents=True)

    store_path = work_dir / "imap_store.json"
    passwd_path = work_dir / "passwd.txt"
    config_path = work_dir / "mailfs.json"
    db_path = work_dir / "cache.db"
    upload_path = work_dir / "upload.bin"
    download_root = work_dir / "downloads"

    large_file_scenarios = seed_large_file_records(store_path)
    store_data = json.loads(store_path.read_text(encoding="utf-8"))
    store_data["users"][args.username] = args.password
    store_path.write_text(json.dumps(store_data, indent=2), encoding="utf-8")

    passwd_path.write_text(f"{args.username}\n{args.password}\n", encoding="utf-8")

    payload = bytes(random.Random(20260401).randrange(0, 256) for _ in range(20000))
    upload_path.write_bytes(payload)

    config = {
        "imap_server": f"localhost:{args.port}",
        "credential_file": to_windows_path(passwd_path),
        "ca_cert_file": to_windows_path(Path(args.cert)),
        "email_name": "mailfs-test",
        "mailbox_prefix": "*",
        "download_dir": to_windows_path(download_root),
        "http_listen_addr": f"127.0.0.1:{args.http_port}",
        "http_copy_addr": f"http://127.0.0.1:{args.http_port}",
        "database_path": to_windows_path(db_path),
        "default_block_size": 4096,
        "allowed_folders": [],
        "ignore_extensions": [],
        "allow_insecure_tls": False,
    }
    config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")

    server = subprocess.Popen(
        [
            "python3",
            args.server_script,
            "--host",
            "0.0.0.0",
            "--port",
            str(args.port),
            "--cert",
            args.cert,
            "--key",
            args.key,
            "--store",
            str(store_path),
            "--username",
            args.username,
            "--password",
            args.password,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    http_server = None

    try:
        wait_for_port(args.port)

        run_client(args.client_exe, config_path, repo_root, ["list-mailboxes"])
        run_client(args.client_exe, config_path, repo_root, ["upload", "Archive", upload_path])
        run_client(args.client_exe, config_path, repo_root, ["cache-mailbox", "Archive"])
        cache_list = run_client(args.client_exe, config_path, repo_root, ["list-cache", "Archive"])
        expected_blocks = math.ceil(len(payload) / config["default_block_size"])
        server_data = json.loads(store_path.read_text(encoding="utf-8"))
        mailbox_messages = server_data["mailboxes"].get("Archive", [])
        uploaded_local_path = to_windows_path(upload_path.resolve())
        expected_download_path = resolve_download_path(download_root, uploaded_local_path)

        seeded_block_total = sum(math.ceil(item["size"] / item["block_size"]) for item in large_file_scenarios)
        if len(mailbox_messages) != expected_blocks + seeded_block_total:
            raise RuntimeError(
                f"unexpected message count: {len(mailbox_messages)} != {expected_blocks + seeded_block_total}"
            )
        if uploaded_local_path not in cache_list.stdout:
            raise RuntimeError(f"cached file listing missing {uploaded_local_path}")
        for scenario in large_file_scenarios:
            if scenario["path"] not in cache_list.stdout:
                raise RuntimeError(f"large synthetic record missing from cache list: {scenario['path']}")

        first_uid = max(item["uid"] for item in mailbox_messages)
        run_client(args.client_exe, config_path, repo_root, ["download", "Archive", uploaded_local_path])
        if not expected_download_path.exists():
            raise RuntimeError(f"downloaded file missing at {expected_download_path}")
        if md5_file(upload_path) != md5_file(expected_download_path):
            raise RuntimeError("downloaded file content does not match uploaded file")

        http_server = start_http_server(args.client_exe, config_path, repo_root, f"127.0.0.1:{args.http_port}")
        wait_for_windows_http_server(args.http_port)

        stream_url = build_http_stream_url(args.http_port, "Archive", uploaded_local_path)
        full_http_path = work_dir / "http-full.bin"
        range_http_path = work_dir / "http-range.bin"
        full_response = http_get_bytes_via_windows(stream_url, full_http_path)
        if full_response["status"] != 200:
            raise RuntimeError(f"unexpected HTTP status for full download: {full_response['status']}")
        if hashlib.md5(full_response["body"]).hexdigest() != md5_file(upload_path):
            raise RuntimeError("HTTP streamed body content does not match uploaded file")
        if full_response["headers"].get("Accept-Ranges") != "bytes":
            raise RuntimeError("HTTP server did not advertise byte range support")

        range_start = 123
        range_end = 4097
        range_response = http_get_bytes_via_windows(stream_url, range_http_path, f"bytes={range_start}-{range_end}")
        if range_response["status"] != 206:
            raise RuntimeError(f"unexpected HTTP status for range download: {range_response['status']}")
        expected_slice = payload[range_start:range_end + 1]
        if range_response["body"] != expected_slice:
            raise RuntimeError("HTTP range body does not match expected slice")
        expected_content_range = f"bytes {range_start}-{range_end}/{len(payload)}"
        if range_response["headers"].get("Content-Range") != expected_content_range:
            raise RuntimeError(
                f"unexpected Content-Range: {range_response['headers'].get('Content-Range')} != {expected_content_range}"
            )

        run_client(args.client_exe, config_path, repo_root, ["delete-uid", "Archive", str(first_uid)])
        server_data = json.loads(store_path.read_text(encoding="utf-8"))
        mailbox_messages = server_data["mailboxes"].get("Archive", [])
        if any(item["uid"] == first_uid for item in mailbox_messages):
            raise RuntimeError(f"deleted uid {first_uid} still exists on server")
        if len(mailbox_messages) != seeded_block_total + expected_blocks - 1:
            raise RuntimeError(f"unexpected message count after delete: {len(mailbox_messages)}")

        delete_cache_list = run_client(args.client_exe, config_path, repo_root, ["list-cache", "Archive"])
        if uploaded_local_path in delete_cache_list.stdout:
            raise RuntimeError("deleted file still appears in local cache list")
        for scenario in large_file_scenarios:
            if scenario["path"] not in delete_cache_list.stdout:
                raise RuntimeError(f"large synthetic record missing after delete: {scenario['path']}")

        download_failed = False
        try:
            run_client(
                args.client_exe,
                config_path,
                repo_root,
                ["download", "Archive", uploaded_local_path],
            )
        except RuntimeError as exc:
            download_failed = True
            error_text = str(exc)
            if "cached file is incomplete" not in error_text and "cached file index not found" not in error_text:
                raise
        if not download_failed:
            raise RuntimeError("download unexpectedly succeeded after deleting one UID")

        print("E2E OK")
        print(f"uploaded_md5={md5_file(upload_path)}")
        print(f"downloaded_md5={md5_file(expected_download_path)}")
        print(f"http_stream_md5={hashlib.md5(full_response['body']).hexdigest()}")
        print(f"http_range={range_start}-{range_end}")
        print(f"message_count={len(mailbox_messages)}")
        print(f"expected_blocks={expected_blocks}")
        print(f"deleted_uid={first_uid}")
        print(f"large_records={len(large_file_scenarios)}")
        return 0
    finally:
        if http_server is not None and http_server.poll() is None:
            http_server.terminate()
            try:
                http_server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                http_server.kill()
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
        if http_server is not None:
            http_stderr_text = ""
            if http_server.stderr:
                http_stderr_text = http_server.stderr.read()
            if http_server.returncode not in (0, -15, 143, None) and http_stderr_text:
                print(http_stderr_text, file=sys.stderr)
        stderr_text = ""
        if server.stderr:
            stderr_text = server.stderr.read()
        if server.returncode not in (0, -15, 143, None) and stderr_text:
            print(stderr_text, file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
