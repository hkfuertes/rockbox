package org.rockbox.Helper;

import android.content.Context;
import android.content.Intent;
import android.content.BroadcastReceiver;
import android.util.Log;

public class HeadphoneUnpluggedReceiver extends BroadcastReceiver {
    private boolean wasPluggedIn = false;
    private int doRestart = 0;

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent.hasExtra("state")) {
            int state = intent.getIntExtra("state", -1);
            if (state == 1) {
                // Headset plugged in, update state
                wasPluggedIn = true;
            } else if (state == 0) {
                // Headset unplugged
                if (wasPluggedIn && (doRestart == 0)) {
                    new Thread(new Runnable() {
                        @Override
                        public void run() {
                            try {
                                Log.d("RockboxService", "Headphones unplugged, restarting Rockbox");
                                java.lang.Process proc = Runtime.getRuntime().exec(new String[]{"am", "force-stop", "org.rockbox"});
                                proc.waitFor();
                            } catch (Exception e) {
                                Log.e("RockboxService", "Failed to restart Rockbox: " + e.getMessage());
                                e.printStackTrace();
                            }
                        }
                    }).start();
                }
            }
        }
    }

    public int setRestart(int config){
        doRestart = config;
        return 1;
    }
}