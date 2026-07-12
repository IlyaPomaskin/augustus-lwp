package com.github.Keriew.augustus;

import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Button;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
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
    }

    private void refreshStatus() {
        boolean ready = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getBoolean(PREF_ASSETS_READY, false);
        statusText.setText(ready ? R.string.asset_status_ready : R.string.asset_status_missing);
    }

    private File c3Dir() {
        return new File(getFilesDir(), C3_DIR_NAME);
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
}
