package org.rockbox.Helper;

import android.util.Log;

import java.io.BufferedReader;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.InputStreamReader;
import java.io.IOException;
import java.io.UnsupportedEncodingException;
import java.net.URLEncoder;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import org.json.JSONObject;
import org.json.JSONException;

public class Connectivity{
    public static boolean lastFm(String username, String password, String apiKey, String sharedSecret, String artist, String track, String album, int timestamp){
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

        // auth
        String method = "auth.getMobileSession";
        String signBase = "api_key" + apiKey +
                    "method" + method + 
                    "password" + password +
                    "username" + username + sharedSecret;
        String apiSig = md5sum(signBase);

        String command = "/data/data/gocurl --cacert /data/data/cacert.pem -s " +
                            "-X POST " +
                            "--url 'https://ws.audioscrobbler.com/2.0/' " +
                            "-d " +
                            "'method="+method+
                            "&api_key="+apiKey+
                            "&username="+username_enc+
                            "&password="+password_enc+
                            "&api_sig="+apiSig+
                            "&format=json'";
        String response = execShell(command);

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
                    "--url 'https://ws.audioscrobbler.com/2.0/' " +
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
}