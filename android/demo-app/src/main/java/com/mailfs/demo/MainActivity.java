package com.mailfs.demo;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.mailfs.android.MailfsHttpServer;

import java.io.File;

public final class MainActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final MailfsHttpServer mailfs = new MailfsHttpServer();

    private EditText imapHost;
    private EditText imapPort;
    private EditText username;
    private EditText password;
    private EditText listenAddr;
    private EditText databasePath;
    private EditText downloadDir;
    private EditText defaultMailbox;
    private EditText commandMailbox;
    private EditText pathArg;
    private EditText outputPath;
    private EditText uidArg;
    private CheckBox allowInsecureTls;
    private TextView status;
    private TextView output;

    private final Runnable statusTicker = new Runnable() {
        @Override
        public void run() {
            updateStatus();
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 7);
        }
        setContentView(buildUi());
        loadPrefs();
        updateStatus();
        handler.post(statusTicker);
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacks(statusTicker);
        super.onDestroy();
    }

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = vertical();
        int pad = dp(18);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);

        TextView title = new TextView(this);
        title.setText("MailFS Android Demo");
        title.setTextSize(24);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView note = new TextView(this);
        note.setText("Run the same MailFS commands as the desktop CLI. The default SQLite cache DB lives under the app cache directory.");
        note.setPadding(0, dp(8), 0, dp(12));
        root.addView(note);

        addSection(root, "Connection");
        imapHost = addField(root, "IMAP host", "imap.qq.com", InputType.TYPE_CLASS_TEXT);
        imapPort = addField(root, "IMAP port", "993", InputType.TYPE_CLASS_NUMBER);
        username = addField(root, "Email", "", InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS);
        password = addField(root, "IMAP password/auth code", "", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        allowInsecureTls = new CheckBox(this);
        allowInsecureTls.setText("Allow insecure TLS");
        allowInsecureTls.setChecked(true);
        root.addView(allowInsecureTls);

        addSection(root, "Storage");
        databasePath = addField(root, "SQLite cache DB path", "default: app cache/mailfs/mailfs_cache.db", InputType.TYPE_CLASS_TEXT);
        downloadDir = addField(root, "Download directory", "default: app files/mailfs/downloads", InputType.TYPE_CLASS_TEXT);
        defaultMailbox = addField(root, "Default mailbox", "", InputType.TYPE_CLASS_TEXT);
        listenAddr = addField(root, "HTTP listen address", "127.0.0.1:9888", InputType.TYPE_CLASS_TEXT);

        addSection(root, "HTTP Server");
        LinearLayout serverButtons = horizontal();
        root.addView(serverButtons);
        addButton(serverButtons, "Start HTTP", v -> startServer());
        addButton(serverButtons, "Stop HTTP", v -> stopServer());
        addButton(serverButtons, "Copy URL", v -> copyBaseUrl());
        addButton(serverButtons, "Open URL", v -> openBaseUrl());

        addSection(root, "Command Args");
        commandMailbox = addField(root, "Mailbox override", "empty uses Default mailbox", InputType.TYPE_CLASS_TEXT);
        pathArg = addField(root, "Path / prefix", "file path, localpath, or prefix", InputType.TYPE_CLASS_TEXT);
        outputPath = addField(root, "Playlist output path", "empty returns JSON in output", InputType.TYPE_CLASS_TEXT);
        uidArg = addField(root, "UID", "for delete-uid", InputType.TYPE_CLASS_NUMBER);

        addSection(root, "CLI Commands");
        addButtonRow(root, "list-mailboxes", "cache-mailbox", "list-cache");
        addButtonRow(root, "check-integrity", "dedup-mailbox", "delete-uid");
        addButtonRow(root, "export-playlist", "upload", "download");

        status = new TextView(this);
        status.setPadding(0, dp(16), 0, 0);
        root.addView(status);

        output = new TextView(this);
        output.setTextIsSelectable(true);
        output.setTypeface(Typeface.MONOSPACE);
        output.setPadding(0, dp(12), 0, 0);
        output.setText("Output will appear here.");
        root.addView(output);

        return scroll;
    }

    private void addSection(LinearLayout root, String text) {
        TextView label = new TextView(this);
        label.setText(text);
        label.setTextSize(18);
        label.setTypeface(Typeface.DEFAULT_BOLD);
        label.setPadding(0, dp(18), 0, dp(4));
        root.addView(label);
    }

    private EditText addField(LinearLayout root, String label, String hint, int inputType) {
        TextView text = new TextView(this);
        text.setText(label);
        text.setPadding(0, dp(8), 0, 0);
        root.addView(text);

        EditText field = new EditText(this);
        field.setSingleLine(true);
        field.setHint(hint);
        field.setInputType(inputType);
        root.addView(field);
        return field;
    }

    private void addButtonRow(LinearLayout root, String first, String second, String third) {
        LinearLayout row = horizontal();
        root.addView(row);
        addButton(row, first, v -> runCommand(first));
        addButton(row, second, v -> runCommand(second));
        addButton(row, third, v -> runCommand(third));
    }

    private void addButton(LinearLayout root, String text, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setOnClickListener(listener);
        root.addView(button, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
    }

    private LinearLayout vertical() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    private LinearLayout horizontal() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        return layout;
    }

    private void startServer() {
        savePrefs();
        Intent intent = new Intent(this, MailfsHttpServerService.class);
        intent.setAction(MailfsHttpServerService.ACTION_START);
        intent.putExtra(MailfsHttpServerService.EXTRA_IMAP_HOST, text(imapHost));
        intent.putExtra(MailfsHttpServerService.EXTRA_IMAP_PORT, parsePort());
        intent.putExtra(MailfsHttpServerService.EXTRA_USERNAME, text(username));
        intent.putExtra(MailfsHttpServerService.EXTRA_PASSWORD, text(password));
        intent.putExtra(MailfsHttpServerService.EXTRA_LISTEN_ADDR, textOr(listenAddr, "127.0.0.1:9888"));
        intent.putExtra(MailfsHttpServerService.EXTRA_DATABASE_PATH, text(databasePath));
        intent.putExtra(MailfsHttpServerService.EXTRA_DOWNLOAD_DIR, text(downloadDir));
        intent.putExtra(MailfsHttpServerService.EXTRA_DEFAULT_MAILBOX, text(defaultMailbox));
        intent.putExtra(MailfsHttpServerService.EXTRA_ALLOW_INSECURE_TLS, allowInsecureTls.isChecked());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
        updateStatus();
    }

    private void stopServer() {
        Intent intent = new Intent(this, MailfsHttpServerService.class);
        intent.setAction(MailfsHttpServerService.ACTION_STOP);
        startService(intent);
        updateStatus();
    }

    private void runCommand(String action) {
        savePrefs();
        output.setText("Running " + action + "...");
        new Thread(() -> {
            String result;
            try {
                MailfsHttpServer.Command command = MailfsHttpServer.Command.of(action);
                command.mailbox = text(commandMailbox);
                command.path = text(pathArg);
                command.outputPath = text(outputPath);
                command.uid = parseUid();
                result = mailfs.runCommand(this, buildConfig(), command);
            } catch (Exception ex) {
                result = "error: " + ex.getMessage() + "\n";
            }
            String finalResult = result;
            runOnUiThread(() -> {
                output.setText(finalResult.isEmpty() ? "(no output)" : finalResult);
                updateStatus();
            });
        }, "mailfs-command-" + action).start();
    }

    private MailfsHttpServer.Config buildConfig() {
        MailfsHttpServer.Config config = new MailfsHttpServer.Config();
        config.imapHost = textOr(imapHost, "imap.qq.com");
        config.imapPort = parsePort();
        config.username = text(username);
        config.password = text(password);
        config.listenAddr = textOr(listenAddr, "127.0.0.1:9888");
        config.copyAddr = "http://" + config.listenAddr;
        config.databasePath = text(databasePath);
        config.downloadDir = text(downloadDir);
        config.defaultMailbox = text(defaultMailbox);
        config.allowInsecureTls = allowInsecureTls.isChecked();
        return config;
    }

    private void copyBaseUrl() {
        String url = baseUrl();
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard != null) {
            clipboard.setPrimaryClip(ClipData.newPlainText("MailFS HTTP URL", url));
            Toast.makeText(this, "Copied " + url, Toast.LENGTH_SHORT).show();
        }
    }

    private void openBaseUrl() {
        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(baseUrl())));
    }

    private String baseUrl() {
        return "http://" + textOr(listenAddr, "127.0.0.1:9888") + "/";
    }

    private int parsePort() {
        try {
            return Integer.parseInt(text(imapPort));
        } catch (NumberFormatException ex) {
            return 993;
        }
    }

    private long parseUid() {
        try {
            return Long.parseLong(text(uidArg));
        } catch (NumberFormatException ex) {
            return 0;
        }
    }

    private void updateStatus() {
        File defaultDb = mailfs.defaultDatabaseFile(this);
        String running = mailfs.isRunning() ? "running" : "stopped";
        String error = mailfs.getLastError();
        String message = "HTTP: " + running
                + "\nFiles: " + getFilesDir().getAbsolutePath()
                + "\nCache: " + getCacheDir().getAbsolutePath()
                + "\nDefault DB: " + defaultDb.getAbsolutePath();
        if (error != null && !error.isEmpty()) {
            message += "\nLast native error: " + error;
        }
        status.setText(message);
    }

    private void loadPrefs() {
        SharedPreferences prefs = getSharedPreferences("mailfs", MODE_PRIVATE);
        imapHost.setText(prefs.getString("imapHost", "imap.qq.com"));
        imapPort.setText(prefs.getString("imapPort", "993"));
        username.setText(prefs.getString("username", ""));
        listenAddr.setText(prefs.getString("listenAddr", "127.0.0.1:9888"));
        databasePath.setText(prefs.getString("databasePath", ""));
        downloadDir.setText(prefs.getString("downloadDir", ""));
        defaultMailbox.setText(prefs.getString("defaultMailbox", ""));
        commandMailbox.setText(prefs.getString("commandMailbox", ""));
        pathArg.setText(prefs.getString("pathArg", ""));
        outputPath.setText(prefs.getString("outputPath", ""));
        allowInsecureTls.setChecked(prefs.getBoolean("allowInsecureTls", true));
    }

    private void savePrefs() {
        getSharedPreferences("mailfs", MODE_PRIVATE)
                .edit()
                .putString("imapHost", text(imapHost))
                .putString("imapPort", text(imapPort))
                .putString("username", text(username))
                .putString("listenAddr", text(listenAddr))
                .putString("databasePath", text(databasePath))
                .putString("downloadDir", text(downloadDir))
                .putString("defaultMailbox", text(defaultMailbox))
                .putString("commandMailbox", text(commandMailbox))
                .putString("pathArg", text(pathArg))
                .putString("outputPath", text(outputPath))
                .putBoolean("allowInsecureTls", allowInsecureTls.isChecked())
                .apply();
    }

    private String text(EditText field) {
        return field.getText().toString().trim();
    }

    private String textOr(EditText field, String fallback) {
        String value = text(field);
        return value.isEmpty() ? fallback : value;
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
