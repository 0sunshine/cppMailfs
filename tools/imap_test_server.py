#!/usr/bin/env python3
import argparse
import fnmatch
import json
import re
import socketserver
import ssl
import threading
from pathlib import Path


class MailStore:
    def __init__(self, store_path: Path):
        self.store_path = store_path
        self.lock = threading.Lock()
        self.data = {"users": {}, "mailboxes": {}, "next_uid": 1}
        if self.store_path.exists():
            self.data = json.loads(self.store_path.read_text(encoding="utf-8"))

    def save(self) -> None:
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.store_path.write_text(json.dumps(self.data, indent=2), encoding="utf-8")

    def set_user(self, username: str, password: str) -> None:
        with self.lock:
            self.data["users"][username] = password
            self.save()

    def login(self, username: str, password: str) -> bool:
        with self.lock:
            return self.data["users"].get(username) == password

    def ensure_mailbox(self, mailbox: str) -> None:
        if mailbox not in self.data["mailboxes"]:
            self.data["mailboxes"][mailbox] = []

    def list_mailboxes(self, pattern: str):
        with self.lock:
          names = sorted(self.data["mailboxes"].keys())
          return [name for name in names if fnmatch.fnmatch(name, pattern.replace("%", "*"))]

    def append_message(self, mailbox: str, raw_message: str) -> int:
        with self.lock:
            self.ensure_mailbox(mailbox)
            uid = self.data["next_uid"]
            self.data["next_uid"] += 1
            self.data["mailboxes"][mailbox].append({"uid": uid, "raw_message": raw_message, "deleted": False})
            self.save()
            return uid

    def search_uids(self, mailbox: str):
        with self.lock:
            self.ensure_mailbox(mailbox)
            return [item["uid"] for item in self.data["mailboxes"][mailbox] if not item.get("deleted", False)]

    def fetch_messages(self, mailbox: str, requested_uids):
        with self.lock:
            self.ensure_mailbox(mailbox)
            allowed = set(requested_uids)
            return [
                item for item in self.data["mailboxes"][mailbox]
                if item["uid"] in allowed and not item.get("deleted", False)
            ]

    def mark_deleted(self, mailbox: str, requested_uids):
        with self.lock:
            self.ensure_mailbox(mailbox)
            deleted_count = 0
            allowed = set(requested_uids)
            for item in self.data["mailboxes"][mailbox]:
                if item["uid"] in allowed and not item.get("deleted", False):
                    item["deleted"] = True
                    deleted_count += 1
            self.save()
            return deleted_count

    def expunge(self, mailbox: str):
        with self.lock:
            self.ensure_mailbox(mailbox)
            items = self.data["mailboxes"][mailbox]
            remaining = [item for item in items if not item.get("deleted", False)]
            removed = len(items) - len(remaining)
            self.data["mailboxes"][mailbox] = remaining
            self.save()
            return removed


def parse_quoted_strings(text: str):
    return re.findall(r'"((?:[^"\\]|\\.)*)"', text)


