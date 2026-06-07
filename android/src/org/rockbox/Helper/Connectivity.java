package org.rockbox.Helper;

import android.util.Log;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.File;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.IOException;
import java.io.UnsupportedEncodingException;
import java.net.URLEncoder;
import java.nio.charset.Charset;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import android.util.Base64;
import org.json.JSONObject;
import org.json.JSONException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.HashMap;

public class Connectivity{   
    // Context for WiFi operations
    private static Context context = null;
    
    public static void setContext(Context ctx) {
        context = ctx;
    }

    public static boolean lastFm(boolean listenbrainz, String apiUrl, String username, String password, String apiKey, String sharedSecret, String artist, String track, String album, int timestamp, long length){
        // make sure all parameters are encoded correctly
        String username_enc;
        String password_enc;
        String artist_enc;
        String track_enc;
        String album_enc;
        try {
            username_enc = URLEncoder.encode(username, "UTF-8");
            password_enc = URLEncoder.encode(password, "UTF-8");

            artist_enc = URLEncoder.encode(artist, "UTF-8");
            track_enc = URLEncoder.encode(track, "UTF-8");
            album_enc = URLEncoder.encode(album, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            Log.d("RockboxService", "Last.fm: Error URL encoding: " + e.getMessage());
            return false;
        }

        String command;
        String response;
        if (!listenbrainz) {
            // auth
            String method = "auth.getMobileSession";
            String signBase = "api_key" + apiKey +
                        "method" + method + 
                        "password" + password +
                        "username" + username + sharedSecret;
            String apiSig = md5sum(signBase);

            command = "/data/data/gocurl --cacert /data/data/cacert.pem -s " +
                                "-X POST " +
                                "--url '" + apiUrl +"' " +
                                "-d " +
                                "'method="+method+
                                "&api_key="+apiKey+
                                "&username="+username_enc+
                                "&password="+password_enc+
                                "&api_sig="+apiSig+
                                "&format=json'";
            response = execShell(command);
            String sessionKey = "";
            try {
                JSONObject jsonObject = new JSONObject(response);
                if (jsonObject.has("session")) {
                    JSONObject session = jsonObject.getJSONObject("session");
                    if (session.has("key")) {
                        sessionKey = session.getString("key");
                    }
                }
            } catch (JSONException e) {
                Log.e("RockboxService", "Last.fm: json error: " + e.getMessage());
                return false;
            }

            // actual scrobble request
            method="track.scrobble";
            signBase =  "album"+album+
                        "api_key"+apiKey+
                        "artist"+artist+
                        "method"+method+
                        "sk"+sessionKey+
                        "timestamp"+String.valueOf(timestamp)+
                        "track"+track+sharedSecret;
            apiSig = md5sum(signBase);
            command = "/data/data/gocurl --cacert /data/data/cacert.pem -s " +
                        "-X POST " +
                        "--url '" + apiUrl +"' " +
                        "-d " +
                        "'method="+method+
                        "&api_key="+apiKey+
                        "&sk="+sessionKey+
                        "&artist="+artist_enc+
                        "&track="+track_enc+
                        "&album="+album_enc+
                        "&timestamp="+String.valueOf(timestamp)+
                        "&api_sig="+apiSig+
                        "&format=json'";
        } else {
            track_enc = track.replace("'", "'\\''");
            artist_enc = artist.replace("'", "'\\''");
            album_enc = album.replace("'", "'\\''");
            String jsonPayload = "{"
                + "\"listen_type\": \"single\","
                + "\"payload\": ["
                +   "{"
                +   "\"listened_at\":" + String.valueOf(timestamp) + ","
                +   "\"track_metadata\": {"
                +       "\"track_name\": \""+ track_enc +"\","
                +       "\"artist_name\": \""+ artist_enc +"\","
                +       "\"release_name\": \""+ album_enc +"\","
                +       "\"additional_info\": {"
                +           "\"duration_ms\":"+ String.valueOf(length)
                +          "}"
                +       "}"
                +    "}"
                + "]"
                + "}";

            command = "/data/data/gocurl --cacert /data/data/cacert.pem -s " +
                  "-X POST " +
                  "--url '" + apiUrl + "/submit-listens" + "' " +
                  "-H 'Content-Type: application/json' " +
                  "-H 'Authorization: Token " + apiKey + "' " +
                  "-d '" + jsonPayload + "'";
        }
        response = execShell(command);

        // check if we got a valid response
        try {
            JSONObject jsonObject = new JSONObject(response);
            if (jsonObject.has("scrobbles")) {
                return true;
            } else {
                return false;
            }
        } catch (JSONException e) {
            Log.e("RockboxService", "Last.fm: json error: " + e.getMessage());
            return false;
        }
    }

    public static String md5sum(String input){
        try {
            MessageDigest md = MessageDigest.getInstance("MD5");
            md.update(input.getBytes());
            byte[] digest = md.digest();
            StringBuilder output = new StringBuilder();
            for (byte b : digest) {
                String hex = Integer.toHexString(0xff & b);
                if (hex.length() == 1) {
                    output.append('0');
                }
                output.append(hex);
            }
            return output.toString();
        } catch (NoSuchAlgorithmException e) {
            Log.e("RockboxService", "md5sum error: " + e.getMessage());
            return "";
        }
    }

    public static String execShell(String command) {
        StringBuilder output = new StringBuilder();
        StringBuilder errorOutput = new StringBuilder();
        java.lang.Process process = null;
        BufferedReader reader = null;
        BufferedReader errorReader = null;

        try {
            process = Runtime.getRuntime().exec("su -u root -c " + command);

            // stdout
            reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
            String line;
            while ((line = reader.readLine()) != null) {
                output.append(line).append("\n");
            }

            // stderr
            errorReader = new BufferedReader(new InputStreamReader(process.getErrorStream()));
            String errorLine;
            while ((errorLine = errorReader.readLine()) != null) {
                errorOutput.append(errorLine).append("\n");
            }

            process.waitFor();
        } catch (IOException | InterruptedException e) {
            Log.e("RockboxService", "exec shell error: " + e.toString());
            return "Error executing command: " + e.getMessage();
        } finally {
            if (reader != null) {
                try {
                    reader.close();
                } catch (IOException e) {
                    Log.e("RockboxService", "exec shell error: " + e.toString());
                }
            }
            if (errorReader != null) {
                try {
                    errorReader.close();
                } catch (IOException e) {
                    Log.e("RockboxService", "exec shell error: " + e.toString());
                }
            }
            if (process != null) {
                process.destroy();
            }
        }

        // Combine stdout + stderr
        if (errorOutput.length() > 0) {
            return output.toString() + "\nErrors:\n" + errorOutput.toString();
        } else {
            return output.toString();
        }
    }

    private static final String GO_CURL_PATH = "/data/data/gocurl";
    private static final String CA_CERT_PATH = "/data/data/cacert.pem";
    private static final String[] ROCKBOX_DOWNLOAD_BASE_PATHS = {
        "/sdcard/.rockbox",
        "/sdcard/audiobookshelf"
    };
    private static final int GO_CURL_CONNECT_TIMEOUT_SECONDS = 10;
    private static final int GO_CURL_TOTAL_TIMEOUT_SECONDS = 30;
    private static final int STREAM_CAPTURE_LIMIT_BYTES = 1024 * 1024;
    private static final long PROCESS_CLEANUP_GRACE_MS = 1000L;
    private static final long STREAM_JOIN_GRACE_MS = 1000L;
    private static final int EEXIST = 17;
    private static final Charset UTF8 = Charset.forName("UTF-8");

    private static native int atomicLinkExclusive(String src, String dst);

    private static final class ShellResult {
        public int exitCode;
        public boolean timedOut;
        public boolean stdoutTruncated;
        public boolean stderrTruncated;
        public String stdout = "";
        public String stderr = "";
    }

    private static String safeString(String value) {
        return value == null ? "" : value;
    }

    private static String shellQuote(String value) {
        return "'" + safeString(value).replace("'", "'\\''") + "'";
    }

    private static String buildGocurlCommand(String method, String url, String headers, String body) {
        StringBuilder command = new StringBuilder();
        String[] headerLines;

        command.append(shellQuote(GO_CURL_PATH));
        command.append(" --cacert ").append(shellQuote(CA_CERT_PATH));
        command.append(" --connect-timeout ").append(String.valueOf(GO_CURL_CONNECT_TIMEOUT_SECONDS));
        command.append(" --json-output");
        command.append(" -X ").append(shellQuote(method));

        headerLines = safeString(headers).split("\\n");
        for (String headerLine : headerLines) {
            String trimmed = headerLine.trim();
            if (trimmed.length() > 0) {
                command.append(" -H ").append(shellQuote(trimmed));
            }
        }

        if (body != null && body.length() > 0) {
            command.append(" -d ").append(shellQuote(body));
        }

        command.append(" --url ").append(shellQuote(url));
        return command.toString();
    }

    private static String buildGocurlDownloadCommand(String url, String headers, String outputPath) {
        StringBuilder command = new StringBuilder();
        String[] headerLines;

        command.append(shellQuote(GO_CURL_PATH));
        command.append(" --cacert ").append(shellQuote(CA_CERT_PATH));
        command.append(" --connect-timeout ").append(String.valueOf(GO_CURL_CONNECT_TIMEOUT_SECONDS));
        command.append(" -L -sS");

        headerLines = safeString(headers).split("\\n");
        for (String headerLine : headerLines) {
            String trimmed = headerLine.trim();
            if (trimmed.length() > 0) {
                command.append(" -H ").append(shellQuote(trimmed));
            }
        }

        command.append(" -o ").append(shellQuote(outputPath));
        command.append(" -w ").append(shellQuote("%{http_code}"));
        command.append(" --url ").append(shellQuote(url));
        return command.toString();
    }

    private static String[] getCanonicalDownloadBasePaths() throws IOException {
        String[] canonicalBases = new String[ROCKBOX_DOWNLOAD_BASE_PATHS.length];
        int i;

        for (i = 0; i < ROCKBOX_DOWNLOAD_BASE_PATHS.length; i++) {
            canonicalBases[i] = new File(ROCKBOX_DOWNLOAD_BASE_PATHS[i]).getCanonicalPath();
        }

        return canonicalBases;
    }

    private static boolean isSafeDownloadPath(File path) {
        int i;

        try {
            String canonical = path.getCanonicalPath();
            String[] canonicalBases = getCanonicalDownloadBasePaths();

            for (i = 0; i < canonicalBases.length; i++) {
                String canonicalBase = canonicalBases[i];
                if (canonical.equals(canonicalBase) || canonical.startsWith(canonicalBase + "/")) {
                    return true;
                }
            }
        } catch (IOException e) {
            return false;
        }

        return false;
    }

    private static Thread drainStream(final InputStream stream,
                                      final ByteArrayOutputStream output,
                                      final int limit,
                                      final boolean[] truncatedFlag) {
        Thread thread = new Thread(new Runnable() {
            public void run() {
                byte[] buffer = new byte[4096];
                int read;
                int total = 0;

                try {
                    while ((read = stream.read(buffer)) != -1) {
                        if (total < limit) {
                            int remaining = limit - total;
                            int copy = read < remaining ? read : remaining;
                            output.write(buffer, 0, copy);
                            total += copy;
                            if (copy < read) {
                                truncatedFlag[0] = true;
                            }
                        } else {
                            truncatedFlag[0] = true;
                        }
                    }
                } catch (IOException e) {
                    truncatedFlag[0] = true;
                } finally {
                    try {
                        stream.close();
                    } catch (IOException e) {
                    }
                }
            }
        }, "rockbox-stream-drain");
        thread.start();
        return thread;
    }

    private static void closeQuietly(Closeable closeable) {
        if (closeable == null) {
            return;
        }

        try {
            closeable.close();
        } catch (IOException e) {
        }
    }

    private static boolean joinQuietly(Thread thread, long timeoutMs) {
        if (thread == null) {
            return true;
        }

        try {
            thread.join(timeoutMs);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }

        return !thread.isAlive();
    }

    private static void closeProcessStreams(Process process) {
        if (process == null) {
            return;
        }

        closeQuietly(process.getInputStream());
        closeQuietly(process.getErrorStream());
        closeQuietly(process.getOutputStream());
    }

    private static void destroyProcessQuietly(Process process) {
        if (process == null) {
            return;
        }

        try {
            process.destroy();
        } catch (Exception e) {
        }
    }

    private static void deleteQuietly(File file) {
        if (file == null) {
            return;
        }

        try {
            if (file.exists()) {
                file.delete();
            }
        } catch (Exception e) {
        }
    }

    private static String ensureDownloadParentDirectory(File parentDir) {
        if (parentDir == null) {
            return "filesystem error: destination parent directory is missing";
        }
        if (parentDir.exists()) {
            if (!parentDir.isDirectory()) {
                return "filesystem error: destination parent is not a directory";
            }
        } else if (!parentDir.mkdirs() && !parentDir.isDirectory()) {
            return "filesystem error: failed to create destination directory";
        }

        if (!parentDir.canWrite()) {
            return "filesystem error: destination directory is not writable";
        }

        return "";
    }

    private static File createDownloadTempFile(File destinationFile) throws IOException {
        String prefix = ".download-" + destinationFile.getName();

        while (prefix.length() < 3) {
            prefix += "_";
        }
        if (prefix.length() > 32) {
            prefix = prefix.substring(0, 32);
        }

        return File.createTempFile(prefix, ".part", destinationFile.getParentFile());
    }

    private static String moveCompletedDownloadIntoPlace(File tempFile,
                                                         File canonicalDestination) {
        int linkRc;

        if (tempFile == null || !tempFile.exists()) {
            return "filesystem error: temporary download file missing after helper exit";
        }

        linkRc = atomicLinkExclusive(tempFile.getAbsolutePath(),
                                     canonicalDestination.getAbsolutePath());
        deleteQuietly(tempFile);

        if (linkRc == 0) {
            return "";
        }
        if (linkRc == EEXIST) {
            return "filesystem error: destination already exists";
        }
        return "filesystem error: failed to move completed download into place";
    }

    private static String summarizeHelperFailure(String detail, String fallback) {
        String normalized = safeString(detail).trim().toLowerCase();

        if (normalized.length() == 0) {
            return fallback;
        }
        if (normalized.contains("timed out") || normalized.contains("timeout")) {
            return "request timed out";
        }
        if (normalized.contains("certificate") || normalized.contains("x509") || normalized.contains("tls") || normalized.contains("ssl")) {
            return "tls/certificate validation failed";
        }
        if (normalized.contains("no such host") || normalized.contains("unknown host") || normalized.contains("name resolution") || normalized.contains("resolve")) {
            return "dns resolution failed";
        }
        if (normalized.contains("connection refused")) {
            return "connection refused";
        }
        if (normalized.contains("network is unreachable") || normalized.contains("no route to host")) {
            return "network unreachable";
        }
        if (normalized.contains("connection reset") || normalized.contains("broken pipe") || normalized.contains("unexpected eof")) {
            return "connection interrupted";
        }
        if (normalized.contains("malformed") || normalized.contains("invalid url") || normalized.contains("unsupported protocol") || normalized.contains("parse")) {
            return "invalid request parameters";
        }

        return fallback;
    }

    private static ShellResult execShellForRequest(String command, int timeoutSeconds) {
        ShellResult result = new ShellResult();
        Process process = null;
        ByteArrayOutputStream stdout = new ByteArrayOutputStream();
        ByteArrayOutputStream stderr = new ByteArrayOutputStream();
        boolean[] stdoutTruncated = new boolean[]{false};
        boolean[] stderrTruncated = new boolean[]{false};
        Thread stdoutThread = null;
        Thread stderrThread = null;
        long deadlineMs = System.currentTimeMillis() + (timeoutSeconds * 1000L);

        try {
            process = new ProcessBuilder("su", "-u", "root", "-c", command).start();
            stdoutThread = drainStream(process.getInputStream(), stdout,
                                       STREAM_CAPTURE_LIMIT_BYTES, stdoutTruncated);
            stderrThread = drainStream(process.getErrorStream(), stderr,
                                       STREAM_CAPTURE_LIMIT_BYTES, stderrTruncated);

            while (true) {
                try {
                    result.exitCode = process.exitValue();
                    break;
                } catch (IllegalThreadStateException e) {
                    if (System.currentTimeMillis() >= deadlineMs) {
                        result.timedOut = true;
                        destroyProcessQuietly(process);
                        closeProcessStreams(process);
                        break;
                    }
                    try {
                        Thread.sleep(100);
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        result.timedOut = true;
                        destroyProcessQuietly(process);
                        closeProcessStreams(process);
                        break;
                    }
                }
            }
        } catch (IOException e) {
            result.exitCode = -1;
            result.stderr = "unable to start helper process";
            return result;
        } finally {
            long joinTimeoutMs = STREAM_JOIN_GRACE_MS;

            if (result.timedOut) {
                joinTimeoutMs = PROCESS_CLEANUP_GRACE_MS;
            }

            if (!joinQuietly(stdoutThread, joinTimeoutMs)) {
                stdoutTruncated[0] = true;
            }
            if (!joinQuietly(stderrThread, joinTimeoutMs)) {
                stderrTruncated[0] = true;
            }

            closeProcessStreams(process);
            destroyProcessQuietly(process);
        }

        result.stdout = new String(stdout.toByteArray(), UTF8);
        result.stderr = new String(stderr.toByteArray(), UTF8);
        result.stdoutTruncated = stdoutTruncated[0];
        result.stderrTruncated = stderrTruncated[0];
        return result;
    }

    public static String[] performSynchronousRequest(String method, String url, String headers, String body) {
        String safeMethod = safeString(method).trim();
        String safeUrl = safeString(url).trim();
        String safeHeaders = safeString(headers);
        String safeBody = safeString(body);
        String command;
        ShellResult shellResult;
        JSONObject jsonObject;
        String errorText = "";
        String responseBody = "";
        int status = 0;

        if (safeMethod.length() == 0) {
            return new String[]{"0", "", "invalid parameter: method is required"};
        }

        if (safeUrl.length() == 0) {
            return new String[]{"0", "", "invalid parameter: url is required"};
        }

        command = buildGocurlCommand(safeMethod, safeUrl, safeHeaders, safeBody);
        shellResult = execShellForRequest(command, GO_CURL_TOTAL_TIMEOUT_SECONDS);

        if (shellResult.timedOut) {
            return new String[]{"0", "", "gocurl request timed out after " + String.valueOf(GO_CURL_TOTAL_TIMEOUT_SECONDS) + " seconds"};
        }

        if (shellResult.stdoutTruncated || shellResult.stderrTruncated) {
            errorText = "gocurl response exceeded Java capture limit";
            if (shellResult.stderrTruncated) {
                errorText += " (stderr truncated)";
            }
            if (shellResult.stdoutTruncated) {
                errorText += shellResult.stderrTruncated ? " and stdout truncated" : " (stdout truncated)";
            }
            return new String[]{"0", "", errorText};
        }

        if (shellResult.stdout.trim().length() == 0) {
            errorText = "gocurl produced no JSON output";
            if (shellResult.exitCode != 0) {
                errorText += " (" + summarizeHelperFailure(shellResult.stderr, "helper process failed") + ")";
            }
            return new String[]{"0", "", errorText};
        }

        try {
            jsonObject = new JSONObject(shellResult.stdout);
        } catch (JSONException e) {
            errorText = "gocurl returned malformed JSON";
            if (shellResult.exitCode != 0) {
                errorText += " (" + summarizeHelperFailure(shellResult.stderr, "helper process failed") + ")";
            }
            return new String[]{"0", "", errorText};
        }

        if (jsonObject.has("error")) {
            errorText = "gocurl request failed: "
                + summarizeHelperFailure(jsonObject.optString("error"), "request failed");
            if (shellResult.exitCode != 0) {
                errorText += " (exit " + String.valueOf(shellResult.exitCode) + ")";
            }
            return new String[]{"0", "", errorText};
        }

        status = jsonObject.optInt("status_code", 0);
        responseBody = safeString(jsonObject.optString("body_base64", ""));
        if (responseBody.length() > 0) {
            try {
                responseBody = new String(Base64.decode(responseBody, Base64.DEFAULT), UTF8);
            } catch (IllegalArgumentException e) {
                return new String[]{"0", "", "gocurl returned invalid response body encoding"};
            }
        } else {
            responseBody = "";
        }

        if (shellResult.exitCode != 0) {
            errorText = "gocurl exited with code " + String.valueOf(shellResult.exitCode);
        }

        return new String[]{String.valueOf(status), responseBody, errorText};
    }

    public static String[] performSynchronousDownload(String url, String headers, String destinationPath) {
        String safeUrl = safeString(url).trim();
        String safeHeaders = safeString(headers);
        String safeDestination = safeString(destinationPath).trim();
        File destinationFile;
        File canonicalDestination;
        File parentDir;
        File tempFile = null;
        String command;
        ShellResult shellResult;
        String statusText;
        int status = 0;
        String errorText = "";
        String[] canonicalBases;

        if (safeUrl.length() == 0) {
            return new String[]{"0", "invalid parameter: url is required"};
        }

        if (safeDestination.length() == 0) {
            return new String[]{"0", "invalid parameter: destination path is required"};
        }

        try {
            destinationFile = new File(safeDestination);
            canonicalDestination = destinationFile.getCanonicalFile();
            canonicalBases = getCanonicalDownloadBasePaths();
        } catch (IOException e) {
            return new String[]{"0", "invalid destination path"};
        }

        if (!isSafeDownloadPath(canonicalDestination)) {
            return new String[]{"0", "invalid destination path: must stay under /sdcard/.rockbox/ or /sdcard/audiobookshelf/"};
        }

        if (canonicalDestination.isDirectory() ||
            canonicalDestination.getPath().equals(canonicalBases[0]) ||
            canonicalDestination.getPath().equals(canonicalBases[1])) {
            return new String[]{"0", "invalid destination path: destination must be a file path"};
        }

        if (canonicalDestination.exists()) {
            return new String[]{"0", "filesystem error: destination already exists"};
        }

        parentDir = canonicalDestination.getParentFile();
        if (parentDir == null || !isSafeDownloadPath(parentDir)) {
            return new String[]{"0", "invalid destination path: parent directory is not allowed"};
        }

        errorText = ensureDownloadParentDirectory(parentDir);
        if (errorText.length() > 0) {
            return new String[]{"0", errorText};
        }

        try {
            tempFile = createDownloadTempFile(canonicalDestination);
        } catch (IOException e) {
            return new String[]{"0", "filesystem error: failed to create temporary download file"};
        }

        command = buildGocurlDownloadCommand(safeUrl, safeHeaders,
                                             tempFile.getAbsolutePath());
        shellResult = execShellForRequest(command, GO_CURL_TOTAL_TIMEOUT_SECONDS);

        if (shellResult.timedOut) {
            deleteQuietly(tempFile);
            return new String[]{"0", "gocurl download timed out after " + String.valueOf(GO_CURL_TOTAL_TIMEOUT_SECONDS) + " seconds"};
        }

        if (shellResult.stdoutTruncated || shellResult.stderrTruncated) {
            deleteQuietly(tempFile);
            return new String[]{"0", "gocurl download status exceeded Java capture limit"};
        }

        statusText = safeString(shellResult.stdout).trim();
        if (statusText.length() > 0) {
            try {
                status = Integer.parseInt(statusText);
            } catch (NumberFormatException e) {
                deleteQuietly(tempFile);
                return new String[]{"0", "gocurl returned invalid HTTP status"};
            }
        }

        if (shellResult.exitCode != 0) {
            deleteQuietly(tempFile);
            errorText = "gocurl download failed: " + summarizeHelperFailure(shellResult.stderr, "helper process failed");
            errorText += " (exit " + String.valueOf(shellResult.exitCode) + ")";
            return new String[]{String.valueOf(status), errorText};
        }

        if (status < 200 || status >= 300) {
            deleteQuietly(tempFile);
            return new String[]{String.valueOf(status), "download not saved: HTTP status " + String.valueOf(status)};
        }

        errorText = moveCompletedDownloadIntoPlace(tempFile, canonicalDestination);
        if (errorText.length() > 0) {
            return new String[]{String.valueOf(status), errorText};
        }

        return new String[]{String.valueOf(status), ""};
    }

    public static boolean downloadPodcast(String podcastUrl, String outputPath, int episode) {
        String command = "/data/data/poddl -ca-cert /data/data/cacert.pem"+
                                         " -write-tag"+ 
                                         " -add-episode-to-title"+
                                         " -add-episode-to-filename"+
                                         " -dest \""+ outputPath + "\"" +
                                         " -num " + String.valueOf(episode) + 
                                         " " + podcastUrl;
        String ret;
        ret = execShell(command);
        if (ret.contains("Failed reading")){
            return false;
        } else {
            return true;
        }
    }

    public static String getPodcastNames(ArrayList<String[]> podcasts) {
        String ret = "";
        for (int i = 0; i < podcasts.size(); i++) {
            ret = ret + podcasts.get(i)[0] + "\n";
        }
        return ret;
    }

    public static String getPodcastUrls(ArrayList<String[]> podcasts){
        String ret = "";
        for (int i = 0; i < podcasts.size(); i++) {
            ret = ret + podcasts.get(i)[1] + "\n";
        }
        return ret;
    }

    public static void startPodcastDownload(ArrayList<String[]> podcasts, int podcastNum, int episode, String podcastFolder) {
        String podcastUrl=podcasts.get(podcastNum)[1];
        String outputPath=podcastFolder;
        if (outputPath.endsWith("/")){
            outputPath = outputPath + podcasts.get(podcastNum)[0];
        } else {
            outputPath = outputPath + "/" + podcasts.get(podcastNum)[0];
        }
        boolean ret;
        String ret_str;

        ret = downloadPodcast(podcastUrl, outputPath, episode);
    }

    public static int getNewEpisodes(String podcastUrl, String podcastFolder) {
        String outputPath = podcastFolder;
        /* currently not using this method but we would need to either pass podcastNum or calculate it
        if (outputPath.endsWith("/")){
            outputPath = outputPath + podcasts.get(podcastNum)[0];
        } else {
            outputPath = outputPath + "/" + podcasts.get(podcastNum)[0];
        }
        */
        String command="/data/data/poddl " + "-dest \"" + outputPath + "\" -ca-cert /data/data/cacert.pem -check " + podcastUrl;
        String ret;
        int newEpisodes;

        ret = execShell(command);
        try {
            newEpisodes = Integer.parseInt(ret.trim());
        } catch (NumberFormatException e) {
            newEpisodes = -1;
            Log.d("RockboxService", "getNewEpisodes: " + e.getMessage());
        }

        return newEpisodes;
    }

    public static String getEpisodeList(ArrayList<String[]> podcasts, int podcastNum, String podcastFolder) {
        String podcastUrl=podcasts.get(podcastNum)[1];
        String outputPath=podcastFolder;
        if (outputPath.endsWith("/")){
            outputPath = outputPath + podcasts.get(podcastNum)[0];
        } else {
            outputPath = outputPath + "/" + podcasts.get(podcastNum)[0];
        }
        String command="/data/data/poddl " + "-dest \"" + outputPath + "\" -ca-cert /data/data/cacert.pem -get-episodes " + podcastUrl;
        String ret;
        ret = execShell(command);

        if (ret != null) {
            return ret;
        } else {
            return "ERROR";
        }
    }

    public static String getEpisodePath(ArrayList<String[]> podcasts, int podcastNum, int num, String podcastFolder) {
        String podcastUrl=podcasts.get(podcastNum)[1];
        String outputPath=podcastFolder;
        if (outputPath.endsWith("/")){
            outputPath = outputPath + podcasts.get(podcastNum)[0];
        } else {
            outputPath = outputPath + "/" + podcasts.get(podcastNum)[0];
        }
        String command = "/data/data/poddl -ca-cert /data/data/cacert.pem"+
                                " -write-tag"+ 
                                " -add-episode-to-title"+
                                " -add-episode-to-filename"+
                                " -dest \""+ outputPath + "\"" +
                                " -num " + String.valueOf(num) + 
                                " -get-location" +
                                " " + podcastUrl;

        String ret;
        ret = execShell(command);

        if (ret != null) {
            return ret;
        } else {
            return "ERROR";
        }
    }

    public static void deleteEpisode(ArrayList<String[]> podcasts, int podcastNum, int episode, String podcastFolder) {
        String podcastUrl=podcasts.get(podcastNum)[1];
        String outputPath=podcastFolder;
        if (outputPath.endsWith("/")){
            outputPath = outputPath + podcasts.get(podcastNum)[0];
        } else {
            outputPath = outputPath + "/" + podcasts.get(podcastNum)[0];
        }

        String command = "/data/data/poddl -ca-cert /data/data/cacert.pem"+
                                        " -write-tag"+ 
                                        " -add-episode-to-title"+
                                        " -add-episode-to-filename"+
                                        " -dest \""+ outputPath + "\"" +
                                        " -delete " + String.valueOf(episode) + 
                                        " " + podcastUrl;
        String ret;
        ret = execShell(command);
    }
    
    public static String connectWifi(String ssid, String password) {
        int i;
        int threshold;

        if (context == null) {
            Log.d("RockboxService", "wifi error: context = null");
            return "Failed";
        }
        
        if (ssid == null || ssid.isEmpty()) {
            Log.d("RockboxService", "wifi error: no ssid configured");
            return "Not configured";
        }
        
        try {
            WifiManager wifiManager = (WifiManager) context.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            
            if (wifiManager == null) {
                Log.d("RockboxService", "wifi error: wifiManager = null");
                return "Failed";
            }
            
            // enable wifi
            if (!wifiManager.isWifiEnabled()) {
                wifiManager.setWifiEnabled(true);
            }
            
            List<WifiConfiguration> configuredNetworks = wifiManager.getConfiguredNetworks();
            int networkId = -1;
            
            if (configuredNetworks != null) {
                for (WifiConfiguration config : configuredNetworks) {
                    if (config.SSID != null && config.SSID.equals("\"" + ssid + "\"")) {
                        networkId = config.networkId;
                        break;
                    }
                }
            }
            
            // If network is not configured, add it
            if (networkId == -1) {
                WifiConfiguration config = new WifiConfiguration();
                config.SSID = "\"" + ssid + "\"";
                config.preSharedKey = "\"" + password + "\"";
                config.allowedKeyManagement.set(WifiConfiguration.KeyMgmt.WPA_PSK);
                config.allowedProtocols.set(WifiConfiguration.Protocol.RSN);
                config.allowedProtocols.set(WifiConfiguration.Protocol.WPA);
                config.allowedPairwiseCiphers.set(WifiConfiguration.PairwiseCipher.CCMP);
                config.allowedPairwiseCiphers.set(WifiConfiguration.PairwiseCipher.TKIP);
                config.allowedGroupCiphers.set(WifiConfiguration.GroupCipher.CCMP);
                config.allowedGroupCiphers.set(WifiConfiguration.GroupCipher.TKIP);
                
                // try to add network for up to 10s, the wifi stack takes some time to get ready
                threshold = 100;
                i = 1;
                while (networkId == -1 && i < threshold){
                    networkId = wifiManager.addNetwork(config);
                    try {
                        Thread.sleep(100);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                    }
                    i++;
                }
                
                if (networkId == -1) {
                    Log.d("RockboxService", "wifi error: networkid = -1");
                    return "Failed";
                }
            }
            
            wifiManager.enableNetwork(networkId, true);
            wifiManager.saveConfiguration();
            
            wifiManager.reconnect();
            Log.d("RockboxService", "wifi connect: success");

            i = 1;
            threshold = 50;
            while (i < threshold){
                try {
                    if (isInternetAvailable()){
                        Log.d("RockboxService", "internet connection: success");
                        return "Success";
                    }
                    Thread.sleep(100);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                i++;
            }
            Log.e("RockboxService", "Failed to connect to internet");
            return "Failed";
        } catch (Exception e) {
            Log.e("RockboxService", "Failed to connect to wifi: " + e.getMessage());
            return "Failed";
        }
    }

    private static boolean isInternetAvailable() {
        try {
            Process p = Runtime.getRuntime().exec("ping -c 1 1.1.1.1");
            int returnVal = p.waitFor();
            return (returnVal == 0);
        } catch (Exception e) {
            return false;
        }
    }
    
    public static String disconnectWifi() {
        if (context == null) {
            Log.d("RockboxService", "wifi error, disconnect: context = null");
            return "Failed";
        }
        
        try {
            WifiManager wifiManager = (WifiManager) context.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            
            if (wifiManager == null) {
                Log.d("RockboxService", "wifi error, disconnect: wifiManager = null");
                return "Failed";
            }
            
            // disconnect network, deactivate wifi
            wifiManager.disconnect();
            wifiManager.setWifiEnabled(false);

            Log.d("RockboxService", "wifi: disconnect success");
            return "Success";
        } catch (Exception e) {
            Log.e("RockboxService", "Failed to disconnect from WiFi: " + e.getMessage());
            return "Failed";
        }
    }
}
