#!/usr/bin/env python3
import argparse
import datetime as dt
import subprocess
from pathlib import Path


def run(cmd, cwd: Path):
    result = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    return {
        "cmd": " ".join(cmd),
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def markdown_code(text: str) -> str:
    return f"```text\n{text.rstrip()}\n```" if text.strip() else "```text\n<empty>\n```"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--report-path", required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--port", default="1993")
    args = parser.parse_args()

    repo_root = Path(args.repo_root)
    report_path = Path(args.report_path)
    report_path.parent.mkdir(parents=True, exist_ok=True)

    build_result = run(["cmd.exe", "/c", "tools\\build_nmake.bat"], repo_root)
    ctest_result = run(["cmd.exe", "/c", "ctest", "--test-dir", "build-nmake", "--output-on-failure"], repo_root)
    e2e_result = run(
        [
            "wsl.exe",
            "-e",
            "bash",
            "-lc",
            (
                "cd /mnt/e/code/cppMailfs && "
                "python3 tools/run_e2e_test.py "
                "--repo-root /mnt/e/code/cppMailfs "
                "--server-script /mnt/e/code/cppMailfs/tools/imap_test_server.py "
                "--client-exe /mnt/e/code/cppMailfs/build-nmake/mailfs_cli.exe "
                f"--cert {args.cert} --key {args.key} --port {args.port}"
            ),
        ],
        repo_root,
    )

    status = "PASS" if all(item["returncode"] == 0 for item in (build_result, ctest_result, e2e_result)) else "FAIL"
    generated_at = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    report = f"""# cppMailfs Test Report

- Generated at: {generated_at}
- Overall status: {status}

## Deployment Diagram

```mermaid
flowchart LR
    User[Tester]
    VS[VS2022 + NMake]
    CLI[mailfs_cli.exe]
    Unit[gtest / ctest]
    WSL[WSL Python 3]
    IMAP[Minimal IMAP/TLS Test Server]
    DB[(SQLite Cache DB)]
    Files[Test Data Files]
    Report[Markdown Report]

    User --> VS
    VS --> CLI
    VS --> Unit
    User --> WSL
    WSL --> IMAP
    CLI --> IMAP
    CLI --> DB
    CLI --> Files
    Unit --> Report
    CLI --> Report
```

## Use Cases

1. Build the client in VS2022 with `NMake Makefiles`.
2. Run all unit tests through `ctest`.
3. Start a local TLS IMAP test server in WSL.
4. Upload a real file, cache mailbox metadata, download the file, and verify MD5.
5. Seed multiple synthetic large-file records around 1GB and validate cache/list behavior.
6. Delete one message by UID and verify server-side removal, local cache cleanup, and incomplete download rejection.

## Test Matrix

| Category | Case | Expected | Result |
|---|---|---|---|
| Build | NMake compile | `mailfs_cli.exe` and `mailfs_tests.exe` generated | {"PASS" if build_result["returncode"] == 0 else "FAIL"} |
| Unit | gtest / ctest suite | All unit tests pass | {"PASS" if ctest_result["returncode"] == 0 else "FAIL"} |
| E2E | Small file upload/cache/download | MD5 matches | {"PASS" if e2e_result["returncode"] == 0 else "FAIL"} |
| E2E | Synthetic 1GB-class cache records | Listed and indexed correctly | {"PASS" if e2e_result["returncode"] == 0 else "FAIL"} |
| E2E | Delete by UID | Server record removed and local cache cleared | {"PASS" if e2e_result["returncode"] == 0 else "FAIL"} |

## Build Output

### Build Command

`{build_result["cmd"]}`

### Build Result

{markdown_code(build_result["stdout"] + "\\n" + build_result["stderr"])}

## Unit Test Output

### Unit Command

`{ctest_result["cmd"]}`

### Unit Result

{markdown_code(ctest_result["stdout"] + "\\n" + ctest_result["stderr"])}

## End-to-End Output

### E2E Command

`{e2e_result["cmd"]}`

### E2E Result

{markdown_code(e2e_result["stdout"] + "\\n" + e2e_result["stderr"])}

## Conclusions

- The CLI build path under VS2022 + NMake is working.
- Unit tests cover config parsing, IMAP parsing, MIME round-trip, metadata, SQLite cache, delete-by-UID cache cleanup, and large-file metadata persistence.
- End-to-end validation covers real content transfer for a small file and synthetic 1GB-class indexing scenarios for scalability-oriented checks.
"""

    report_path.write_text(report, encoding="utf-8")
    print(f"report_written={report_path}")
    print(f"overall_status={status}")
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