class ImapHandler(socketserver.StreamRequestHandler):
    def setup(self):
        super().setup()
        self.username = None
        self.selected_mailbox = None

    def send_line(self, text: str) -> None:
        self.wfile.write(text.encode("utf-8") + b"\r\n")
        self.wfile.flush()

    def handle(self):
        self.send_line("* OK minimal test IMAP server ready")
        while True:
            raw_line = self.rfile.readline()
            if not raw_line:
                break
            line = raw_line.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line:
                continue

            parts = line.split(" ", 2)
            if len(parts) < 2:
                continue
            tag = parts[0]
            command = parts[1].upper()
            arguments = parts[2] if len(parts) > 2 else ""

            if command == "LOGIN":
                values = parse_quoted_strings(arguments)
                if len(values) != 2 or not self.server.store.login(values[0], values[1]):
                    self.send_line(f"{tag} NO LOGIN failed")
                    continue
                self.username = values[0]
                self.send_line(f"{tag} OK LOGIN completed")
            elif command == "LIST":
                values = parse_quoted_strings(arguments)
                pattern = values[-1] if values else "*"
                for mailbox in self.server.store.list_mailboxes(pattern):
                    self.send_line(f'* LIST (\\HasNoChildren) "/" "{mailbox}"')
                self.send_line(f"{tag} OK LIST completed")
            elif command == "SELECT":
                values = parse_quoted_strings(arguments)
                mailbox = values[0] if values else arguments.strip('"')
                self.server.store.ensure_mailbox(mailbox)
                self.selected_mailbox = mailbox
                count = len(self.server.store.search_uids(mailbox))
                self.send_line(f"* {count} EXISTS")
                self.send_line(f"{tag} OK [READ-WRITE] SELECT completed")
            elif command == "UID" and arguments.upper().startswith("SEARCH"):
                if not self.selected_mailbox:
                    self.send_line(f"{tag} BAD mailbox not selected")
                    continue
                uids = self.server.store.search_uids(self.selected_mailbox)
                uid_text = " ".join(str(uid) for uid in uids)
                self.send_line(f"* SEARCH {uid_text}".rstrip())
                self.send_line(f"{tag} OK SEARCH completed")
            elif command == "UID" and arguments.upper().startswith("FETCH"):
                if not self.selected_mailbox:
                    self.send_line(f"{tag} BAD mailbox not selected")
                    continue
                uid_list_text = arguments.split(" ", 2)[1]
                requested_uids = [int(item) for item in uid_list_text.split(",") if item]
                for seq, item in enumerate(self.server.store.fetch_messages(self.selected_mailbox, requested_uids), start=1):
                    payload = item["raw_message"].encode("utf-8")
                    self.wfile.write(
                        f"* {seq} FETCH (UID {item['uid']} BODY[] {{{len(payload)}}}\r\n".encode("utf-8")
                    )
                    self.wfile.write(payload)
                    self.wfile.write(b"\r\n)\r\n")
                    self.wfile.flush()
                self.send_line(f"{tag} OK FETCH completed")
            elif command == "UID" and arguments.upper().startswith("STORE"):
                if not self.selected_mailbox:
                    self.send_line(f"{tag} BAD mailbox not selected")
                    continue
                uid_list_text = arguments.split(" ", 3)[1]
                requested_uids = [int(item) for item in uid_list_text.split(",") if item]
                self.server.store.mark_deleted(self.selected_mailbox, requested_uids)
                self.send_line(f"{tag} OK STORE completed")
            elif command == "APPEND":
                values = parse_quoted_strings(arguments)
                mailbox = values[0] if values else "INBOX"
                match = re.search(r"\{(\d+)\}\s*$", arguments)
                if not match:
                    self.send_line(f"{tag} BAD APPEND missing literal size")
                    continue
                size = int(match.group(1))
                self.send_line("+ Ready for literal data")
                payload = self.rfile.read(size).decode("utf-8", errors="replace")
                uid = self.server.store.append_message(mailbox, payload)
                self.send_line(f"{tag} OK [APPENDUID 1 {uid}] APPEND completed")
            elif command == "EXPUNGE":
                if not self.selected_mailbox:
                    self.send_line(f"{tag} BAD mailbox not selected")
                    continue
                removed = self.server.store.expunge(self.selected_mailbox)
                for index in range(removed):
                    self.send_line(f"* {index + 1} EXPUNGE")
                self.send_line(f"{tag} OK EXPUNGE completed")
            elif command == "LOGOUT":
                self.send_line("* BYE LOGOUT requested")
                self.send_line(f"{tag} OK LOGOUT completed")
                break
            else:
                self.send_line(f"{tag} BAD unsupported command")


class ThreadedSSLServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True

    def __init__(self, server_address, handler_cls, ssl_context, store):
        self.ssl_context = ssl_context
        self.store = store
        super().__init__(server_address, handler_cls)

    def get_request(self):
        newsocket, fromaddr = self.socket.accept()
        connstream = self.ssl_context.wrap_socket(newsocket, server_side=True)
        return connstream, fromaddr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=1993)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--store", required=True)
    parser.add_argument("--username", required=True)
    parser.add_argument("--password", required=True)
    args = parser.parse_args()

    store = MailStore(Path(args.store))
    store.set_user(args.username, args.password)
    store.ensure_mailbox("Archive")
    store.save()

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=args.cert, keyfile=args.key)

    with ThreadedSSLServer((args.host, args.port), ImapHandler, context, store) as server:
        server.serve_forever()


if __name__ == "__main__":
    main()
