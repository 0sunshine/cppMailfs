#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import math
import os
import random
import sqlite3
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


def query_scalar(db_path: Path, sql: str, params=()):
    conn = sqlite3.connect(db_path)
    try:
        row = conn.execute(sql, params).fetchone()
        return row[0] if row else None
    finally:
        conn.close()


def query_row(db_path: Path, sql: str, params=()):
    conn = sqlite3.connect(db_path)
    try:
        return conn.execute(sql, params).fetchone()
    finally:
        conn.close()


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
    args = parser.parse_args()

    repo_root = Path(args.repo_root)
    work_dir = repo_root / "test-output"
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    store_path = work_dir / "imap_store.json"
    passwd_path = work_dir / "passwd.txt"
    config_path = work_dir / "mailfs.json"
    db_path = work_dir / "cache.db"
    upload_path = work_dir / "upload.bin"
    download_path = work_dir / "download.bin"

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

    try:
        wait_for_port(args.port)

        run_client(args.client_exe, config_path, repo_root, ["list-mailboxes"])
        run_client(args.client_exe, config_path, repo_root, ["upload", "Archive", upload_path, "/demo/upload.bin"])
        run_client(args.client_exe, config_path, repo_root, ["cache-mailbox", "Archive"])
        cache_list = run_client(args.client_exe, config_path, repo_root, ["list-cache", "Archive"])
        expected_blocks = math.ceil(len(payload) / config["default_block_size"])
        server_data = json.loads(store_path.read_text(encoding="utf-8"))
        mailbox_messages = server_data["mailboxes"].get("Archive", [])

        seeded_block_total = sum(math.ceil(item["size"] / item["block_size"]) for item in large_file_scenarios)
        if len(mailbox_messages) != expected_blocks + seeded_block_total:
            raise RuntimeError(
                f"unexpected message count: {len(mailbox_messages)} != {expected_blocks + seeded_block_total}"
            )
        if "/demo/upload.bin" not in cache_list.stdout:
            raise RuntimeError("cached file listing missing /demo/upload.bin")
        for scenario in large_file_scenarios:
            if scenario["path"] not in cache_list.stdout:
                raise RuntimeError(f"large synthetic record missing from cache list: {scenario['path']}")

        cached_rows = query_scalar(db_path, "SELECT COUNT(*) FROM cache_files")
        if cached_rows != len(large_file_scenarios) + 1:
            raise RuntimeError(f"unexpected cached file count: {cached_rows}")
        for scenario in large_file_scenarios:
            row = query_row(
                db_path,
                "SELECT filesize, blockcount FROM cache_files WHERE localpath = ?",
                (scenario["path"],),
            )
            if row is None:
                raise RuntimeError(f"missing cache row for {scenario['path']}")
            if row[0] != scenario["size"]:
                raise RuntimeError(f"unexpected cached size for {scenario['path']}: {row[0]}")
            if row[1] != math.ceil(scenario["size"] / scenario["block_size"]):
                raise RuntimeError(f"unexpected block count for {scenario['path']}: {row[1]}")

        first_uid = max(item["uid"] for item in mailbox_messages)
        run_client(args.client_exe, config_path, repo_root, ["download", "Archive", "/demo/upload.bin", download_path])
        if md5_file(upload_path) != md5_file(download_path):
            raise RuntimeError("downloaded file content does not match uploaded file")

        run_client(args.client_exe, config_path, repo_root, ["delete-uid", "Archive", str(first_uid)])
        server_data = json.loads(store_path.read_text(encoding="utf-8"))
        mailbox_messages = server_data["mailboxes"].get("Archive", [])
        if any(item["uid"] == first_uid for item in mailbox_messages):
            raise RuntimeError(f"deleted uid {first_uid} still exists on server")
        if len(mailbox_messages) != seeded_block_total + expected_blocks - 1:
            raise RuntimeError(f"unexpected message count after delete: {len(mailbox_messages)}")

        remaining_cache_files = query_scalar(db_path, "SELECT COUNT(*) FROM cache_files")
        remaining_cache_blocks = query_scalar(db_path, "SELECT COUNT(*) FROM cache_blocks")
        if remaining_cache_files != len(large_file_scenarios) or remaining_cache_blocks != seeded_block_total:
            raise RuntimeError(
                f"cache rows not cleared after delete: files={remaining_cache_files}, blocks={remaining_cache_blocks}"
            )

        delete_cache_list = run_client(args.client_exe, config_path, repo_root, ["list-cache", "Archive"])
        if "/demo/upload.bin" in delete_cache_list.stdout:
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
                ["download", "Archive", "/demo/upload.bin", download_path],
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
        print(f"downloaded_md5={md5_file(download_path)}")
        print(f"message_count={len(mailbox_messages)}")
        print(f"expected_blocks={expected_blocks}")
        print(f"deleted_uid={first_uid}")
        print(f"large_records={len(large_file_scenarios)}")
        return 0
    finally:
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
        stderr_text = ""
        if server.stderr:
            stderr_text = server.stderr.read()
        if server.returncode not in (0, -15, 143, None) and stderr_text:
            print(stderr_text, file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
