package com.mailfs.android;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

import org.json.JSONException;
import org.json.JSONObject;

public final class MailfsHttpServer implements AutoCloseable {
    public static final class Config {
        public String imapHost = "imap.qq.com";
        public int imapPort = 993;
        public String username = "";
        public String password = "";
        public String credentialFile = "";
        public String caCertFile = "";
        public boolean allowInsecureTls = true;
        public String logLevel = "info";
        public String logFile = "";
        public String databasePath = "";
        public String downloadDir = "";
        public String listenAddr = "127.0.0.1:9888";
        public String copyAddr = "http://127.0.0.1:9888";
        public String defaultMailbox = "";
        public String emailName = "mailfs";
        public String ownerName = "android";
        public long defaultBlockSize = 32L * 1024L * 1024L;
        public int cacheFetchBatchSize = 32;
    }

    public static final class Command {
        public String action = "";
        public String mailbox = "";
        public String path = "";
        public String outputPath = "";
        public long uid = 0;

        public static Command of(String action) {
            Command command = new Command();
            command.action = action;
            return command;
        }
    }

    public interface ProgressListener {
        void onProgress(String action, long done, long total, String message);
    }

    public synchronized boolean start(Context context, Config config) throws IOException {
        PreparedConfig prepared = prepareConfig(context, config);

        return MailfsNative.start(
                prepared.imapHost,
                prepared.imapPort,
                prepared.credentialFile,
                prepared.caCertFile,
                prepared.allowInsecureTls,
                prepared.logLevel,
                prepared.logFile,
                prepared.databasePath,
                prepared.downloadDir,
                prepared.listenAddr,
                prepared.copyAddr,
                prepared.defaultMailbox,
                prepared.emailName,
                prepared.ownerName,
                prepared.defaultBlockSize,
                prepared.cacheFetchBatchSize);
    }

    public synchronized String runCommand(Context context, Config config, Command command) throws IOException {
        return runCommand(context, config, command, null);
    }

    public synchronized String runCommand(
            Context context,
            Config config,
            Command command,
            ProgressListener progressListener) throws IOException {
        PreparedConfig prepared = prepareConfig(context, config);
        try {
            JSONObject root = new JSONObject();
            root.put("command", valueOrEmpty(command.action));
            root.put("mailbox", valueOrEmpty(command.mailbox));
            root.put("path", valueOrEmpty(command.path));
            root.put("outputPath", valueOrEmpty(command.outputPath));
            root.put("uid", command.uid);
            root.put("config", prepared.toJson());
            if (progressListener == null) {
                return MailfsNative.runCommand(root.toString());
            }
            return MailfsNative.runCommandWithProgress(root.toString(), progressListener);
        } catch (JSONException ex) {
            throw new IOException("Failed to build command request", ex);
        }
    }

    public File defaultDatabaseFile(Context context) {
        return new File(mailfsCacheDir(context.getApplicationContext()), "mailfs_cache.db");
    }

    private PreparedConfig prepareConfig(Context context, Config config) throws IOException {
        Context appContext = context.getApplicationContext();
        File baseDir = new File(appContext.getFilesDir(), "mailfs");
        if (!baseDir.exists() && !baseDir.mkdirs()) {
            throw new IOException("Failed to create " + baseDir);
        }
        File cacheDir = mailfsCacheDir(appContext);
        if (!cacheDir.exists() && !cacheDir.mkdirs()) {
            throw new IOException("Failed to create " + cacheDir);
        }

        String credentialFile = valueOrEmpty(config.credentialFile);
        if (credentialFile.isEmpty()) {
            File credentials = new File(baseDir, "credentials.txt");
            if (!valueOrEmpty(config.username).isEmpty() && !valueOrEmpty(config.password).isEmpty()) {
                writePrivateText(credentials, config.username + "\n" + config.password + "\n");
            }
            credentialFile = credentials.getAbsolutePath();
        }

        String databasePath = valueOrEmpty(config.databasePath);
        if (databasePath.isEmpty()) {
            databasePath = new File(cacheDir, "mailfs_cache.db").getAbsolutePath();
        }

        String logFile = valueOrEmpty(config.logFile);
        if (logFile.isEmpty()) {
            logFile = new File(baseDir, "mailfs.log").getAbsolutePath();
        }

        String downloadDir = valueOrEmpty(config.downloadDir);
        if (downloadDir.isEmpty()) {
            downloadDir = new File(baseDir, "downloads").getAbsolutePath();
        }

        PreparedConfig prepared = new PreparedConfig();
        prepared.imapHost = valueOrDefault(config.imapHost, "imap.qq.com");
        prepared.imapPort = config.imapPort;
        prepared.credentialFile = credentialFile;
        prepared.caCertFile = valueOrEmpty(config.caCertFile);
        prepared.allowInsecureTls = config.allowInsecureTls;
        prepared.logLevel = valueOrDefault(config.logLevel, "info");
        prepared.logFile = logFile;
        prepared.databasePath = databasePath;
        prepared.downloadDir = downloadDir;
        prepared.listenAddr = valueOrDefault(config.listenAddr, "127.0.0.1:9888");
        prepared.copyAddr = valueOrDefault(config.copyAddr, "http://127.0.0.1:9888");
        prepared.defaultMailbox = valueOrEmpty(config.defaultMailbox);
        prepared.emailName = valueOrDefault(config.emailName, "mailfs");
        prepared.ownerName = valueOrDefault(config.ownerName, "android");
        prepared.defaultBlockSize = config.defaultBlockSize;
        prepared.cacheFetchBatchSize = config.cacheFetchBatchSize;
        return prepared;
    }

    public synchronized void stop() {
        MailfsNative.stop();
    }

    public boolean isRunning() {
        return MailfsNative.isRunning();
    }

    public String getLastError() {
        return MailfsNative.lastError();
    }

    @Override
    public void close() {
        stop();
    }

    private static void writePrivateText(File file, String text) throws IOException {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("Failed to create " + parent);
        }
        try (FileOutputStream output = new FileOutputStream(file, false)) {
            output.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String valueOrDefault(String value, String fallback) {
        String text = valueOrEmpty(value);
        return text.isEmpty() ? fallback : text;
    }

    private static String valueOrEmpty(String value) {
        return value == null ? "" : value.trim();
    }

    private static File mailfsCacheDir(Context appContext) {
        return new File(appContext.getCacheDir(), "mailfs");
    }

    private static final class PreparedConfig {
        String imapHost;
        int imapPort;
        String credentialFile;
        String caCertFile;
        boolean allowInsecureTls;
        String logLevel;
        String logFile;
        String databasePath;
        String downloadDir;
        String listenAddr;
        String copyAddr;
        String defaultMailbox;
        String emailName;
        String ownerName;
        long defaultBlockSize;
        int cacheFetchBatchSize;

        JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("imapHost", imapHost);
            json.put("imapPort", imapPort);
            json.put("credentialFile", credentialFile);
            json.put("caCertFile", caCertFile);
            json.put("allowInsecureTls", allowInsecureTls);
            json.put("logLevel", logLevel);
            json.put("logFile", logFile);
            json.put("databasePath", databasePath);
            json.put("downloadDir", downloadDir);
            json.put("listenAddr", listenAddr);
            json.put("copyAddr", copyAddr);
            json.put("defaultMailbox", defaultMailbox);
            json.put("emailName", emailName);
            json.put("ownerName", ownerName);
            json.put("defaultBlockSize", defaultBlockSize);
            json.put("cacheFetchBatchSize", cacheFetchBatchSize);
            return json;
        }
    }
}
