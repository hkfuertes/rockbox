/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2010 Thomas Martitz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

package org.rockbox;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.rockbox.Helper.Logger;
import org.rockbox.Helper.MediaButtonReceiver;
import org.rockbox.Helper.RunForegroundManager;
import org.rockbox.Helper.BrightnessController;
import org.rockbox.Helper.ScreenTimeoutController;
import org.rockbox.Helper.ExternalAppsManager;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.Service;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.os.IBinder;
import android.os.ResultReceiver;
import android.os.PowerManager;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.util.Log;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.text.Html;

/* This class is used as the main glue between java and c.
 * All access should be done through RockboxService.get_instance() for safety.
 */

public class RockboxService extends Service
{
    /* this Service is really a singleton class - well almost. */
    private static RockboxService instance = null;

    /* locals needed for the c code and Rockbox state */
    private static volatile boolean rockbox_running;
    private Activity mCurrentActivity = null;
    private RunForegroundManager mFgRunner;
    private MediaButtonReceiver mMediaButtonReceiver;
    private ResultReceiver mResultReceiver;
    
    /* Regular checks */
    private long mLastRestartTime = 0; // Timestamp of last restart attempt
    private static final long RESTART_COOLDOWN_MS = 500; // Minimum 1 seconds between restarts
    private PowerManager pm;
    /* possible result values for intent handling */ 
    public static final int RESULT_INVOKING_MAIN = 0;
    public static final int RESULT_LIB_LOAD_PROGRESS = 1;
    public static final int RESULT_SERVICE_RUNNING = 3;
    public static final int RESULT_ERROR_OCCURED = 4;
    public static final int RESULT_LIB_LOADED = 5;
    public static final int RESULT_ROCKBOX_EXIT = 6;

    @Override
    public void onCreate()
    {
        instance = this;
        pm = (PowerManager) this.getSystemService(Context.POWER_SERVICE);
        mMediaButtonReceiver = new MediaButtonReceiver(this);
        mFgRunner = new RunForegroundManager(this);
    }

    public static RockboxService getInstance()
    {
        /* don't call the constructor here, the instances are managed by
         * android, so we can't just create a new one */
        return instance;
    }

    public boolean isRockboxRunning()
    {
        return rockbox_running;
    }
    public Activity getActivity()
    {
        return mCurrentActivity;
    }

    public void setActivity(Activity a)
    {
        mCurrentActivity = a;
    }
    
    private void putResult(int resultCode)
    {
        putResult(resultCode, null);
    }

    private void putResult(int resultCode, Bundle resultData)
    {
        if (mResultReceiver != null)
            mResultReceiver.send(resultCode, resultData);
    }

    private void doStart(Intent intent)
    {
        Logger.d("Start RockboxService (Intent: " + intent.getAction() + ")");

        if (intent.getAction().equals("org.rockbox.ResendTrackUpdateInfo"))
        {
            if (rockbox_running)
                mFgRunner.resendUpdateNotification();
            return;
        }

        if (intent.hasExtra("callback"))
            mResultReceiver = (ResultReceiver) intent.getParcelableExtra("callback");

        if (!rockbox_running)
            startService();

        if (intent.getAction().equals(Intent.ACTION_MEDIA_BUTTON))
        {
            KeyEvent kev = intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT);
            /* Only handle non-repeat events in RockboxService */
            /* Repeat events are handled by MediaButtonReceiver */
            if (kev.getRepeatCount() == 0)
            {
                /* Normal press/release event */
                RockboxFramebuffer.buttonHandler(kev.getKeyCode(),
                                    kev.getAction() == KeyEvent.ACTION_DOWN);
            }
        }

        /* (Re-)attach the media button receiver, in case it has been lost */
        mMediaButtonReceiver.register();
        putResult(RESULT_SERVICE_RUNNING);

