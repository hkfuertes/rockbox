#include "config.h"
#include "stdarg.h"
#include "string.h"
#include "stdio.h"
#include "kernel.h"
#include "system.h"
#include "screen_access.h"
#include "font.h"
#include "debug.h"
#include "misc.h"
#include "settings.h"
#include "scrollbar.h"
#include "lang.h"
#include "action.h"
#include "icon.h"
#include "brightness_picker.h"
#include "viewport.h"
#include "backlight.h"

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
#include "../firmware/target/hosted/android/brightness-android.h"
#endif

/* structure for brightness info */
struct brightness_pick
{
    int brightness;                 /* brightness value (0-100) */
};

/* maximum brightness value */
#define BRIGHTNESS_MAX 100

/* minimum brightness value */
#define BRIGHTNESS_MIN 0

/* brightness step size */
#define BRIGHTNESS_STEP 4

#define MARGIN_TOP              2 /* Top margin of screen                 */
#define MARGIN_BOTTOM           6 /* Bottom margin of screen              */
#define SLIDER_TEXT_MARGIN      2 /* Gap between text and slider          */
#define TITLE_MARGIN_BOTTOM     4 /* Space below title bar                */
#define SELECTOR_TB_MARGIN      1 /* Margin on top and bottom of selector */
#define SELECTOR_WIDTH          get_icon_width(display->screen_type)
#define SELECTOR_HEIGHT         8 /* Height of > and < bitmaps            */

static void draw_screen(struct screen *display, char *title,
                        struct brightness_pick *brightness)
{
    unsigned  text_color       = LCD_BLACK;
    unsigned  background_color = LCD_WHITE;
    int       char_height, line_height;
    int       text_top;
    int       slider_x, slider_width;
    struct viewport vp;

    viewport_set_defaults(&vp, display->screen_type);
    struct viewport * last_vp = display->set_viewport(&vp);

    display->clear_viewport();

    if (display->depth > 1)
    {
        text_color       = display->get_foreground();
        background_color = display->get_background();
    }

    /* Draw title string */
    display->set_drawinfo(DRMODE_SOLID, text_color, background_color);
    vp.flags |= VP_FLAG_ALIGN_CENTER;
    display->putsxy(0, MARGIN_TOP, title);

    /* Get slider positions and top starting position */
    char_height  = display->getcharheight();
    text_top     = MARGIN_TOP + char_height +
                   TITLE_MARGIN_BOTTOM + SELECTOR_TB_MARGIN;
    slider_x     = SELECTOR_WIDTH;
    slider_width = vp.width - SELECTOR_WIDTH - slider_x - SLIDER_TEXT_MARGIN;
    line_height  = char_height + 2*SELECTOR_TB_MARGIN;

    /* Draw scrollbar */
    unsigned sb_flags = HORIZONTAL;
    display->set_drawinfo(DRMODE_SOLID, text_color, background_color);

    gui_scrollbar_draw(display,                     /* screen */
                       slider_x,                    /* x */
                       text_top + char_height / 4,  /* y */
                       slider_width,                /* width */
                       char_height / 2,             /* height */
                       BRIGHTNESS_MAX,              /* items */
                       0,                           /* min_shown */
                       brightness->brightness,      /* max_shown */
                       sb_flags);                   /* flags */


    /* Draw instructions */
    text_top += line_height * 2;
    display->set_drawinfo(DRMODE_SOLID, text_color, background_color);
    vp.flags |= VP_FLAG_ALIGN_CENTER;
    display->putsxy(0, text_top, "Scroll Wheel: adjust brightness");
    text_top += char_height;
    display->putsxy(0, text_top, "Back/Menu: exit");

    display->update_viewport();
    display->set_viewport(last_vp);
}

/***********
 set_brightness
 returns true if USB was inserted, false otherwise
 brightness is a pointer to the brightness value (0-100) to modify
 ***********/
bool set_brightness(struct screen *display, char *title, int *brightness)
{
    int exit = 0;
    struct brightness_pick brightness_pick;

    /* Initialize with saved brightness setting if available */
    brightness_pick.brightness = global_settings.brightness;

    /* Apply the initial brightness setting */
    backlight_set_brightness(brightness_pick.brightness);

    while (!exit)
    {
        int button;

        if (display != NULL)
        {
            draw_screen(display, title, &brightness_pick);
        }
        else
        {
            FOR_NB_SCREENS(i)
                draw_screen(&screens[i], title, &brightness_pick);
        }

        button = get_action(CONTEXT_SETTINGS_BRIGHTNESSCHOOSER, TIMEOUT_BLOCK);

        switch (button)
        {
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
            case ACTION_SETTINGS_DEC:
            case ACTION_SETTINGS_DECREPEAT:
                /* Decrease brightness */
                if (brightness_pick.brightness > BRIGHTNESS_MIN) {
                    brightness_pick.brightness = brightness_pick.brightness - BRIGHTNESS_STEP;
                    /* Apply the change immediately */
                    global_settings.brightness = brightness_pick.brightness;
                    backlight_set_brightness(brightness_pick.brightness);
                }
                break;

            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
            case ACTION_SETTINGS_INC:
            case ACTION_SETTINGS_INCREPEAT:
                /* Increase brightness */
                if (brightness_pick.brightness < BRIGHTNESS_MAX) {
                    brightness_pick.brightness = brightness_pick.brightness + BRIGHTNESS_STEP;
                    /* Apply the change immediately */
                    global_settings.brightness = brightness_pick.brightness;
                    backlight_set_brightness(brightness_pick.brightness);
                }
                break;

            case ACTION_STD_CANCEL:
            case ACTION_STD_MENU:
            case ACTION_STD_OK:
                /* Exit and save the current brightness setting */
                global_settings.brightness = brightness_pick.brightness;
                settings_save();
                exit = 1;
                break;

            default:
                if (default_event_handler(button) == SYS_USB_CONNECTED)
                    return true;
                break;
        }
    }

    /* Update the passed brightness value */
    *brightness = brightness_pick.brightness;

    return false;
} 
