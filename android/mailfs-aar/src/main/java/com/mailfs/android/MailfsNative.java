package com.mailfs.android;

final class MailfsNative {
    static {
        System.loadLibrary("mailfs_jni");
    }

    private MailfsNative() {
    }

    static native boolean start(
            String imapHost,
            int imapPort,
            String credentialFile,
            String caCertFile,
            boolean allowInsecureTls,
            String logLevel,
            String logFile,
            String databasePath,
            String downloadDir,
            String listenAddr,
            String copyAddr,
            String defaultMailbox,
            String emailName,
            String ownerName,
            long defaultBlockSize,
            int cacheFetchBatchSize);

    static native void stop();

    static native boolean isRunning();

    static native String lastError();

    static native String runCommand(String requestJson);

    static native String runCommandWithProgress(String requestJson, MailfsHttpServer.ProgressListener progressListener);
}