        rockbox_running = true;
    }

    public void onStart(Intent intent, int startId) {
        doStart(intent);
    }

    public int onStartCommand(Intent intent, int flags, int startId)
    {
        /* if null, then the service was most likely restarted by android
         * after getting killed for memory pressure earlier */
        if (intent == null)
            intent = new Intent("org.rockbox.ServiceRestarted");
        doStart(intent);
        return START_STICKY;
    }

    private void startService()
    {
        while (!Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState())) {
            try {
                Log.d("RockboxService", "SD not ready yet, postponing start.");
                Thread.sleep(100); // wait a little bit before checkign again if the SD is mounted
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        }
        final Object lock = new Object();
        Thread rb = new Thread(new Runnable()
        {
            public void run()
            {
                final int BUFFER = 8*1024;
                String rockboxDirPath = "/data/data/org.rockbox/app_rockbox/rockbox";
                String rockboxCreditsPath = "/data/data/org.rockbox/app_rockbox/rockbox/rocks/viewers";
                String rockboxSdDirPath = "/sdcard/.rockbox";

                /* the following block unzips libmisc.so, which contains the files 
                 * we ship, such as themes. It's needed to put it into a .so file
                 * because there's no other way to ship files and have access
                 * to them from native code
                 */
                File libMisc = new File("/data/data/org.rockbox/lib/libmisc.so");
                /* use arbitrary file to determine whether extracting is needed */
                File arbitraryFile = new File(rockboxCreditsPath, "credits.rock");
                File rockboxInfoFile = new File(rockboxSdDirPath, "rockbox-info.txt");
                /* unzip newer or doesnt exist */
                boolean doExtract = !arbitraryFile.exists()
                        || (libMisc.lastModified() > arbitraryFile.lastModified());

                /* load library before unzipping which may take a while
                 * but at least tell if unzipping is going to be done before*/
                synchronized (lock) {
                    Bundle bdata = new Bundle();
                    bdata.putBoolean("unzip", doExtract);
                    System.loadLibrary("rockbox");
                    putResult(RESULT_LIB_LOADED, bdata);
                    lock.notify();
                }

                if (doExtract)
                {
                    boolean extractToSd = false;
                    if(rockboxInfoFile.exists()) {
                        extractToSd = true;
                        Logger.d("extracting resources to SD card");
                    }
                    else {
                        Logger.d("extracting resources to internal memory");
                    }
                    try
                    {
                        Bundle progressData = new Bundle();
                        byte data[] = new byte[BUFFER];
                        ZipFile zipfile = new ZipFile(libMisc);
                        Enumeration<? extends ZipEntry> e = zipfile.entries();
                        progressData.putInt("max", zipfile.size());

                        while(e.hasMoreElements())
                        {
                           ZipEntry entry = (ZipEntry) e.nextElement();
                           File file;
                           /* strip off /.rockbox when extracting */
                           String fileName = entry.getName();
                           int slashIndex = fileName.indexOf('/', 1);
                           /* codecs are now stored as libs, only keep rocks on internal */
                           if(extractToSd == false
                               || fileName.substring(slashIndex).startsWith("/rocks"))
                           {
                               file = new File(rockboxDirPath + fileName.substring(slashIndex));
                           }
                           else
                           {
                               file = new File(rockboxSdDirPath + fileName.substring(slashIndex));
                           }

                           if (!entry.isDirectory())
                           {
                               /* Create the parent folders if necessary */
                               File folder = new File(file.getParent());
                               if (!folder.exists())
                                   folder.mkdirs();

                               /* Extract file */
                               BufferedInputStream is = new BufferedInputStream(zipfile.getInputStream(entry), BUFFER);
                               FileOutputStream fos = new FileOutputStream(file);
                               BufferedOutputStream dest = new BufferedOutputStream(fos, BUFFER);

                               int count;
                               while ((count = is.read(data, 0, BUFFER)) != -1)
                                  dest.write(data, 0, count);

                               dest.flush();
                               dest.close();
                               is.close();
                           }

                           progressData.putInt("value", progressData.getInt("value", 0) + 1);
                           putResult(RESULT_LIB_LOAD_PROGRESS, progressData);
                        }
                        arbitraryFile.setLastModified(libMisc.lastModified());
                    } catch(Exception e) {
                        Logger.d("Exception when unzipping", e);
                        Bundle bundle = new Bundle();
                        e.printStackTrace();
                        bundle.putString("error", getString(R.string.error_extraction));
                        putResult(RESULT_ERROR_OCCURED, bundle);
                    }
                }

                /* Generate default config if none exists yet */
                File rockboxConfig = new File(Environment.getExternalStorageDirectory(), ".rockbox/config.cfg");
                if (!rockboxConfig.exists()) {
                    File rbDir = new File(rockboxConfig.getParent());
                    if (!rbDir.exists())
                        rbDir.mkdirs();

                    OutputStreamWriter strm;
                    try {
                        strm = new OutputStreamWriter(new FileOutputStream(rockboxConfig));
                        strm.write("# config generated by RockboxService\n");
                        strm.write("start directory: " + Environment.getExternalStorageDirectory().getAbsolutePath() + "/" + "\n");
                        strm.write("lang: /sdcard/.rockbox/langs/" + getString(R.string.rockbox_language_file) + "\n");
                        strm.write("wheel vibration intensity: 15\n");
                        strm.write("idle poweroff: 0\n");
                        strm.write("font: /.rockbox/fonts/24-Terminus-Bold.fnt\n");
                        strm.write("timestretch enabled: on\n");
                        strm.write("Timestretch mode: on\n");
                        strm.write("volume adjustment mode: perceptual\n");
                        strm.close();
                    } catch(Exception e) {
                        Logger.d("Exception when writing default config", e);
                    }
                }

                /* Start native code */
                putResult(RESULT_INVOKING_MAIN);

                main();

                putResult(RESULT_ROCKBOX_EXIT);

                Logger.d("Stop service: main() returned");
                stopSelf(); /* service is of no use anymore */
            }
        }, "Rockbox thread");
        rb.setDaemon(false);
        /* wait at least until the library is loaded */
        synchronized (lock)
        {
            rb.start();
            while(true)
            {
                try {
                    lock.wait();
                } catch (InterruptedException e) {
                    continue;
                }
                break;
            }
        }
    }

    private native void main();

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }

    void startForeground()
    {
        mFgRunner.startForeground();
    }

    void stopForeground()
    {
        mFgRunner.stopForeground();
    }

    @Override
    public void onDestroy()
    {
        super.onDestroy();
        /* Don't unregister so we can receive them (and startup the service)
         * after idle power-off. Hopefully it's OK if mMediaButtonReceiver is
         * garbage collected.
         *  mMediaButtonReceiver.unregister(); */
        mMediaButtonReceiver = null;
        /* Make sure our notification is gone. */
        stopForeground();
        
        instance = null;
        rockbox_running = false;
        System.runFinalization();
        /* exit() seems unclean but is needed in order to get the .so file garbage 
         * collected, otherwise Android caches this Service and librockbox.so
         * The library must be reloaded to zero the bss and reset data
         * segment */
        System.exit(0);
    }

    /* Android brightness control methods for JNI interface */
    private BrightnessController brightnessController = null;
    private ScreenTimeoutController screenTimeoutController = null;

    /**
     * Set Android brightness to a specific percentage
     * Called from native code via JNI
     */
    public int setAndroidBrightnessPercent(int percent)
    {
        if (brightnessController == null) {
            brightnessController = new BrightnessController();
        }
        return brightnessController.setBrightnessPercent(percent);
    }

    /**
     * Tell RockboxFramebuffer we are not in root menu
     * Called from native code via JNI
     */
    public int isNotRoot(boolean notRoot)
    {
        Log.d("RockboxButton", "Service - isNotRoot: " + notRoot);
        if (notRoot) {
            //Log.d("RockboxButton", "We are in root menu (RockboxService)"); 
            RockboxFramebuffer.isNotRoot = true;
        } else {
            //Log.d("RockboxButton", "We are NOT in root menu (RockboxService)"); 
            RockboxFramebuffer.isNotRoot = false;
        }
        return 1;
    }

    /**
     * Get current Android brightness as percentage
     * Called from native code via JNI
     * @return Current brightness percentage (0-100)
     */
    public int getAndroidBrightnessPercent()
    {
        if (brightnessController == null) {
            brightnessController = new BrightnessController();
        }
        return brightnessController.getBrightnessPercent();
    }

    /* Android screen timeout control methods for JNI interface */
    /**
     * Set Android screen timeout to a specific value in seconds
     * Called from native code via JNI
     */
    public int setAndroidScreenTimeout(int timeoutSeconds)
    {
        if (screenTimeoutController == null) {
            screenTimeoutController = new ScreenTimeoutController();
        }
        return screenTimeoutController.setScreenTimeout(timeoutSeconds);
    }

    /**
     * Get current Android screen timeout in seconds
     * Called from native code via JNI
     * @return Current screen timeout in seconds (0=never, -1=system default)
     */
    public int getAndroidScreenTimeout()
    {
        if (screenTimeoutController == null) {
            screenTimeoutController = new ScreenTimeoutController();
        }
        return screenTimeoutController.getScreenTimeout();
    }

    /* Android external apps methods for JNI interface */
    private ExternalAppsManager externalAppsManager = null;

    /**
     * Get the number of installed applications
     */
    public int getExternalAppsCount()
    {
        if (externalAppsManager == null) {
            externalAppsManager = new ExternalAppsManager(this);
        }
        return externalAppsManager.getAppCount();
    }

    /**
     * Get app name by index
     */
    public String getExternalAppName(int index)
    {
        if (externalAppsManager == null) {
            externalAppsManager = new ExternalAppsManager(this);
        }

        try {
            java.util.List<ExternalAppsManager.AppInfo> apps = externalAppsManager.getInstalledApps();
            if (index >= 0 && index < apps.size()) {
                return apps.get(index).appName;
            }
        } catch (Exception e) {
            Logger.d("Error getting app name at index: " + index, e);
        }
        return null;
    }

    /**
     * Get app package name by index
     */
    public String getExternalAppPackageName(int index)
    {
        if (externalAppsManager == null) {
            externalAppsManager = new ExternalAppsManager(this);
        }

        try {
            java.util.List<ExternalAppsManager.AppInfo> apps = externalAppsManager.getInstalledApps();
            if (index >= 0 && index < apps.size()) {
                return apps.get(index).packageName;
            }
        } catch (Exception e) {
            Logger.d("Error getting app package name at index: " + index, e);
        }
        return null;
    }

    /**
     * Launch an application by index
     */
    public boolean launchExternalApp(int index)
    {
        if (externalAppsManager == null) {
            externalAppsManager = new ExternalAppsManager(this);
        }

        try {
            java.util.List<ExternalAppsManager.AppInfo> apps = externalAppsManager.getInstalledApps();
            if (index >= 0 && index < apps.size()) {
                return externalAppsManager.launchApp(apps.get(index).packageName);
            }
        } catch (Exception e) {
            Logger.d("Error launching app at index: " + index, e);
        }
        return false;
    }

    public void shutdownDevice() {
        final Activity activity = getActivity();
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                new AlertDialog.Builder(activity)
                    .setTitle("Shutdown Device")
                    .setMessage("Are you sure you want to shut down the device?")
                    .setPositiveButton("Yes", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface dialog, int which) {
                            // Run shutdown in background thread
                            new Thread(new Runnable() {
                                public void run() {
                                    try {
                                        Log.d("RockboxService", "Attempting device shutdown...");
                                        java.lang.Process proc = Runtime.getRuntime().exec(new String[]{"su", "-c", "reboot -p"});
                                        proc.waitFor();
                                    } catch (Exception e) {
                                        Log.e("RockboxService", "Failed to shutdown device: " + e.getMessage());
                                        e.printStackTrace();
                                    }
                                }
                            }).start();
                        }
                    })
                    .setNegativeButton("No", null)
                    .show();
            }
        });
    }

    public void switchFirmware() {
        final Activity activity = getActivity();
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                new AlertDialog.Builder(activity)
                    .setMessage(Html.fromHtml("<br><b>Are you sure you want to switch</b>" +
                                "<br>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>to the stock firmware?</b>" +
                                "<br>" +
                                "<br><b>&nbsp;&nbsp;&nbsp;&nbsp;To switch back to Rockbox</b>" +
                                "<br>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;press & hold:" +
                                "<br>" +
                                "<br><b>Back + Play for 15-20 seconds</b>"))
                    .setPositiveButton("Yes", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface dialog, int which) {
                            // Run in background thread
                            new Thread(new Runnable() {
                                public void run() {
                                    try {
                                        Log.d("RockboxService", "Switching to stock firmware...");
                                        java.lang.Process proc = Runtime.getRuntime().exec(new String[]{"su", "-c", "sh", "/data/data/switch-to-stock.sh"});
                                        proc.waitFor();
                                    } catch (Exception e) {
                                        Log.e("RockboxService", "Failed to shutdown device: " + e.getMessage());
                                        e.printStackTrace();
                                    }
                                }
                            }).start();
                        }
                    })
                    .setNegativeButton("No", null)
                    .show();
            }
        });
    }

    public void updateRockbox() {
        final Activity activity = getActivity();
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                new AlertDialog.Builder(activity)
                    .setTitle("Update Rockbox")
                    .setMessage("Do you want to update Rockbox? \n\nThe device will restart automatically afterwards.")
                    .setPositiveButton("Yes", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface dialog, int which) {
                            // Run in background thread
                            new Thread(new Runnable() {
                                public void run() {
                                    try {
                                        Log.d("RockboxService", "Attempting Rockbox Update...");
                                        String[] cmdArray = new String[] {
                                            "su", "-u", "root", "-c",
                                            "nohup sh /sdcard/.rockbox/update/update.sh > /sdcard/.rockbox/update/nohup.out 2>&1 &"
                                            };
                                        java.lang.Process proc = Runtime.getRuntime().exec(cmdArray);
                                        proc.waitFor();
                                    } catch (Exception e) {
                                        Log.e("RockboxService", "Failed to update Rockbox: " + e.getMessage());
                                        e.printStackTrace();
                                    }
                                }
                            }).start();
                        }
                    })
                    .setNegativeButton("No", null)
                    .show();
            }
        });
    }

    public static void setSystemTimeAsRoot(final String dateString) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    // Parse the date string and convert to Unix timestamp
                    java.text.SimpleDateFormat sdf = new java.text.SimpleDateFormat("yyyyMMdd.HHmmss");
                    java.util.Date date = sdf.parse(dateString);
                    long unixTimestamp = date.getTime() / 1000; // Convert to Unix timestamp

                    // Set the system time using system call
                    Log.d("RockboxTime", "Setting system time to: " + dateString + " (Unix: " + unixTimestamp + ")");
                    String dateCmd = "date " + unixTimestamp + " & am force-stop org.rockbox";
                    Log.d("RockboxTime", "Executing command: " + dateCmd);
                    java.lang.Process process = Runtime.getRuntime().exec(new String[]{"input", "keyevent", "KEYCODE_MEDIA_STOP"});
                    process.waitFor();
                    process = Runtime.getRuntime().exec(new String[]{"sh", "-c", dateCmd});
                    process.waitFor();
                } catch (Exception e) {
                    Log.e("RockboxTime", "Failed to set system time: " + e.getMessage());
                }
            }
        }).start();
    }
}
