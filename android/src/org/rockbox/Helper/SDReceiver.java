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

package org.rockbox.Helper;

import org.rockbox.RockboxService;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.util.Log;

public class SDReceiver extends BroadcastReceiver {
    public SDReceiver(Context c){
        IntentFilter filter = new IntentFilter(Intent.ACTION_MEDIA_MOUNTED);
        filter.addDataScheme("file");
        c.registerReceiver(this, filter);
    }
    @Override
    public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_MEDIA_MOUNTED.equals(intent.getAction())) {
            // Your code here, e.g., scan files, notify user, etc.
            Log.d("RockboxSD", "SD mounted, restart Rockbox.");
            RockboxService s = RockboxService.getInstance();
            s.restartApp();
            // Other operations
        }
    }
}
