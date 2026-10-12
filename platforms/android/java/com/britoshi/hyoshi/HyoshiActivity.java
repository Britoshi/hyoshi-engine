package com.britoshi.hyoshi;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

// The activity for a game on Hyoshi Engine: SDL's, loading the game from libmain.so (the library
// hyoshi_add_app makes on Android). SDL is linked into it statically, so there's no libSDL3.so.
//
// Files opened with the app or shared to it (the manifest's intent filters) are copied into a
// folder of their own in the cache's opened/ folder, under their own names, and reach the game as
// files dropped on the window (Platform::TakeDroppedFiles); ForgetDroppedFile deletes the copies.
// Until the native side is ready for them (onNativeReady), they wait here.
public class HyoshiActivity extends SDLActivity {
    private static final String TAG = "Hyoshi";
    // Where copies go, inside getCacheDir(); Platform.cpp names it too.
    private static final String OPENED_FOLDER = "opened";

    private static final Object openedLock = new Object();
    private static final List<String> pendingFiles = new ArrayList<>();
    private static boolean isNativeReady = false;
    private static long copyCount = 0;

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    // The orientation is the manifest's, changed only by the engine (Platform::SetOrientation) when
    // the game asks for another. SDL would request its own each time it makes its window: each new
    // request makes the system apply the rotation lock again, and the screen turns about.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // SDL's onCreate would pass a content:// link's path along as a file, which it isn't.
        List<Uri> opened = takeOpenedUris(getIntent());
        super.onCreate(savedInstanceState);
        copyOpenedFiles(opened);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        copyOpenedFiles(takeOpenedUris(intent));
    }

    // Called by the native side once SDL takes events: files opened before then arrive now.
    public static void onNativeReady() {
        List<String> ready;
        synchronized (openedLock) {
            isNativeReady = true;
            ready = new ArrayList<>(pendingFiles);
            pendingFiles.clear();
        }
        for (String path : ready) {
            SDLActivity.onNativeDropFile(path);
        }
    }

    // The files an intent carries (opened with the app, or shared to it), cleared from the intent.
    private List<Uri> takeOpenedUris(Intent intent) {
        List<Uri> uris = new ArrayList<>();
        if (intent == null) {
            return uris;
        }
        String action = intent.getAction();
        if (Intent.ACTION_VIEW.equals(action) && intent.getData() != null) {
            uris.add(intent.getData());
            intent.setData(null);
        } else if (Intent.ACTION_SEND.equals(action) || Intent.ACTION_SEND_MULTIPLE.equals(action)) {
            uris.addAll(getSharedUris(intent, Intent.ACTION_SEND_MULTIPLE.equals(action)));
        }
        intent.removeExtra(Intent.EXTRA_STREAM);
        return uris;
    }

    // EXTRA_STREAM's links; Android 13 added typed getters and deprecated the old ones.
    @SuppressWarnings("deprecation")
    private static List<Uri> getSharedUris(Intent intent, boolean isMultiple) {
        List<Uri> uris = new ArrayList<>();
        if (isMultiple) {
            ArrayList<Uri> shared = Build.VERSION.SDK_INT >= 33
                                        ? intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri.class)
                                        : intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (shared != null) {
                uris.addAll(shared);
            }
        } else {
            Uri uri = Build.VERSION.SDK_INT >= 33 ? intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri.class)
                                                  : intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (uri != null) {
                uris.add(uri);
            }
        }
        return uris;
    }

    // Copies off the main thread, since archives can be large.
    private void copyOpenedFiles(List<Uri> uris) {
        if (uris.isEmpty()) {
            return;
        }
        final File opened = new File(getCacheDir(), OPENED_FOLDER);
        new Thread(() -> {
            for (Uri uri : uris) {
                File folder;
                synchronized (openedLock) {
                    folder = new File(opened, System.currentTimeMillis() + "-" + ++copyCount);
                }
                folder.mkdirs();
                File copy = copyToFolder(uri, folder);
                if (copy == null) {
                    continue;
                }
                String path = copy.getAbsolutePath();
                boolean isReady;
                synchronized (openedLock) {
                    isReady = isNativeReady;
                    if (!isReady) {
                        pendingFiles.add(path);
                    }
                }
                if (isReady) {
                    SDLActivity.onNativeDropFile(path);
                }
            }
        }, "HyoshiOpenedFiles").start();
    }

    private File copyToFolder(Uri uri, File folder) {
        File copy = new File(folder, getDisplayName(uri));
        try (InputStream input = getContentResolver().openInputStream(uri);
             OutputStream output = new FileOutputStream(copy)) {
            if (input == null) {
                throw new java.io.IOException("no stream");
            }
            byte[] buffer = new byte[1 << 16];
            for (int read; (read = input.read(buffer)) > 0;) {
                output.write(buffer, 0, read);
            }
            Log.i(TAG, "Opened " + uri + " as " + copy.getName());
            return copy;
        } catch (Exception e) {
            Log.w(TAG, "Could not open " + uri + ": " + e);
            copy.delete();
            folder.delete();
            return null;
        }
    }

    // The file's own name (with its extension, which the game goes by), else the link's last part.
    private String getDisplayName(Uri uri) {
        String name = null;
        if ("content".equals(uri.getScheme())) {
            try (Cursor cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME },
                                                            null, null, null)) {
                if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                    name = cursor.getString(0);
                }
            } catch (Exception e) {
                Log.w(TAG, "No name for " + uri + ": " + e);
            }
        }
        if (name == null) {
            name = uri.getLastPathSegment();
        }
        if (name == null) {
            name = "opened";
        }
        name = name.replaceAll("[/\\\\:*?\"<>|]", "_").trim();
        return name.isEmpty() || name.startsWith(".") ? "opened" + name : name;
    }
}
