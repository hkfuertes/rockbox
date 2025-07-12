#include "config.h"
#include "system.h"
#include "lang.h"
#include "menu.h"
#include "list.h"
#include "action.h"
#include "screens.h"
#include "yesno.h"
#include "settings.h"
#include "splash.h"

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
#include "../firmware/target/hosted/android/external-apps-android.h"
#endif

/* External apps menu data structure */
struct external_apps_data {
    int app_count;
    int current_selection;
    bool needs_refresh;
};

/* Callback function for the external apps list */
static const char* external_apps_get_name(int selected_item, void *data, char *buffer, size_t buffer_len)
{
    (void)data;

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
    const char* app_name = android_external_apps_get_name(selected_item);
    if (app_name != NULL) {
        strlcpy(buffer, app_name, buffer_len);
        return buffer;
    }
#endif

    /* Fallback if Android API is not available */
    snprintf(buffer, buffer_len, "App %d", selected_item + 1);
    return buffer;
}

/* Callback function for external apps menu actions */
static int external_apps_action_callback(int action, struct gui_synclist *lists)
{
    int selected = lists->selected_item;

    switch (action) {
        case ACTION_STD_OK:
            /* Launch the selected app */
#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
            if (android_external_apps_launch(selected)) {
                /* App launched successfully */
                return ACTION_EXIT_MENUITEM;
            } else {
                /* Failed to launch app */
                splash(HZ*2, "Failed to launch app");
            }
#endif
            break;

        case ACTION_STD_CANCEL:
            return action;

        case ACTION_STD_CONTEXT:
            /* Show context menu for the selected app */
            {
#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
                const char* app_name = android_external_apps_get_name(selected);
                const char* package_name = android_external_apps_get_package_name(selected);

                if (app_name != NULL && package_name != NULL) {
                    char message[256];
                    snprintf(message, sizeof(message), "Launch %s?", app_name);

                    if (yesno_pop_confirm(message)) {
                        if (android_external_apps_launch(selected)) {
                            return ACTION_EXIT_MENUITEM;
                        } else {
                            splash(HZ*2, "Failed to launch app");
                        }
                    }
                }
#endif
            }
            break;
    }

    return action;
}

/* Main external apps menu function */
static int show_external_apps(void)
{
    struct external_apps_data data = {0, 0, true};
    struct simplelist_info info;

#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
    /* Get the number of installed apps */
    data.app_count = android_external_apps_get_count();
#else
    data.app_count = 0;
#endif

    if (data.app_count == 0) {
        splash(HZ*2, "No external apps found");
        return 0;
    }

    /* Initialize the simplelist */
    simplelist_info_init(&info, str(LANG_EXTERNAL_APPS), data.app_count, (void*)&data);
    info.get_name = external_apps_get_name;
    info.action_callback = external_apps_action_callback;
    info.title_icon = Icon_NOICON;

    /* Show the list */
    return simplelist_show_list(&info) ? 1 : 0;
}

/* External apps menu item */
#if (CONFIG_PLATFORM & PLATFORM_ANDROID)
MENUITEM_FUNCTION(external_apps_item, 0, ID2P(LANG_EXTERNAL_APPS),
                  show_external_apps, NULL, Icon_NOICON);
#endif