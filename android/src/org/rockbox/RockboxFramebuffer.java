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

import java.nio.ByteBuffer;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewConfiguration;
import android.os.Vibrator;
import android.util.Log;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.graphics.Paint;

public class RockboxFramebuffer extends SurfaceView 
                                 implements SurfaceHolder.Callback
{
    private final DisplayMetrics metrics;
    private final ViewConfiguration view_config;
    private Bitmap btm;
    private final Paint sharpPaint = new Paint();

    private static final int[] duration_mapping = {
        0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50
    };

    private static final int CENTER_KEYCODE = KeyEvent.KEYCODE_ENTER; // 66
    private static final long LONG_PRESS_DURATION_MS = 1000;
    private Handler longPressHandler = new Handler(Looper.getMainLooper());
    private boolean centerLongPressDetected = false;
    private PowerManager powerManager;

    private boolean justCreated = true;
    private Runnable centerLongPressRunnable = new Runnable() {
        @Override
        public void run() {
            centerLongPressDetected = true;
            Log.d("RockboxButton", "Turning off screen...");
        }
    };

    // Framebuffer size constants
    private int FB_WIDTH = 480;   // Native resolution
    private int FB_HEIGHT = 360;  // Native resolution

    /* first stage init; needs to run from a thread that has a Looper 
     * setup stuff that needs a Context */
    public RockboxFramebuffer(Context c)
    {
        super(c);
        metrics = c.getResources().getDisplayMetrics();
        view_config = ViewConfiguration.get(c);
        getHolder().addCallback(this);
        /* Needed so we can catch KeyEvents */
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(true);
        /* don't draw until native is ready (2nd stage) */
        setEnabled(false);
        sharpPaint.setFilterBitmap(false);
        powerManager = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
    }

    private void update(ByteBuffer framebuffer)
    {
        SurfaceHolder holder = getHolder();                            
        Canvas c = holder.lockCanvas();
        if (c == null)
            return;

        btm.copyPixelsFromBuffer(framebuffer);
        synchronized (holder)
        { /* draw */
            c.drawColor(android.graphics.Color.BLACK); // clear canvas first!
            
            // Check resolution mode
            int currentMode = getDisplayResolutionMode();
            int currentActivity = getCurrentActivity();
            
            // Don't scale plugins - they're designed for 360p resolution
            boolean isPlugin = (currentActivity == 14); // ACTIVITY_PLUGIN = 14

            if (isPlugin) {
                // Plugin mode - render the full 480x360 framebuffer directly, no scaling
                Rect src = new Rect(0, 0, FB_WIDTH, FB_HEIGHT);
                Rect dst = new Rect(0, 0, c.getWidth(), c.getHeight());
                c.drawBitmap(btm, src, dst, sharpPaint);
            } else if (currentMode == 1) {
                // 360p native mode - no scaling, render at native 480x360
                Rect src = new Rect(0, 0, FB_WIDTH, FB_HEIGHT);
                Rect dst = new Rect(0, 0, c.getWidth(), c.getHeight());
                c.drawBitmap(btm, src, dst, sharpPaint);
            } else {
                // 240p compatibility mode - extract 320x240 from center of 480x360 and scale up
                int virtualWidth = 320;
                int virtualHeight = 240;
                
                // Source: extract 320x240 from the top left of the 480x360 framebuffer
                Rect src = new Rect(0, 0, virtualWidth, virtualHeight);
                
                // Destination: scale the 320x240 to fill the entire screen
                Rect dst = new Rect(0, 0, c.getWidth(), c.getHeight());
                
                c.drawBitmap(btm, src, dst, sharpPaint);
            }
        }
        holder.unlockCanvasAndPost(c);
    }
    
    private void update(ByteBuffer framebuffer, Rect dirty)
    {
        update(framebuffer);
    }

    public boolean onTouchEvent(MotionEvent me)
    {
        // Check resolution mode for proper scaling
        int currentMode = getDisplayResolutionMode();
        float scaleX, scaleY;
        
        if (currentMode == 1) {
            // 360p native mode - direct mapping from screen to 480x360
            scaleX = (float)FB_WIDTH / (float)getWidth();
            scaleY = (float)FB_HEIGHT / (float)getHeight();
        } else {
            // 240p compatibility mode - map from screen to 320x240
            scaleX = 320.0f / (float)getWidth();
            scaleY = 240.0f / (float)getHeight();
        }
        
        int x = (int) (me.getX() * scaleX);
        int y = (int) (me.getY() * scaleY);

        switch (me.getAction())
        {
        case MotionEvent.ACTION_CANCEL:
        case MotionEvent.ACTION_UP:
            touchHandler(false, x, y);
            return true;
        case MotionEvent.ACTION_MOVE:
        case MotionEvent.ACTION_DOWN:
            touchHandler(true, x, y);
            return true;
        }

        return false;
    }

    public boolean onKeyDown(int keyCode, KeyEvent event)
    {
        justCreated = false;
        if ((keyCode == CENTER_KEYCODE) && event.getRepeatCount() == 0) {
            centerLongPressDetected = false;
            longPressHandler.postDelayed(centerLongPressRunnable, LONG_PRESS_DURATION_MS);
            return true;
        }
        /* Handle repeat events */
        else {
            if (event.getRepeatCount() > 0)
            {
                return buttonHandlerRepeat(keyCode);
            }
            else
            {
                return buttonHandler(keyCode, true);
            }
        }
    }

    public boolean onKeyUp(int keyCode, KeyEvent event)
    {
        if (keyCode == CENTER_KEYCODE) {
            // Cancel pending timer
            longPressHandler.removeCallbacks(centerLongPressRunnable);

            // Only put device to sleep if long-press was detected
            if (centerLongPressDetected) {
                centerLongPressDetected = false;
                Log.d("RockboxButton", "Putting device to sleep");
                try {
                    powerManager.goToSleep(SystemClock.uptimeMillis());
                    Log.d("RockboxButton", "Device put to sleep");
                } catch (Exception e) {
                    Log.e("RockboxButton", "Failed to put device to sleep: " + e.getMessage());
                }
                return true;
            } else if (!justCreated){
                // center button was pressed but not long enough, handle like a normal press
                try {
                    buttonHandler(keyCode, true);
                    // pause to make Rockbox catch up
                    Thread.sleep(10);
                } catch (InterruptedException e) {
                    Log.e("RockboxButton", "Failed sending center keyDown event: " + e.getMessage());
                }
            }
        }
        if (!(keyCode == CENTER_KEYCODE && justCreated)){
            justCreated = false;
            return buttonHandler(keyCode, false);
        }
        else {
            Log.d("RockboxButton", "Just woke up - don't handle this center press");
            justCreated = false;
            return true;
        }
    }
 
    private int getDpi()
    {
        return metrics.densityDpi;
    }

    private int getScrollThreshold()
    {
        return view_config.getScaledTouchSlop();
    }

    private native void touchHandler(boolean down, int x, int y);
    public native static boolean buttonHandler(int keycode, boolean state);
    public native static boolean buttonHandlerRepeat(int keycode);
    public native static void triggerVibrationNative(int baseDuration, int boostDuration);
    
    public native void surfaceCreated(SurfaceHolder holder);
    
    // Add native method to force a full redraw from native code
    public native void forceFullRedraw();

    // Add method to change resolution
    public native int getDisplayResolutionMode();
    
    // Add method to get current activity
    public native int getCurrentActivity();

    // Add native method to notify setting change
    public native void onDisplayResolutionChanged(int mode);

    /* Trigger vibration for button feedback */
    public static void triggerVibration(Context context, int baseDuration, int boostDuration) {
        try {
            Vibrator vibrator = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
            if (vibrator != null) {
                int base_ms = duration_mapping[baseDuration];
                int boost_ms = boostDuration;
                int total_ms = base_ms + boost_ms;
                vibrator.vibrate(total_ms);
            } else {
                android.util.Log.e("RockboxFramebuffer", "Vibrator is null");
            }
        } catch (Exception e) {
            android.util.Log.e("RockboxFramebuffer", "Vibration error: " + e.getMessage());
        }
    }
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Always create the bitmap at the native framebuffer size (480x360)
        btm = Bitmap.createBitmap(FB_WIDTH, FB_HEIGHT, Bitmap.Config.RGB_565);
        setEnabled(true);
        // Trigger a full framebuffer redraw from native code
        forceFullRedraw();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        setEnabled(false);
        btm = null;
    }
}
