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

Each command opens in its own Activity from the demo home screen. For uploads, the `Upload` screen has
file and folder picker buttons. Picked `content://` items are copied into the app cache import directory
first, then uploaded through the native `upload` command. `List cache` renders a local-path tree with
expandable folders; tapping a cached file starts the bundled ARtc player with that file's HTTP URL, and
long-pressing a file opens its details.

The demo app packages the external `artcplayer_release_1.0.15_2026030516.aar` player from the sibling
player demo and currently restricts the APK ABI to `arm64-v8a` to match that player package.

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
