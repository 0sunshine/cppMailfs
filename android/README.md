# cppMailfs Android

This Android workspace contains:

- `mailfs-aar`: AAR library that wraps the native `HttpImapDownloadServer` through JNI.
- `demo-app`: small APK that starts/stops the HTTP-to-IMAP server from a foreground service.

Build from this directory:

```powershell
.\gradlew.bat :mailfs-aar:assembleRelease :demo-app:assembleDebug
```

Outputs:

- `mailfs-aar/build/outputs/aar/mailfs-aar-release.aar`
- `demo-app/build/outputs/apk/debug/demo-app-debug.apk`

The AAR API is `com.mailfs.android.MailfsHttpServer`.

```java
MailfsHttpServer server = new MailfsHttpServer();
MailfsHttpServer.Config config = new MailfsHttpServer.Config();
config.username = "your_email@example.com";
config.password = "your_imap_password_or_auth_code";
config.listenAddr = "127.0.0.1:9888";
server.start(context, config);
```

The demo also exposes the CLI commands:

- `list-mailboxes`
- `cache-mailbox`
- `list-cache`
- `check-integrity`
- `dedup-mailbox`
- `delete-uid`
- `export-playlist`
- `upload`
- `download`
- `serve-http`

You can run the same commands from the AAR:

```java
MailfsHttpServer.Command command = MailfsHttpServer.Command.of("list-cache");
command.mailbox = "INBOX";
String output = server.runCommand(context, config, command);
```

By default the SQLite cache DB is stored at:

```text
context.getCacheDir()/mailfs/mailfs_cache.db
```

`serve-http` streams files from that SQLite cache index. If you already have a desktop-generated
`mailfs_cache.db`, copy it to the app cache path shown by the demo, or pass an absolute path through
`config.databasePath`.
