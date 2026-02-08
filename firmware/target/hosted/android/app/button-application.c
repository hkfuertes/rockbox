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
 *****************************************************************************/


#include "button.h"
#include "android_keyevents.h"

static bool ignore_back_button = false;
void android_ignore_back_button(bool yes)
{
    ignore_back_button = yes;
}

int key_to_button(int keyboard_key)
{
    switch (keyboard_key)
    {
        case KEYCODE_BACK:
            return BUTTON_MENU;
        case KEYCODE_ENTER:
            return BUTTON_SELECT;
        /*
        case KEYCODE_VOLUME_UP:
            return BUTTON_VOL_UP;
        case KEYCODE_VOLUME_DOWN:
            return BUTTON_VOL_DOWN;
        */
        default:
            return BUTTON_NONE;
    }
}

unsigned multimedia_to_button(int keyboard_key)
{
    switch (keyboard_key)
    {
        case KEYCODE_MEDIA_PLAY_PAUSE:
            return BUTTON_PLAY;
        case KEYCODE_MEDIA_NEXT:
            return BUTTON_RIGHT;
        case KEYCODE_MEDIA_PREVIOUS:
            return BUTTON_LEFT;
        /* KEYCODE_MEDIA_PLAY and KEYCODE_MEDIA_PAUSE are mapped to the the scroll buttons.
           This way Android recognizes these buttons even when the screen is off. */
        case KEYCODE_MEDIA_PLAY:
            return BUTTON_SCROLL_BACK;
        case KEYCODE_MEDIA_PAUSE:
            return BUTTON_SCROLL_FWD;
        default:
            return BUTTON_NONE;
    }
}

unsigned dpad_to_button(int keyboard_key)
{
    switch (keyboard_key)
    {
        /* These buttons only post a single release event.
         * doing otherwise will cause action.c to lock up waiting for
         * a release (because android sends press/unpress to us too quickly
         */
        
        /*
        case KEYCODE_DPAD_UP:
            return BUTTON_DPAD_UP;
        case KEYCODE_DPAD_DOWN:
            return BUTTON_DPAD_DOWN;
        case KEYCODE_DPAD_LEFT:
            return BUTTON_DPAD_LEFT;
        case KEYCODE_DPAD_RIGHT:
            return BUTTON_DPAD_RIGHT;
        */
        default:
            return BUTTON_NONE;
    }
}
    
