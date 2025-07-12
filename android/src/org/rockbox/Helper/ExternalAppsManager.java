package org.rockbox.Helper;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Drawable;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * Manager class for handling external Android applications
 * List installed apps and launch them
 */
public class ExternalAppsManager {

    private Context context;
    private PackageManager packageManager;

    public static class AppInfo {
        public String packageName;
        public String appName;
        public String className;
        public boolean isSystemApp;

        public AppInfo(String packageName, String appName, String className, boolean isSystemApp) {
            this.packageName = packageName;
            this.appName = appName;
            this.className = className;
            this.isSystemApp = isSystemApp;
        }
    }

    public ExternalAppsManager(Context context) {
        this.context = context;
        this.packageManager = context.getPackageManager();
    }

    public List<AppInfo> getInstalledApps() {
        List<AppInfo> apps = new ArrayList<>();

        try {
            // Get all installed applications
            List<ApplicationInfo> installedApps = packageManager.getInstalledApplications(
                PackageManager.GET_META_DATA);

            for (ApplicationInfo appInfo : installedApps) {
                // Skip system apps
                if ((appInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0) {
                    continue;
                }

                // Get the main activity for this app
                Intent launchIntent = packageManager.getLaunchIntentForPackage(appInfo.packageName);
                if (launchIntent != null) {
                    String appName = appInfo.loadLabel(packageManager).toString();
                    String className = launchIntent.getComponent().getClassName();

                    apps.add(new AppInfo(appInfo.packageName, appName, className, false));
                }
            }

            // Sort apps alphabetically
            Collections.sort(apps, new Comparator<AppInfo>() {
                @Override
                public int compare(AppInfo app1, AppInfo app2) {
                    return app1.appName.compareToIgnoreCase(app2.appName);
                }
            });

        } catch (Exception e) {
            Logger.d("Error getting installed apps", e);
        }

        return apps;
    }

    public boolean launchApp(String packageName) {
        try {
            Intent launchIntent = packageManager.getLaunchIntentForPackage(packageName);
            if (launchIntent != null) {
                launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(launchIntent);
                return true;
            }
        } catch (Exception e) {
            Logger.d("Error launching app: " + packageName, e);
        }
        return false;
    }

    public int getAppCount() {
        return getInstalledApps().size();
    }

}