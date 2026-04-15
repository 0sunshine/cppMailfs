package com.mailfs.demo;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

import com.mailfs.android.MailfsHttpServer;

public final class MailfsHttpServerService extends Service {
    public static final String ACTION_START = "com.mailfs.demo.START";
    public static final String ACTION_STOP = "com.mailfs.demo.STOP";
    public static final String EXTRA_IMAP_HOST = "imapHost";
    public static final String EXTRA_IMAP_PORT = "imapPort";
    public static final String EXTRA_USERNAME = "username";
    public static final String EXTRA_PASSWORD = "password";
    public static final String EXTRA_LISTEN_ADDR = "listenAddr";
    public static final String EXTRA_DATABASE_PATH = "databasePath";
    public static final String EXTRA_DOWNLOAD_DIR = "downloadDir";
    public static final String EXTRA_DEFAULT_MAILBOX = "defaultMailbox";
    public static final String EXTRA_ALLOW_INSECURE_TLS = "allowInsecureTls";

    private static final String CHANNEL_ID = "mailfs_http";
    private static final int NOTIFICATION_ID = 1001;

    private final MailfsHttpServer server = new MailfsHttpServer();

    @Override
    public void onCreate() {
        super.onCreate();
        ensureNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopSelf();
            return START_NOT_STICKY;
        }

        startForeground(NOTIFICATION_ID, buildNotification("Starting on " + readListenAddr(intent)));

        try {
            MailfsHttpServer.Config config = new MailfsHttpServer.Config();
            config.imapHost = stringExtra(intent, EXTRA_IMAP_HOST, "imap.qq.com");
            config.imapPort = intExtra(intent, EXTRA_IMAP_PORT, 993);
            config.username = stringExtra(intent, EXTRA_USERNAME, "");
            config.password = stringExtra(intent, EXTRA_PASSWORD, "");
            config.listenAddr = readListenAddr(intent);
            config.copyAddr = "http://" + config.listenAddr;
            config.databasePath = stringExtra(intent, EXTRA_DATABASE_PATH, "");
            config.downloadDir = stringExtra(intent, EXTRA_DOWNLOAD_DIR, "");
            config.defaultMailbox = stringExtra(intent, EXTRA_DEFAULT_MAILBOX, "");
            config.allowInsecureTls = boolExtra(intent, EXTRA_ALLOW_INSECURE_TLS, true);

            boolean started = server.start(this, config);
            if (!started) {
                startForeground(NOTIFICATION_ID, buildNotification("Failed: " + server.getLastError()));
                stopSelf();
                return START_NOT_STICKY;
            }
            startForeground(NOTIFICATION_ID, buildNotification("Serving on " + config.listenAddr));
            return START_STICKY;
        } catch (Exception ex) {
            startForeground(NOTIFICATION_ID, buildNotification("Failed: " + ex.getMessage()));
            stopSelf();
            return START_NOT_STICKY;
        }
    }

    @Override
    public void onDestroy() {
        server.stop();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "MailFS HTTP",
                NotificationManager.IMPORTANCE_LOW);
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) {
            manager.createNotificationChannel(channel);
        }
    }

    private Notification buildNotification(String text) {
        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return builder
                .setSmallIcon(R.drawable.ic_mailfs_service)
                .setContentTitle("MailFS HTTP server")
                .setContentText(text)
                .setOngoing(true)
                .build();
    }

    private static String readListenAddr(Intent intent) {
        return stringExtra(intent, EXTRA_LISTEN_ADDR, "127.0.0.1:9888");
    }

    private static String stringExtra(Intent intent, String key, String fallback) {
        if (intent == null) {
            return fallback;
        }
        String value = intent.getStringExtra(key);
        return value == null || value.trim().isEmpty() ? fallback : value.trim();
    }

    private static int intExtra(Intent intent, String key, int fallback) {
        return intent == null ? fallback : intent.getIntExtra(key, fallback);
    }

    private static boolean boolExtra(Intent intent, String key, boolean fallback) {
        return intent == null ? fallback : intent.getBooleanExtra(key, fallback);
    }
}
