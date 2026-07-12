package com.github.Keriew.augustus;

import android.app.WallpaperManager;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.documentfile.provider.DocumentFile;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Setup / settings screen for the live wallpaper. Under the 2026-07-12 data model the wallpaper
 * reads Caesar 3 data from the app's internal storage, not over SAF at runtime. This activity lets
 * the user pick their Caesar 3 folder once and copies every file into {@code getFilesDir()/c3/},
 * where the native engine finds it with plain fopen (see android.c). It also drops the bundled
 * wallpaper save into that same directory so the wallpaper has a city to render.
 */
public class AssetSelectionActivity extends AppCompatActivity {

    private static final String TAG = "augustus";
    private static final String PREFS_NAME = "augustus_prefs";
    private static final String PREF_ASSETS_READY = "assets_ready";
    private static final String PREF_C3_PATH = "c3_path";
    private static final String C3_DIR_NAME = "c3";
    private static final String WALLPAPER_SAVE = "wallpaper.svx";
    private static final int COPY_BUFFER_SIZE = 64 * 1024;
    // EXTRA_INITIAL_URI hint (API 26+) so the SAF picker opens at Download instead of the
    // last-used provider (e.g. Google Drive), where the pushed C3 data lives.
    private static final Uri INITIAL_PICKER_URI =
            Uri.parse("content://com.android.externalstorage.documents/document/primary%3ADownload");

    private static final String INI_NAME = "augustus.ini";
    private static final String K_SCALE = "ui_wallpaper_scale";
    private static final String K_BRIGHTNESS = "ui_wallpaper_brightness";
    private static final String K_MAP_CHANGE = "ui_wallpaper_map_change_minutes";
    private static final String K_SPEED = "ui_wallpaper_speed";

    private static final int SCALE_MAX = 200;
    private static final int SCALE_DEFAULT = 0;
    private static final int BRIGHTNESS_MAX = 100;
    private static final int BRIGHTNESS_DEFAULT = 100;
    private static final int SPEED_MAX = 200;
    private static final int SPEED_DEFAULT = 0;
    private static final int MAP_CHANGE_DEFAULT_MINUTES = 0;
    private static final int[] MAP_CHANGE_INTERVAL_MINUTES = {0, 10, 30, 120, 1440};

    private static final String SDL_ACTIVITY_CLASS_NAME = "org.libsdl.app.SDLActivity";

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private TextView statusText;
    private Button selectButton;

