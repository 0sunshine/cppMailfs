package com.mailfs.demo;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;

public final class MainActivity extends Activity {
    private EditText imapHost;
    private EditText imapPort;
    private EditText username;
    private EditText password;
    private EditText listenAddr;
    private EditText databasePath;
    private EditText downloadDir;
    private EditText defaultMailbox;
    private CheckBox allowInsecureTls;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 7);
        }
        setContentView(buildUi());
        loadPrefs();
    }

    @Override
    protected void onPause() {
        savePrefs();
        super.onPause();
    }

    private ScrollView buildUi() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = Ui.vertical(this);
        int pad = Ui.dp(this, 18);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);

        root.addView(Ui.title(this, "MailFS Demo"));
        root.addView(Ui.body(this, "Configure once, then open each command in its own screen."));

        root.addView(Ui.section(this, "Connection"));
        imapHost = Ui.field(this, root, "IMAP host", "imap.qq.com", InputType.TYPE_CLASS_TEXT);
        imapPort = Ui.field(this, root, "IMAP port", "993", InputType.TYPE_CLASS_NUMBER);
        username = Ui.field(this, root, "Email", "", InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS);
        password = Ui.field(this, root, "IMAP password/auth code", "", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        allowInsecureTls = new CheckBox(this);
        allowInsecureTls.setText("Allow insecure TLS");
        root.addView(allowInsecureTls);

        root.addView(Ui.section(this, "Storage"));
        databasePath = Ui.field(this, root, "SQLite cache DB path", "default: app cache/mailfs/mailfs_cache.db", InputType.TYPE_CLASS_TEXT);
        downloadDir = Ui.field(this, root, "Download directory", "default: app files/mailfs/downloads", InputType.TYPE_CLASS_TEXT);
        defaultMailbox = Ui.field(this, root, "Default mailbox", "", InputType.TYPE_CLASS_TEXT);
        listenAddr = Ui.field(this, root, "HTTP listen address", "127.0.0.1:9888", InputType.TYPE_CLASS_TEXT);

        root.addView(Ui.section(this, "Commands"));
        addCommand(root, "HTTP server", HttpServerActivity.class);
        addCommand(root, "List mailboxes", ListMailboxesActivity.class);
        addCommand(root, "Cache mailbox", CacheMailboxActivity.class);
        addCommand(root, "List cache", ListCacheActivity.class);
        addCommand(root, "Check integrity", CheckIntegrityActivity.class);
        addCommand(root, "Deduplicate mailbox", DedupMailboxActivity.class);
        addCommand(root, "Delete UID", DeleteUidActivity.class);
        addCommand(root, "Export playlist", ExportPlaylistActivity.class);
        addCommand(root, "Upload", UploadActivity.class);
        addCommand(root, "Download", DownloadActivity.class);
        return scroll;
    }

    private void addCommand(LinearLayout root, String label, Class<?> activity) {
        Ui.addButton(root, Ui.button(this, label, v -> {
            savePrefs();
            startActivity(new Intent(this, activity));
        }));
    }

    private void loadPrefs() {
        SharedPreferences prefs = MailfsSettings.prefs(this);
        imapHost.setText(prefs.getString("imapHost", "imap.qq.com"));
        imapPort.setText(prefs.getString("imapPort", "993"));
        username.setText(prefs.getString("username", ""));
        password.setText(prefs.getString("password", ""));
        listenAddr.setText(prefs.getString("listenAddr", "127.0.0.1:9888"));
        databasePath.setText(prefs.getString("databasePath", ""));
        downloadDir.setText(prefs.getString("downloadDir", ""));
        defaultMailbox.setText(prefs.getString("defaultMailbox", ""));
        allowInsecureTls.setChecked(prefs.getBoolean("allowInsecureTls", true));
    }

    private void savePrefs() {
        MailfsSettings.prefs(this).edit()
                .putString("imapHost", text(imapHost))
                .putString("imapPort", text(imapPort))
                .putString("username", text(username))
                .putString("password", text(password))
                .putString("listenAddr", text(listenAddr))
                .putString("databasePath", text(databasePath))
                .putString("downloadDir", text(downloadDir))
                .putString("defaultMailbox", text(defaultMailbox))
                .putBoolean("allowInsecureTls", allowInsecureTls.isChecked())
                .apply();
    }

    private String text(EditText field) {
        return field.getText().toString().trim();
    }
}