    private final ActivityResultLauncher<Uri> treePicker = registerForActivityResult(
            new ActivityResultContracts.OpenDocumentTree(),
            uri -> {
                if (uri == null) {
                    return;
                }
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                startCopy(uri);
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_asset_selection);

        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.rootView), (v, windowInsets) -> {
            Insets insets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars());
            v.setPadding(insets.left, insets.top, insets.right, insets.bottom);
            return WindowInsetsCompat.CONSUMED;
        });

        statusText = findViewById(R.id.status_text);
        selectButton = findViewById(R.id.select_button);
        selectButton.setOnClickListener(v ->
                treePicker.launch(Build.VERSION.SDK_INT >= Build.VERSION_CODES.O ? INITIAL_PICKER_URI : null));

        refreshStatus();
        setupSettingsControls();
        setupSetWallpaperButton();
    }

    private void setupSettingsControls() {
        SeekBar scaleBar = findViewById(R.id.scale_bar);
        scaleBar.setMax(SCALE_MAX);
        scaleBar.setProgress(readConfigKey(K_SCALE, SCALE_DEFAULT));
        scaleBar.setOnSeekBarChangeListener(new SimpleSeek(value -> writeConfigKey(K_SCALE, value)));

        SeekBar brightnessBar = findViewById(R.id.brightness_bar);
        brightnessBar.setMax(BRIGHTNESS_MAX);
        brightnessBar.setProgress(readConfigKey(K_BRIGHTNESS, BRIGHTNESS_DEFAULT));
        brightnessBar.setOnSeekBarChangeListener(new SimpleSeek(value -> writeConfigKey(K_BRIGHTNESS, value)));

        SeekBar speedBar = findViewById(R.id.speed_bar);
        speedBar.setMax(SPEED_MAX);
        speedBar.setProgress(readConfigKey(K_SPEED, SPEED_DEFAULT));
        speedBar.setOnSeekBarChangeListener(new SimpleSeek(value -> writeConfigKey(K_SPEED, value)));

        Spinner mapChangeSpinner = findViewById(R.id.map_change_spinner);
        mapChangeSpinner.setAdapter(ArrayAdapter.createFromResource(this, R.array.map_change_options,
                android.R.layout.simple_spinner_dropdown_item));
        int currentMapChangeMinutes = readConfigKey(K_MAP_CHANGE, MAP_CHANGE_DEFAULT_MINUTES);
        for (int i = 0; i < MAP_CHANGE_INTERVAL_MINUTES.length; i++) {
            if (MAP_CHANGE_INTERVAL_MINUTES[i] == currentMapChangeMinutes) {
                mapChangeSpinner.setSelection(i);
            }
        }
        mapChangeSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                writeConfigKey(K_MAP_CHANGE, MAP_CHANGE_INTERVAL_MINUTES[position]);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
    }

    private void setupSetWallpaperButton() {
        findViewById(R.id.set_wallpaper_button).setOnClickListener(v -> {
            Intent intent = new Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER);
            intent.putExtra(WallpaperManager.EXTRA_LIVE_WALLPAPER_COMPONENT,
                    new ComponentName(this, SDL_ACTIVITY_CLASS_NAME));
            try {
                startActivity(intent);
            } catch (ActivityNotFoundException e) {
                startActivity(new Intent(WallpaperManager.ACTION_LIVE_WALLPAPER_CHOOSER));
            }
        });
    }

    private void refreshStatus() {
        boolean ready = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getBoolean(PREF_ASSETS_READY, false);
        statusText.setText(ready ? R.string.asset_status_ready : R.string.asset_status_missing);
    }

    private File c3Dir() {
        return new File(getFilesDir(), C3_DIR_NAME);
    }

    private void writeConfigKey(String key, int value) {
        File ini = new File(c3Dir(), INI_NAME);
        c3Dir().mkdirs();
        LinkedHashMap<String, String> keyValues = new LinkedHashMap<>();
        if (ini.exists()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(ini))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    int eq = line.indexOf('=');
                    if (eq > 0) {
                        keyValues.put(line.substring(0, eq), line.substring(eq + 1));
                    }
                }
            } catch (IOException e) {
                Log.e(TAG, "read ini", e);
            }
        }
        keyValues.put(key, Integer.toString(value));
        try (BufferedWriter writer = new BufferedWriter(new FileWriter(ini))) {
            for (Map.Entry<String, String> entry : keyValues.entrySet()) {
                writer.write(entry.getKey() + "=" + entry.getValue() + "\n");
            }
        } catch (IOException e) {
            Log.e(TAG, "write ini", e);
        }
    }

    private int readConfigKey(String key, int fallback) {
        File ini = new File(c3Dir(), INI_NAME);
        if (!ini.exists()) {
            return fallback;
        }
        try (BufferedReader reader = new BufferedReader(new FileReader(ini))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0 && line.substring(0, eq).equals(key)) {
                    try {
                        return Integer.parseInt(line.substring(eq + 1).trim());
                    } catch (NumberFormatException nfe) {
                        return fallback;
                    }
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "read ini", e);
        }
        return fallback;
    }

    private void startCopy(Uri treeUri) {
        selectButton.setEnabled(false);
        statusText.setText(R.string.asset_status_copying);

        executor.execute(() -> {
            String error = null;
            try {
                File destination = c3Dir();
                if (!destination.exists() && !destination.mkdirs()) {
                    throw new IOException("Cannot create " + destination);
                }
                DocumentFile root = DocumentFile.fromTreeUri(this, treeUri);
                if (root == null) {
                    throw new IOException("Cannot open the selected folder");
                }
                copyTree(root, destination);
                copyBundledSave(destination);

                SharedPreferences.Editor editor = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit();
                editor.putBoolean(PREF_ASSETS_READY, true);
                editor.putString(PREF_C3_PATH, destination.getAbsolutePath());
                editor.apply();
            } catch (Exception e) {
                Log.e(TAG, "Asset copy failed", e);
                error = e.getMessage();
            }

            final String finalError = error;
            mainHandler.post(() -> {
                selectButton.setEnabled(true);
                if (finalError == null) {
                    statusText.setText(R.string.asset_status_ready);
                } else {
                    statusText.setText(getString(R.string.asset_status_error, finalError));
                }
            });
        });
    }

    private void copyTree(DocumentFile dir, File destinationDir) throws IOException {
        for (DocumentFile child : dir.listFiles()) {
            String name = child.getName();
            if (name == null) {
                continue;
            }
            File target = new File(destinationDir, name);
            if (child.isDirectory()) {
                if (!target.exists() && !target.mkdirs()) {
                    throw new IOException("Cannot create " + target);
                }
                copyTree(child, target);
            } else {
                copyStreamToFile(getContentResolver().openInputStream(child.getUri()), target);
            }
        }
    }

    private void copyBundledSave(File destinationDir) throws IOException {
        copyStreamToFile(getAssets().open(WALLPAPER_SAVE), new File(destinationDir, WALLPAPER_SAVE));
    }

    private void copyStreamToFile(InputStream input, File target) throws IOException {
        if (input == null) {
            throw new IOException("Cannot read source for " + target);
        }
        try (InputStream in = input; OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[COPY_BUFFER_SIZE];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        }
    }

    @Override
    protected void onDestroy() {
        executor.shutdown();
        super.onDestroy();
    }

    // Native methods retained from the removed DirectorySelectionActivity. Their JNI exports live in
    // android.c / asset_handler.c as Java_com_github_Keriew_augustus_AssetSelectionActivity_*.
    public native void gotDirectory();

    public native void releaseAssetManager();

    private interface IntConsumer {
        void accept(int value);
    }

    /** Writes the SeekBar's value only once the user releases it, not on every pixel of drag. */
    private static class SimpleSeek implements SeekBar.OnSeekBarChangeListener {
        private final IntConsumer onRelease;

        private SimpleSeek(IntConsumer onRelease) {
            this.onRelease = onRelease;
        }

        @Override
        public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
        }

        @Override
        public void onStartTrackingTouch(SeekBar seekBar) {
        }

        @Override
        public void onStopTrackingTouch(SeekBar seekBar) {
            onRelease.accept(seekBar.getProgress());
        }
    }
}
