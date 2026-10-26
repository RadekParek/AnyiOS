package org.podnative;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import dev.radek.conventor.SharedCatalog;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;

public final class MainActivity extends Activity {
    private static final int IMPORT_FILE = 10, EXPORT = 11, IMPORT_FOLDER = 12,
                             IMPORT_APP_FOLDER = 13, EXPORT_SANDBOX = 14, RUN_ANYIOSEMU = 15;
    private SharedPreferences prefs;
    private boolean landscapeRequested = false;
    private GameLibrary library;
    private PrototypeView prototype;
    private TextView liveStatus, progressLabel;
    private String tab = "Library", exportGame = "", activeAnyiOSEmuGameId = "";
    private boolean resumeTest, dark;
    private int background, surface, ink, muted, line, accent, soft;
    private long renderedRevision;
    private boolean renderedBusy;
    private String renderedNotice = "";
    private final Runnable libraryChanged = () -> {
        if (isFinishing() || isDestroyed())
            return;
        if (prototype != null)
            return;
        if (renderedRevision == library.revision && renderedBusy == SharedCatalog.isBusy()
            && renderedNotice.equals(library.notice) && progressLabel != null)
            progressLabel.setText(library.progress);
        else
            home();
    };
    @Override
    public void onCreate(Bundle state) {
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        // Keep the native diagnostics/ANGLE test in sync with persisted switches before its
        // renderer initializes. AnyiOSEMU reads the same settings before its next SDL launch.
        NativeBridge.setVerbose(prefs.getBoolean("verboseLogging", false));
        NativeBridge.setNativeGles(prefs.getBoolean("nativeGles", false));
        dark =
            !getSharedPreferences("radek_settings", MODE_PRIVATE).getBoolean("light_theme", false);
        setTheme(dark ? R.style.AppThemeDark : R.style.AppTheme);
        super.onCreate(state);
        library = GameLibrary.get(this);
        if (state != null) {
            resumeTest = state.getBoolean("prototype", false);
            tab = state.getString("tab", "Library");
            exportGame = state.getString("exportGame", "");
        }
        background = color(dark ? "#0B101D" : "#F5F7FA");
        surface = color(dark ? "#141D2F" : "#FFFFFF");
        ink = color(dark ? "#FFFFFF" : "#16202E");
        muted = color(dark ? "#A0B2C7" : "#5C6877");
        line = color(dark ? "#2D3B4F" : "#DCE2EA");
        accent = dev.radek.conventor.SharedTheme.accent(this);
        soft = color(dark ? "#203E30" : "#DDF3E7");
        Diagnostics.record(
            "APP", "AnyiOSEMU ready; IPA and .app launches use the ARM32 guest runtime");
        home();
        if (state == null && getIntent().getBooleanExtra("importIpaFolder", false))
            selectFolder(IMPORT_FOLDER);
    }
    /** SHA-256 of the signing certificate, so a sideloaded APK can be verified by hand. */
    private String signatureFingerprint() {
        try {
            android.content.pm.Signature[] signatures =
                getPackageManager()
                    .getPackageInfo(getPackageName(),
                        android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES)
                    .signingInfo.getApkContentsSigners();
            java.security.MessageDigest digest = java.security.MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest(signatures[0].toByteArray());
            StringBuilder out = new StringBuilder();
            for (byte value : hash)
                out.append(String.format(java.util.Locale.ROOT, "%02X:", value));
            return out.substring(0, out.length() - 1);
        } catch (Exception e) {
            return "Unavailable: " + e.getMessage();
        }
    }
    private void appendNoticeAsset(StringBuilder notices, String name, String label) {
        notices.append("\n\n--- ").append(label).append(" ---\n\n");
        try (InputStream input = getAssets().open(name);
            ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int read;
            while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
            notices.append(new String(output.toByteArray(), StandardCharsets.UTF_8));
        } catch (Exception e) {
            notices.append("Could not load bundled notice asset '")
                .append(name)
                .append("': ")
                .append(e.getMessage());
        }
    }
    private void appendNoticeAssetsUnder(StringBuilder notices, String directory) {
        try {
            String[] children = getAssets().list(directory);
            if (children == null || children.length == 0) {
                appendNoticeAsset(notices, directory, directory);
                return;
            }
            for (String child : children) {
                String path = directory + "/" + child;
                String[] nested = getAssets().list(path);
                if (nested != null && nested.length > 0)
                    appendNoticeAssetsUnder(notices, path);
                else
                    appendNoticeAsset(notices, path, path);
            }
        } catch (Exception e) {
            notices.append("\n\nCould not list bundled notice directory '")
                .append(directory)
                .append("': ")
                .append(e.getMessage());
        }
    }
    private void showRuntimeNotices() {
        StringBuilder notices = new StringBuilder();
        appendNoticeAsset(notices, "anyiose-runtime-notices.txt", "AnyiOSEMU provenance");
        appendNoticeAsset(notices, "LICENSE", "Mozilla Public License 2.0");
        String[] noticeAssets = {"vendor/SDL/LICENSE.txt", "vendor/dynarmic/LICENSE.txt",
            "vendor/dynarmic/README.md", "vendor/openal-soft/COPYING", "vendor/libxml2/Copyright",
            "vendor/stb/LICENSE", "vendor/PVRTDecompress/LICENSE.md",
            "vendor/symphonia-codec-aac/LICENSE", "touchHLE_dylibs/README.md",
            "touchHLE_dylibs/COPYING.libgcc", "touchHLE_dylibs/COPYING.libsqlite3",
            "touchHLE_dylibs/COPYING.libstdcxx", "touchHLE_dylibs/COPYING.libz",
            "touchHLE_fonts/README.md", "touchHLE_fonts/LICENSE.liberation",
            "touchHLE_fonts/LICENSE.noto", "anyiose-rust-dependencies.txt",
            "anyiose-dynarmic-license.txt"};
        for (String name : noticeAssets) appendNoticeAsset(notices, name, name);
        appendNoticeAssetsUnder(notices, "angle-notices");
        TextView text = text(notices.toString(), 12, ink, false);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextIsSelectable(true);
        text.setPadding(dp(18), dp(14), dp(18), dp(14));
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        new AlertDialog.Builder(this)
            .setTitle("AnyiOSEMU third-party notices")
            .setView(scroll)
            .setPositiveButton("Done", null)
            .show();
    }
    private static int color(String value) {
        return Color.parseColor(value);
    }
    private int dp(int n) {
        return Math.round(n * getResources().getDisplayMetrics().density);
    }
    private LinearLayout column() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }
    private LinearLayout row() {
        LinearLayout layout = new LinearLayout(this);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        return layout;
    }
    private GradientDrawable shape(int fill, int radius, boolean stroke) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(dp(radius));
        if (stroke)
            drawable.setStroke(dp(1), line);
        return drawable;
    }
    private void clickable(View view, int fill, int radius, Runnable action) {
        view.setBackground(
            new RippleDrawable(ColorStateList.valueOf(dark ? 0x20ffffff : 0x14000000),
                shape(fill, radius, false), shape(Color.WHITE, radius, false)));
        view.setFocusable(true);
        view.setOnClickListener(v -> {
            if (android.animation.ValueAnimator.areAnimatorsEnabled()) {
                v.animate()
                    .scaleX(0.97f)
                    .scaleY(0.97f)
                    .setDuration(70)
                    .withEndAction(() -> v.animate().scaleX(1).scaleY(1).setDuration(140).start())
                    .start();
            }
            action.run();
        });
    }
    private TextView text(String content, int size, int textColor, boolean bold) {
        TextView view = new TextView(this);
        view.setText(content);
        view.setTextSize(size);
        view.setTextColor(textColor);
        view.setFontFeatureSettings("kern");
        view.setTypeface(
            Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        view.setLineSpacing(dp(2), 1);
        view.setIncludeFontPadding(false);
        return view;
    }
    private void gap(LinearLayout layout, int height) {
        layout.addView(new View(this), new LinearLayout.LayoutParams(1, dp(height)));
    }
    private Button action(String label, boolean primary, Runnable callback) {
        Button view = new Button(this);
        view.setAllCaps(false);
        view.setText(label);
        view.setTextSize(15);
        view.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        view.setMinHeight(dp(50));
        view.setMinimumWidth(0);
        view.setMinWidth(0);
        view.setPadding(dp(18), dp(12), dp(18), dp(12));
        view.setTextColor(primary ? (dark ? color("#08291D") : Color.WHITE) : ink);
        clickable(view, primary ? accent : soft, 14, callback);
        return view;
    }
    private Button button(LinearLayout parent, String title, boolean primary, Runnable callback) {
        Button view = action(title, primary, callback);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(10);
        parent.addView(view, lp);
        return view;
    }
    private LinearLayout card(LinearLayout parent) {
        LinearLayout card = column();
        card.setPadding(dp(18), dp(18), dp(18), dp(18));
        card.setBackground(shape(surface, 22, true));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(16);
        parent.addView(card, lp);
        if (android.animation.ValueAnimator.areAnimatorsEnabled()) {
            card.setAlpha(0);
            card.setTranslationY(dp(12));
            card.animate()
                .alpha(1)
                .translationY(0)
                .setStartDelay(Math.min(160, parent.getChildCount() * 18))
                .setDuration(260)
                .start();
        }
        return card;
    }
    private void heading(LinearLayout parent, String title, String subtitle) {
        gap(parent, 18);
        parent.addView(text(title, 31, ink, true));
        gap(parent, 8);
        if (!subtitle.isEmpty())
            parent.addView(text(subtitle, 14, muted, false));
    }
    private ImageView icon(int resource, int tint, int size) {
        ImageView view = new ImageView(this);
        view.setImageResource(resource);
        view.setImageTintList(ColorStateList.valueOf(tint));
        view.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        view.setLayoutParams(new LinearLayout.LayoutParams(dp(size), dp(size)));
        return view;
    }
    private void home() {
        getWindow().getDecorView().setSystemUiVisibility(dark
                ? 0
                : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        getWindow().setStatusBarColor(background);
        getWindow().setNavigationBarColor(surface);
        if (android.os.Build.VERSION.SDK_INT >= 30)
            getWindow().getInsetsController().show(WindowInsets.Type.systemBars());
        LinearLayout root = column();
        root.setBackgroundColor(background);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            } else
                view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                    insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        LinearLayout brand = row();
        brand.setPadding(dp(22), dp(16), dp(22), dp(8));
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.ic_brand);
        brand.addView(logo, new LinearLayout.LayoutParams(dp(32), dp(32)));
        TextView wordmark = text("AnyiOS / AnyiOSEMU", 19, ink, true);
        LinearLayout.LayoutParams wordmarkLp = new LinearLayout.LayoutParams(0, -2, 1);
        wordmarkLp.leftMargin = dp(10);
        brand.addView(wordmark, wordmarkLp);
        TextView preview = text("PREVIEW", 10, muted, true);
        preview.setLetterSpacing(0.08f);
        preview.setPadding(dp(10), dp(7), dp(10), dp(7));
        preview.setBackground(shape(soft, 8, false));
        brand.addView(preview);
        root.addView(brand);
        brand.setContentDescription("AnyiOS AnyiOSEMU. Tap to return to module selection.");
        brand.setFocusable(true);
        brand.setOnClickListener(v -> finish());
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        LinearLayout content = column();
        content.setPadding(dp(22), 0, dp(22), dp(28));
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        progressLabel = null;
        if (tab.equals("Library"))
            libraryPage(content);
        else if (tab.equals("Settings"))
            settings(content);
        else
            diagnostics(content);
        View divider = new View(this);
        divider.setBackgroundColor(line);
        root.addView(divider, new LinearLayout.LayoutParams(-1, dp(1)));
        LinearLayout navigation = row();
        navigation.setBackgroundColor(surface);
        navigation.setPadding(dp(10), dp(7), dp(10), dp(7));
        String[] titles = {"Library", "Settings", "Diagnostics"};
        int[] icons = {R.drawable.ic_library, R.drawable.ic_settings, R.drawable.ic_diagnostics};
        for (int i = 0; i < titles.length; i++) {
            String name = titles[i];
            boolean selected = name.equals(tab);
            LinearLayout item = column();
            item.setGravity(Gravity.CENTER);
            item.setPadding(dp(4), dp(8), dp(4), dp(8));
            item.addView(icon(icons[i], selected ? accent : muted, 22));
            gap(item, 5);
            item.addView(text(name, 11, selected ? accent : muted, selected));
            item.setContentDescription(name + (selected ? ", selected" : ""));
            clickable(item, selected ? soft : surface, 14, () -> {
                tab = name;
                home();
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1);
            lp.setMargins(dp(3), 0, dp(3), 0);
            navigation.addView(item, lp);
        }
        root.addView(navigation);
        renderedRevision = library.revision;
        renderedBusy = SharedCatalog.isBusy();
        renderedNotice = library.notice;
        setContentView(root);
        root.requestApplyInsets();
    }
    private void libraryPage(LinearLayout parent) {
        List<GameLibrary.Game> games = library.games();
        boolean largeText = getResources().getConfiguration().fontScale > 1.35f;
        LinearLayout titleRow = largeText ? column() : row();
        TextView title = text("Library", 31, ink, true);
        titleRow.addView(title,
            largeText ? new LinearLayout.LayoutParams(-1, -2)
                      : new LinearLayout.LayoutParams(0, -2, 1));
        if (largeText)
            gap(titleRow, 12);
        Button add = action("＋ Add game", true, this::addGame);
        add.setEnabled(!SharedCatalog.isBusy());
        add.setAlpha(SharedCatalog.isBusy() ? 0.5f : 1);
        titleRow.addView(add);
        gap(parent, 18);
        parent.addView(titleRow);
        gap(parent, 8);
        parent.addView(text(games.isEmpty() ? "Your games, together in one place."
                                            : games.size()
                    + (games.size() == 1 ? " game" : " games") + " · stored on this device",
            14, muted, false));
        operationState(parent);
        if (games.isEmpty()) {
            LinearLayout empty = card(parent);
            empty.setPadding(dp(24), dp(34), dp(24), dp(28));
            empty.setGravity(Gravity.CENTER_HORIZONTAL);
            FrameLayout tile = new FrameLayout(this);
            tile.setBackground(shape(soft, 26, false));
            ImageView mark = new ImageView(this);
            mark.setImageResource(R.drawable.ic_brand);
            FrameLayout.LayoutParams markLp =
                new FrameLayout.LayoutParams(dp(58), dp(58), Gravity.CENTER);
            tile.addView(mark, markLp);
            empty.addView(tile, new LinearLayout.LayoutParams(dp(100), dp(100)));
            gap(empty, 24);
            TextView intro = text("A home for your games", 23, ink, true);
            intro.setGravity(Gravity.CENTER);
            empty.addView(intro);
            gap(empty, 12);
            TextView explanation = text(
                "Import an IPA, a folder of separate IPAs, or an extracted app bundle. We keep a private copy and read its app details. Unsupported or broken games stay visible with an explanation.",
                15, muted, false);
            explanation.setGravity(Gravity.CENTER);
            empty.addView(explanation);
            gap(empty, 16);
            Button button = action("Add your first game", true, this::addGame);
            button.setEnabled(!SharedCatalog.isBusy());
            empty.addView(button, new LinearLayout.LayoutParams(-1, -2));
        } else {
            gap(parent, 18);
            // Emulator-style grid: three tiles per row on phones, four on tablets, and a
            // readable single column only when the window or text scale demands it.
            int width = getResources().getConfiguration().screenWidthDp;
            int columns = width < 320 || getResources().getConfiguration().fontScale > 1.45f ? 1
                : width >= 840                                                               ? 4
                                                                                             : 3;
            for (int index = 0; index < games.size(); index += columns) {
                LinearLayout grid = row();
                grid.setGravity(Gravity.TOP);
                for (int column = 0; column < columns; column++) {
                    int position = index + column;
                    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1);
                    lp.setMargins(
                        column == 0 ? 0 : dp(4), 0, column == columns - 1 ? 0 : dp(4), dp(10));
                    if (position < games.size())
                        grid.addView(gameCard(games.get(position)), lp);
                    else
                        grid.addView(new View(this), lp);
                }
                parent.addView(grid);
            }
        }
        LinearLayout status = card(parent);
        status.addView(text("Experimental AnyiOSEMU runtime", 14, ink, true));
        gap(status, 7);
        status.addView(text(
            "AnyiOSEMU runs ARM32 iPhone OS apps through a high-level compatibility runtime, SDL touch input and the bundled ANGLE driver. Universal ARM32+ARM64 bundles launch through ARM32; ARM64-only apps are unsupported. Compatibility and visible rendering must be checked on the actual device—an APK build or clean exit is not proof of gameplay.",
            13, muted, false));
        File[] legacy = new File(getFilesDir(), "imports").listFiles();
        if (legacy != null && legacy.length > 0) {
            LinearLayout old = card(parent);
            old.addView(text("Earlier imports", 16, ink, true));
            gap(old, 6);
            old.addView(text(
                "Your previous inspection copies are still here. Add them to the new library without changing the originals.",
                13, muted, false));
            for (File file : legacy)
                if (file.isFile())
                    button(old, file.getName(), false, () -> library.importLegacy(file))
                        .setEnabled(!SharedCatalog.isBusy());
        }
    }
    private void operationState(LinearLayout parent) {
        if (library.busy) {
            LinearLayout progress = card(parent);
            LinearLayout header = row();
            ProgressBar spinner = new ProgressBar(this);
            header.addView(spinner, new LinearLayout.LayoutParams(dp(24), dp(24)));
            progressLabel =
                text(library.progress.isEmpty() ? "Opening your library…" : library.progress, 14,
                    ink, false);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1);
            lp.leftMargin = dp(14);
            header.addView(progressLabel, lp);
            progress.addView(header);
            if (library.cancellable)
                button(progress, "Cancel operation", false, library::cancel);
        }
        if (!library.notice.isEmpty()) {
            LinearLayout notice = card(parent);
            notice.addView(text(library.error ? "Couldn’t finish that action" : "Library updated",
                15, library.error ? color(dark ? "#FFB4AB" : "#B3261E") : ink, true));
            gap(notice, 6);
            notice.addView(text(library.notice, 14, muted, false));
            button(notice, "Dismiss", false, library::dismissNotice);
        }
    }
    private View gameCard(GameLibrary.Game game) {
        LinearLayout card = column();
        card.setPadding(dp(9), dp(9), dp(9), dp(11));
        clickable(card, surface, 18, () -> gameDetails(game));
        card.setContentDescription(game.title + ", " + game.architecture + ", " + game.status
            + (game.canLaunchInAnyiOSEMU() ? ", open details to launch in AnyiOSEMU"
                                           : ", game launch unavailable"));
        FrameLayout artwork = new FrameLayout(this);
        GradientDrawable artworkBackdrop = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[] {color(dark ? "#174C38" : "#CAF2DD"), color(dark ? "#15342C" : "#E4F4BC")});
        artworkBackdrop.setCornerRadius(dp(16));
        artwork.setBackground(artworkBackdrop);
        artwork.setClipToOutline(true);
        TextView letter = text(game.title.substring(0, game.title.offsetByCodePoints(0, 1))
                                   .toUpperCase(java.util.Locale.ROOT),
            30, accent, true);
        letter.setGravity(Gravity.CENTER);
        artwork.addView(letter, new FrameLayout.LayoutParams(-1, -1));
        if (game.cover().isFile()) {
            ImageView image = new ImageView(this);
            image.setImageURI(Uri.fromFile(game.cover()));
            image.setScaleType(ImageView.ScaleType.CENTER_CROP);
            if (game.kind.equals("IPA")) {
                image.setScaleX(2f);
                image.setScaleY(2f);
            }
            artwork.addView(image, new FrameLayout.LayoutParams(-1, -1));
        }
        card.addView(artwork, new LinearLayout.LayoutParams(-1, dp(92)));
        gap(card, 8);
        TextView title = text(game.title, 13, ink, true);
        title.setMaxLines(2);
        title.setEllipsize(TextUtils.TruncateAt.END);
        title.setMinHeight(dp(32));
        card.addView(title);
        gap(card, 5);
        TextView subtitle = text(game.architecture + " · " + game.kind, 9, muted, false);
        subtitle.setMaxLines(1);
        subtitle.setEllipsize(TextUtils.TruncateAt.END);
        card.addView(subtitle);
        gap(card, 6);
        // The chip states what the runtime knows, never a compatibility score.
        String chip = !game.canLaunchInAnyiOSEMU()
            ? game.status.equals("Encrypted") ? "Encrypted" : game.status
            : "ARM32 · experimental";
        TextView status = text(chip, 9, accent, true);
        status.setPadding(dp(7), dp(4), dp(7), dp(4));
        status.setBackground(shape(soft, 6, false));
        status.setMaxLines(1);
        status.setEllipsize(TextUtils.TruncateAt.END);
        card.addView(status);
        return card;
    }
    private void addGame() {
        if (library.busy)
            return;
        Dialog sheet = new Dialog(this);
        LinearLayout content = column();
        content.setPadding(dp(24), dp(26), dp(24), dp(28));
        content.setBackground(shape(surface, 26, false));
        content.addView(text("Add to your library", 25, ink, true));
        gap(content, 10);
        content.addView(text(
            "Choose your legally obtained game files. Originals stay untouched; everything is copied into private app storage.",
            14, muted, false));
        gap(content, 10);
        button(content, "Import IPA file(s)", true, () -> {
            sheet.dismiss();
            selectFile();
        });
        button(content, "Import folder of IPAs", false, () -> {
            sheet.dismiss();
            selectFolder(IMPORT_FOLDER);
        });
        button(content, "Import extracted .app folder", false, () -> {
            sheet.dismiss();
            selectFolder(IMPORT_APP_FOLDER);
        });
        button(content, "Import executable for inspection", false, () -> {
            sheet.dismiss();
            selectFile();
        });
        gap(content, 14);
        content.addView(text(
            "Choose one or more IPA files, a folder containing IPAs, or the exact extracted .app folder. Originals stay untouched. IPA resources are read on demand; writable game data stays in a separate per-game sandbox. Executable-only imports are for inspection and cannot launch. AnyiOSEMU is experimental—not Apple's full iOS Simulator or a native game port.",
            12, muted, false));
        sheet.setContentView(content);
        sheet.show();
        sheet.getWindow().setBackgroundDrawableResource(android.R.color.transparent);
        sheet.getWindow().setGravity(Gravity.BOTTOM);
        sheet.getWindow().setLayout(-1, -2);
    }
    private void selectFile() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                                   .setType("*/*")
                                   .addCategory(Intent.CATEGORY_OPENABLE)
                                   .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                                   .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION),
            IMPORT_FILE);
    }
    private void selectFolder(int request) {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                                   .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                       | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),
            request);
    }
    private void gameDetails(GameLibrary.Game game) {
        Dialog sheet = new Dialog(this);
        ScrollView scroll = new ScrollView(this);
        LinearLayout content = column();
        content.setPadding(dp(24), dp(26), dp(24), dp(28));
        content.setBackground(shape(surface, 26, false));
        scroll.addView(content);
        content.addView(text(game.title, 25, ink, true));
        gap(content, 8);
        content.addView(text(game.version, 14, muted, false));
        gap(content, 20);
        content.addView(text(game.kind + "  ·  " + game.architecture + "\n"
                + GameLibrary.formatBytes(game.bytes) + " stored privately"
                + (game.bundleId.isEmpty() ? "" : "\n" + game.bundleId),
            13, muted, false));
        gap(content, 18);
        content.addView(text(!game.canLaunchInAnyiOSEMU() ? game.status.equals("Encrypted")
                    ? "Encrypted executable"
                    : "Cannot launch this import"
                                                          : "AnyiOSEMU · experimental launch",
            17, ink, true));
        gap(content, 8);
        content.addView(text(!game.blockReason.isEmpty() ? game.blockReason
                : game.status.equals("Encrypted")
                ? "The game was added, but its executable is protected. AnyiOSEMU does not bypass DRM."
                : game.kind.equals("Executable")
                ? "This executable-only import is retained for inspection, but AnyiOSEMU needs a complete IPA or extracted .app bundle."
                : game.kind.equals("IPA")
                ? "AnyiOSEMU launches the original IPA and chooses its validated ARM32 slice. Universal ARM32+ARM64 apps run through ARM32; ARM64-only apps are unsupported. The Android activity handles touch input, and bundled ANGLE is the default graphics driver. Missing iOS services, DRM or a device-specific graphics issue can still stop startup or leave the display blank; a successful build or exit does not prove gameplay."
                : "AnyiOSEMU launches this private extracted .app copy using its ARM32 executable. Touch input is handled by the runtime, and bundled ANGLE is the default graphics driver. Persistent runtime data stays isolated in this game's sandbox and is included in sandbox exports. Missing iOS services, DRM or a device-specific graphics issue can still stop startup or leave the display blank.",
            14, muted, false));
        gap(content, 16);
        content.addView(text("GAME SANDBOX", 11, muted, true));
        gap(content, 6);
        TextView sandboxInfo = text("sandbox/Documents · legacy/portable game saves\n"
                + "sandbox/BundleOverrides · IPA app-resource writes\n"
                + "sandbox/ZipCache · rebuildable IPA resource cache (not exported)\n"
                + "sandbox/AnyiOSEMU/AnyiOSEMU_sandbox/<bundle ID> · AnyiOSEMU Documents, Library and saves\n"
                + "sandbox/AnyiOSEMU/AnyiOSEMU_log.txt · latest runtime diagnostics\n"
                + "AnyiOSEMU's rebuildable PVRTC cache is excluded from exports. Extracted .app bundle/ data is included when applicable.",
            12, muted, false);
        sandboxInfo.setTextIsSelectable(true);
        content.addView(sandboxInfo);
        Button launch = action("Launch in AnyiOSEMU · experimental", true, () -> {
            sheet.dismiss();
            launchAnyiOSEMU(game);
        });
        launch.setEnabled(game.canLaunchInAnyiOSEMU() && !SharedCatalog.isBusy());
        LinearLayout.LayoutParams launchLp = new LinearLayout.LayoutParams(-1, -2);
        launchLp.topMargin = dp(18);
        content.addView(launch, launchLp);
        if (!game.launchReport.isEmpty()) {
            button(content, "Latest AnyiOSEMU session report", false, () -> {
                TextView output = text(game.launchReport, 12, ink, false);
                output.setTextIsSelectable(true);
                output.setPadding(dp(20), dp(16), dp(20), dp(16));
                ScrollView reportScroll = new ScrollView(this);
                reportScroll.addView(output);
                new AlertDialog.Builder(this)
                    .setTitle("AnyiOSEMU session report")
                    .setView(reportScroll)
                    .setPositiveButton("Done", null)
                    .setNeutralButton("Export", (dialog, which) -> exportReport(game))
                    .show();
            });
        }
        button(content, "View import report", false, () -> showReport(game));
        button(content, "Export report", false, () -> exportReport(game));
        button(content, "Export sandbox data", false, () -> exportSandbox(game));
        button(content, "Refresh details & artwork", false, () -> {
            sheet.dismiss();
            library.refreshGame(game);
        }).setEnabled(!SharedCatalog.isBusy());
        button(content, "Rename", false, () -> {
            if (library.busy)
                return;
            EditText input = new EditText(this);
            input.setText(game.title);
            input.setSingleLine(true);
            input.setSelectAllOnFocus(true);
            input.setFilters(
                new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(120)});
            new AlertDialog.Builder(this)
                .setTitle("Rename game")
                .setView(input)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Save",
                    (dialog, which) -> {
                        library.rename(game, input.getText().toString());
                        sheet.dismiss();
                    })
                .show();
        }).setEnabled(!SharedCatalog.isBusy());
        button(content, "Remove local copy", false, () -> {
            if (library.busy)
                return;
            new AlertDialog.Builder(this)
                .setTitle("Remove " + game.title + "?")
                .setMessage(
                    "This removes the shared entry from BOTH Emulator and Converter, including local outputs. Your original IPA or folder is not changed.")
                .setNegativeButton("Keep game", null)
                .setPositiveButton("Remove",
                    (dialog, which) -> {
                        library.remove(game);
                        sheet.dismiss();
                    })
                .show();
        }).setEnabled(!SharedCatalog.isBusy());
        sheet.setContentView(scroll);
        sheet.show();
        sheet.getWindow().setBackgroundDrawableResource(android.R.color.transparent);
        sheet.getWindow().setGravity(Gravity.BOTTOM);
        sheet.getWindow().setLayout(-1, -2);
    }
    private void launchAnyiOSEMU(GameLibrary.Game game) {
        if (!game.canLaunchInAnyiOSEMU())
            return;
        java.lang.ref.WeakReference<MainActivity> owner = new java.lang.ref.WeakReference<>(this);
        library.prepareAnyiOSEmu(game, source -> {
            MainActivity activity = owner.get();
            if (activity == null || activity.isFinishing() || activity.isDestroyed())
                return;
            if (source == null) {
                String message = activity.library.notice.isEmpty()
                    ? "Emulator could not prepare this game. Re-import the IPA or extracted app folder and try again."
                    : activity.library.notice;
                if (message.startsWith("Emulator preparation cancelled.")) {
                    android.widget.Toast
                        .makeText(activity, message, android.widget.Toast.LENGTH_SHORT)
                        .show();
                } else {
                    activity.showPreparationError(message);
                }
                return;
            }
            Intent intent =
                new Intent(activity, AnyiOSEmuActivity.class)
                    .putExtra(AnyiOSEmuActivity.EXTRA_GAME_ID, game.id)
                    .putExtra(AnyiOSEmuActivity.EXTRA_GAME_TITLE, game.title)
                    .putExtra(AnyiOSEmuActivity.EXTRA_SOURCE_PATH, source.getAbsolutePath())
                    .putExtra(AnyiOSEmuActivity.EXTRA_DATA_PATH,
                        game.anyioseDataDirectory().getAbsolutePath())
                    .putExtra(AnyiOSEmuActivity.EXTRA_SHOW_CONSOLE,
                        activity.prefs.getBoolean("showConsole", false));
            try {
                activity.activeAnyiOSEmuGameId = game.id;
                activity.startActivityForResult(intent, RUN_ANYIOSEMU);
                activity.library.recordAnyiOSEmuReport(game.id,
                    "AnyiOSEMU session outcome pending; the runtime log will be captured on exit.");
            } catch (Exception e) {
                activity.activeAnyiOSEmuGameId = "";
                Diagnostics.record("ANYIOSEMU_START_ERROR", e.toString());
                activity.showPreparationError("Could not open the Emulator activity: " + e);
            }
        });
    }
    private void showPreparationError(String message) {
        String fullDetails = TextUtils.isEmpty(message)
            ? "The game could not be prepared. Re-import the IPA or extracted app folder and try again."
            : message;
        final String details = fullDetails.length() > 12000
            ? fullDetails.substring(0, 12000) + "\nDetails shortened."
            : fullDetails;
        TextView output = text(details, 13, ink, false);
        output.setTypeface(Typeface.MONOSPACE);
        output.setTextIsSelectable(true);
        output.setPadding(dp(18), dp(14), dp(18), dp(14));
        ScrollView scroll = new ScrollView(this);
        scroll.addView(output);
        scroll.setLayoutParams(new android.view.ViewGroup.LayoutParams(-1, dp(300)));
        new AlertDialog.Builder(this)
            .setTitle("Couldn’t open Emulator")
            .setView(scroll)
            .setNegativeButton("Copy details",
                (dialog, which) -> {
                    android.content.ClipboardManager clipboard =
                        getSystemService(android.content.ClipboardManager.class);
                    if (clipboard != null) {
                        clipboard.setPrimaryClip(
                            ClipData.newPlainText("AnyiOS emulator error", details));
                        android.widget.Toast
                            .makeText(
                                this, "Error details copied", android.widget.Toast.LENGTH_SHORT)
                            .show();
                    }
                })
            .setPositiveButton("Done", null)
            .show();
    }
    static List<Uri> selectedFiles(Intent data) {
        LinkedHashSet<Uri> selected = new LinkedHashSet<>();
        ClipData clip = data.getClipData();
        if (clip != null)
            for (int i = 0; i < clip.getItemCount(); i++)
                if (clip.getItemAt(i).getUri() != null)
                    selected.add(clip.getItemAt(i).getUri());
        if (data.getData() != null)
            selected.add(data.getData());
        return new ArrayList<>(selected);
    }
    private void showReport(GameLibrary.Game game) {
        // Older library entries keep their persisted report across upgrades. Rebuild stale
        // static-import reports before showing them so implemented bindings are not presented
        // as unresolved merely because the report predates the runtime export filter.
        if (game.report.contains("STATIC_IMPORT_EVIDENCE:")
            && !game.report.contains("IMPORT_REPORT_FILTER: nlist ordinals")) {
            new AlertDialog.Builder(this)
                .setTitle("Import report needs refreshing")
                .setMessage("This saved static import list predates the current runtime-binding "
                    + "filter. Refresh it before viewing so working imports are not misreported.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Refresh report",
                    (dialog, which)
                        -> library.refreshGame(game,
                            () -> {
                                if (isFinishing() || isDestroyed())
                                    return;
                                GameLibrary.Game refreshed = null;
                                for (GameLibrary.Game candidate : library.games())
                                    if (candidate.id.equals(game.id)) {
                                        refreshed = candidate;
                                        break;
                                    }
                                if (refreshed != null
                                    && refreshed.report.contains(
                                        "IMPORT_REPORT_FILTER: nlist ordinals")) {
                                    showReport(refreshed);
                                } else {
                                    new AlertDialog.Builder(this)
                                        .setTitle("Import report not refreshed")
                                        .setMessage("The current static import report could not be "
                                            + "rebuilt. No stale unresolved-import list was shown. See "
                                            + "the latest launch report or Diagnostics for the blocker.")
                                        .setPositiveButton("Done", null)
                                        .show();
                                }
                            }))
                .show();
            return;
        }
        TextView report = text(game.report.length() > 20000 ? game.report.substring(0, 20000)
                    + "\n\nPreview shortened. Export to read the complete report."
                                                            : game.report,
            12, ink, false);
        report.setTypeface(Typeface.MONOSPACE);
        report.setTextIsSelectable(true);
        report.setPadding(dp(20), dp(16), dp(20), dp(16));
        ScrollView scroll = new ScrollView(this);
        scroll.addView(report);
        new AlertDialog.Builder(this)
            .setTitle("Import report")
            .setView(scroll)
            .setPositiveButton("Done", null)
            .setNeutralButton("Export", (dialog, which) -> exportReport(game))
            .show();
    }
    private void exportReport(GameLibrary.Game game) {
        exportGame = game == null ? "" : game.id;
        startActivityForResult(
            new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .setType("text/plain")
                .addCategory(Intent.CATEGORY_OPENABLE)
                .putExtra(Intent.EXTRA_TITLE,
                    game == null ? "AnyiOS-diagnostics.txt" : "AnyiOS-game-report.txt"),
            EXPORT);
    }
    private void exportSandbox(GameLibrary.Game game) {
        exportGame = game.id;
        String safeTitle = game.title.replaceAll("[^A-Za-z0-9._-]+", "_");
        startActivityForResult(
            new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .setType("application/zip")
                .addCategory(Intent.CATEGORY_OPENABLE)
                .putExtra(Intent.EXTRA_TITLE, "AnyiOS-" + safeTitle + "-sandbox.zip"),
            EXPORT_SANDBOX);
    }
    private void settingRow(
        LinearLayout parent, String title, String description, String value, Runnable callback) {
        LinearLayout container = row();
        container.setPadding(0, dp(15), 0, dp(15));
        LinearLayout labels = column();
        labels.addView(text(title, 15, ink, true));
        if (!description.isEmpty()) {
            gap(labels, 5);
            labels.addView(text(description, 12, muted, false));
        }
        container.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
        TextView current = text(value + "  ›", 13, accent, true);
        current.setPadding(dp(12), 0, 0, 0);
        container.addView(current);
        clickable(container, surface, 10, callback);
        container.setContentDescription(title + ", " + value);
        parent.addView(container);
    }
    private void settings(LinearLayout parent) {
        heading(parent, "Settings", "Simple controls. Clear limits.");
        LinearLayout display = card(parent);
        display.addView(text("DISPLAY", 11, muted, true));
        settingRow(display, "Resolution", "Short edge; never upscales the surface",
            prefs.getInt("resolution", 1080) + "p",
            ()
                -> choose("Render resolution", new String[] {"720p", "1080p · FHD", "1440p · QHD"},
                    new int[] {720, 1080, 1440}, "resolution", 1080));
        settingRow(display, "Frame limit", "Passed to AnyiOSEMU; not a displayed-FPS guarantee",
            prefs.getInt("fps", 60) + " FPS",
            ()
                -> choose("Frame limit",
                    new String[] {"30 FPS", "60 FPS", "120 FPS · experimental"},
                    new int[] {30, 60, 120}, "fps", 60));
        settingRow(display, "Session badge", "Brief AnyiOSEMU overlay at launch",
            prefs.getBoolean("showConsole", false) ? "Shown" : "Hidden", this::toggleConsole);
        // Detailed GLES call logging is useful for black-screen investigations, but can
        // measurably slow the guest. Keep it opt-in.
        settingRow(display, "Verbose logging",
            "Add detailed GLES calls to AnyiOSEMU logs; may reduce speed",
            prefs.getBoolean("verboseLogging", false) ? "On" : "Off", this::toggleVerbose);
        String[] appearances = {"Light", "Dark"};
        settingRow(display, "Appearance", "Choose your preferred look", dark ? "Dark" : "Light",
            () -> choose("Appearance", appearances, new int[] {1, 2}, "theme", dark ? 2 : 1));
        display.addView(text(
            "Frame cap, graphics driver and composition options apply on the next AnyiOSEMU launch; render resolution is for the ANGLE surface test.",
            12, muted, false));
        LinearLayout graphics = card(parent);
        graphics.addView(text("GRAPHICS", 11, muted, true));
        gap(graphics, 13);
        graphics.addView(text("Bundled ANGLE · AnyiOSEMU graphics", 17, ink, true));
        gap(graphics, 7);
        graphics.addView(text(
            "AnyiOSEMU and the ANGLE surface test use real, source-built Google ANGLE at the pinned revision. The test requests Vulkan explicitly; AnyiOSEMU uses ANGLE by default and records any native-driver fallback in its session log. No root access, developer options or system driver changes are involved. Build success is not device-rendering proof.",
            13, muted, false));
        gap(graphics, 7);
        // Prefer the pinned driver by default. Native GLES is an explicit alternate path for
        // games that need device-specific behavior; initialization failures remain visible.
        settingRow(graphics, "Native OpenGL ES",
            "Use the device driver instead of bundled ANGLE; failures stay explicit",
            prefs.getBoolean("nativeGles", false) ? "On" : "Off", this::toggleNativeGles);
        settingRow(graphics, "Force Composition",
            "Route EAGL frames through the compositor; may fix display issues but can cost speed",
            prefs.getBoolean("forceComposition", false) ? "On" : "Off",
            this::toggleForceComposition);
        button(graphics, "Run ANGLE surface test", true, this::launch);
        LinearLayout storage = card(parent);
        storage.addView(text("ON THIS DEVICE", 11, muted, true));
        gap(storage, 12);
        long bytes = 0;
        for (GameLibrary.Game game : library.games()) bytes += game.bytes;
        storage.addView(text(library.games().size() + " games · " + GameLibrary.formatBytes(bytes)
                + " imported files",
            16, ink, true));
        gap(storage, 8);
        storage.addView(text(
            "Each game keeps legacy Documents, IPA bundle overrides and AnyiOSEMU's separate per-app Documents/Library data. Runtime logs are included in sandbox exports; rebuildable IPA and PVRTC caches are excluded. Originals stay private and untouched. Remove a game from its details to free the local copy.",
            13, muted, false));
        LinearLayout about = card(parent);
        about.addView(text("AnyiOS " + BuildConfig.VERSION_NAME, 16, ink, true));
        gap(about, 9);
        about.addView(text(
            "AnyiOSEMU is an experimental ARM32 iPhone OS app runtime. Universal ARM32+ARM64 bundles use their ARM32 slice; ARM64-only apps are unsupported. Compatibility, rendering and performance vary by app and device and require real-device testing.",
            13, muted, false));
        button(about, "Runtime licenses & notices", false, this::showRuntimeNotices);
        gap(about, 10);
        about.addView(text("SIGNING CERTIFICATE SHA-256", 10, muted, true));
        gap(about, 4);
        TextView fingerprint = text(signatureFingerprint(), 10, muted, false);
        fingerprint.setTypeface(Typeface.MONOSPACE);
        fingerprint.setTextIsSelectable(true);
        about.addView(fingerprint);
        gap(about, 8);
        about.addView(text(
            "Android's installer may warn that a self-built APK is unsafe. That dialog comes from Play Protect for any app installed outside Play, not from this app. Compare this fingerprint and the artifact's SHA-256 before installing, then choose \"install anyway\".",
            12, muted, false));
        button(about, "Reset display & appearance", false,
            ()
                -> new AlertDialog.Builder(this)
                    .setTitle("Reset settings?")
                    .setMessage("Imported games are kept.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Reset",
                        (dialog, which) -> {
                            prefs.edit()
                                .remove("resolution")
                                .remove("fps")
                                .remove("theme")
                                .remove("nativeGles")
                                .remove("forceComposition")
                                .remove("showConsole")
                                .remove("verboseLogging")
                                .apply();
                            recreate();
                        })
                    .show());
    }
    private void choose(String title, String[] labels, int[] values, String key, int fallback) {
        int selected = 0;
        for (int i = 0; i < values.length; i++)
            if (values[i] == (key.equals("theme") ? (dark ? 2 : 1) : prefs.getInt(key, fallback)))
                selected = i;
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setSingleChoiceItems(labels, selected,
                (dialog, which) -> {
                    prefs.edit().putInt(key, values[which]).apply();
                    dialog.dismiss();
                    if (key.equals("theme")) {
                        boolean light = values[which] == 1
                            || values[which] == 0
                                && (getResources().getConfiguration().uiMode
                                       & Configuration.UI_MODE_NIGHT_MASK)
                                    != Configuration.UI_MODE_NIGHT_YES;
                        getSharedPreferences("radek_settings", MODE_PRIVATE)
                            .edit()
                            .putBoolean("light_theme", light)
                            .apply();
                        recreate();
                    } else
                        home();
                })
            .setNegativeButton("Cancel", null)
            .show();
    }
    /** The optional session badge is brief; detailed runtime logs remain in the saved report. */
    private void toggleConsole() {
        new AlertDialog.Builder(this)
            .setTitle("Session badge")
            .setMessage(
                "Show a brief AnyiOSEMU badge over the game at launch? It fades after five seconds; the display stays clean afterward and the runtime report is saved either way.")
            .setNegativeButton("Hidden",
                (dialog, which) -> {
                    prefs.edit().putBoolean("showConsole", false).apply();
                    home();
                })
            .setPositiveButton("Shown",
                (dialog, which) -> {
                    prefs.edit().putBoolean("showConsole", true).apply();
                    home();
                })
            .show();
    }
    /**
     * Detailed GLES call logging is opt-in and can reduce performance; it does not change game
     * behavior.
     */
    private void toggleVerbose() {
        boolean on = !prefs.getBoolean("verboseLogging", false);
        prefs.edit().putBoolean("verboseLogging", on).apply();
        NativeBridge.setVerbose(on);
        Diagnostics.record("Settings",
            on ? "Detailed GLES logging enabled for the next AnyiOSEMU run; it may reduce speed."
               : "Detailed GLES logging disabled.");
        home();
    }
    /** The default is the pinned bundled ANGLE stack; native GLES is an explicit alternative. */
    private void toggleNativeGles() {
        boolean on = !prefs.getBoolean("nativeGles", false);
        prefs.edit().putBoolean("nativeGles", on).apply();
        NativeBridge.setNativeGles(on);
        Diagnostics.record("Settings",
            on ? "Native OpenGL ES enabled for the next AnyiOSEMU/graphics-test launch."
               : "Bundled ANGLE selected for the next AnyiOSEMU/graphics-test launch.");
        home();
    }
    private void toggleForceComposition() {
        boolean on = !prefs.getBoolean("forceComposition", false);
        prefs.edit().putBoolean("forceComposition", on).apply();
        Diagnostics.record("Settings",
            on ? "Force Composition enabled for the next AnyiOSEMU launch; it may reduce speed."
               : "Force Composition disabled for the next AnyiOSEMU launch.");
        home();
    }
    private void diagnostics(LinearLayout parent) {
        heading(parent, "Diagnostics", "Useful evidence, without uploading your games.");
        operationState(parent);
        LinearLayout tools = card(parent);
        tools.addView(text("Graphics & runtime", 18, ink, true));
        gap(tools, 8);
        tools.addView(text(
            "Check the active backend with the 3D test. Export logs after a failure, or export a game’s import report from its details.",
            14, muted, false));
        button(tools, "Export diagnostic log", true, () -> exportReport(null));
        button(tools, "Run graphics test", false, this::launch);
        button(tools, "Run ARM/Thumb CPU self-test", false, library::runCpuProbe)
            .setEnabled(!SharedCatalog.isBusy());
        String backend = prefs.getString("lastBackend", "");
        if (!backend.isEmpty()) {
            LinearLayout renderer = card(parent);
            renderer.addView(text("Last initialized backend", 15, ink, true));
            gap(renderer, 10);
            renderer.addView(text(backend, 12, muted, false));
        }
        LinearLayout logs = card(parent);
        logs.addView(text("Recent events", 16, ink, true));
        gap(logs, 12);
        String report = Diagnostics.export();
        if (report.length() > 14000)
            report = "… earlier events omitted from this preview …\n"
                + report.substring(report.length() - 14000);
        TextView output = text(report, 11, muted, false);
        output.setTypeface(Typeface.MONOSPACE);
        output.setTextIsSelectable(true);
        logs.addView(output);
        gap(parent, 14);
        parent.addView(text(
            "Reports can contain app names, imported symbols and device information. Review them before sharing. Proprietary game files are never included.",
            12, muted, false));
    }
    /** Full-screen, rotation-aware window used by the standalone ANGLE surface test. */
    private void enterEmulatorDisplay() {
        // The surface test is full-screen in landscape so its backend output can be inspected.
        if (!landscapeRequested) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            landscapeRequested = true;
        }
        android.view.Window window = getWindow();
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }
    private void leaveEmulatorDisplay() {
        if (landscapeRequested) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            landscapeRequested = false;
        }
        android.view.Window window = getWindow();
        window.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.show(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_DEFAULT);
            }
        }
        window.getDecorView().setSystemUiVisibility(dark
                ? 0
                : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
    }
    @Override
    public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        // Rotation lands here because the manifest declares configChanges. Re-assert the
        // immersive window for the ANGLE graphics test; the guest runtime has its own activity.
        if (prototype != null)
            enterEmulatorDisplay();
        else
            home();
    }
    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused && prototype != null)
            enterEmulatorDisplay();
    }
    private void launch() {
        LinearLayout root = column();
        root.setBackgroundColor(color("#10141D"));
        root.setPadding(dp(12), dp(10), dp(12), 0);
        Button back = action("‹  Stop graphics test", false, this::stop);
        root.addView(back, new LinearLayout.LayoutParams(-1, -2));
        liveStatus = text("Initializing embedded ANGLE…", 11, color("#CCD6EC"), false);
        liveStatus.setMaxLines(3);
        liveStatus.setEllipsize(TextUtils.TruncateAt.END);
        liveStatus.setPadding(dp(4), dp(10), dp(4), dp(10));
        root.addView(liveStatus);
        prototype = new PrototypeView(
            this, prefs.getInt("fps", 60), prefs.getInt("resolution", 1080), info -> {
                if (liveStatus != null)
                    liveStatus.setText(info);
                if (info.startsWith("embedded ANGLE"))
                    prefs.edit().putString("lastBackend", info).apply();
            });
        root.addView(prototype, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);
        enterEmulatorDisplay();
    }
    private void stop() {
        leaveEmulatorDisplay();
        resumeTest = false;
        if (prototype != null) {
            prototype.halt();
            prototype = null;
        }
        liveStatus = null;
        home();
    }
    @Override
    protected void onStart() {
        super.onStart();
        library.observe(libraryChanged);
    }
    @Override
    protected void onStop() {
        library.unobserve(libraryChanged);
        super.onStop();
    }
    @Override
    protected void onPause() {
        boolean wasRunning = prototype != null;
        if (wasRunning)
            stop();
        resumeTest = wasRunning;
        Diagnostics.record("LIFECYCLE", "paused; graphics resources released if active");
        super.onPause();
    }
    @Override
    protected void onResume() {
        super.onResume();
        if (resumeTest) {
            resumeTest = false;
            launch();
        } else
            libraryChanged.run();
    }
    @Override
    protected void onSaveInstanceState(Bundle out) {
        out.putBoolean("prototype", prototype != null || resumeTest);
        out.putString("tab", tab);
        out.putString("exportGame", exportGame);
        super.onSaveInstanceState(out);
    }
    @Override
    public void onBackPressed() {
        if (prototype != null)
            stop();
        else if (!tab.equals("Library")) {
            tab = "Library";
            home();
        } else
            super.onBackPressed();
    }
    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == RUN_ANYIOSEMU) {
            String gameId = activeAnyiOSEmuGameId;
            String report = null;
            if (data != null) {
                String returnedId = data.getStringExtra(AnyiOSEmuActivity.EXTRA_GAME_ID);
                if (returnedId != null && !returnedId.isEmpty())
                    gameId = returnedId;
                report = data.getStringExtra(AnyiOSEmuActivity.EXTRA_GAME_REPORT);
            }
            activeAnyiOSEmuGameId = "";
            if (!gameId.isEmpty()) {
                if (report == null || report.isEmpty())
                    report = "AnyiOSEMU session returned without a final activity report. "
                        + "Runtime log: sandbox/AnyiOSEMU/AnyiOSEMU_log.txt. "
                        + "Visual output and gameplay remain unverified.";
                library.recordAnyiOSEmuReport(gameId, report);
            }
            return;
        }
        if (result != RESULT_OK || data == null)
            return;
        if (request == IMPORT_FILE) {
            List<Uri> selected = selectedFiles(data);
            if (selected.size() == 1)
                library.importFile(selected.get(0));
            else if (!selected.isEmpty())
                library.importFiles(selected);
            return;
        }
        if (data.getData() == null)
            return;
        Uri uri = data.getData();
        if (request == IMPORT_FOLDER || request == IMPORT_APP_FOLDER) {
            if (library.busy) {
                android.widget.Toast
                    .makeText(this, "Wait for the current library action to finish",
                        android.widget.Toast.LENGTH_LONG)
                    .show();
                return;
            }
            try {
                getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException e) {
                Diagnostics.record("STORAGE", "Using temporary folder grant");
            }
            if (request == IMPORT_FOLDER)
                library.importIpaFolder(uri);
            else
                library.importFolder(uri);
        } else if (request == EXPORT) {
            String report = Diagnostics.export();
            if (!exportGame.isEmpty()) {
                GameLibrary.Game selected = null;
                for (GameLibrary.Game game : library.games())
                    if (game.id.equals(exportGame))
                        selected = game;
                if (selected == null) {
                    new AlertDialog.Builder(this)
                        .setTitle("Game no longer available")
                        .setMessage("The game was removed before its report could be exported.")
                        .setPositiveButton("OK", null)
                        .show();
                    return;
                }
                report = "AnyiOS game import report\nApp=" + selected.title
                    + "\nVersion=" + selected.version + "\nBuild=" + BuildConfig.SOURCE_REVISION
                    + "\n\n" + selected.report
                    + (selected.launchReport.isEmpty()
                            ? ""
                            : "\n\nLATEST ANYIOSEMU SESSION\n" + selected.launchReport);
            }
            final String output = report;
            final android.content.Context app = getApplicationContext();
            new Thread(() -> {
                try (OutputStream stream = app.getContentResolver().openOutputStream(uri, "wt")) {
                    if (stream == null)
                        throw new java.io.IOException("Provider could not open the destination");
                    stream.write(output.getBytes(StandardCharsets.UTF_8));
                    Diagnostics.record("EXPORT", "Report saved");
                    new android.os.Handler(android.os.Looper.getMainLooper())
                        .post(()
                                  -> android.widget.Toast
                                      .makeText(
                                          app, "Report saved", android.widget.Toast.LENGTH_SHORT)
                                      .show());
                } catch (Exception e) {
                    Diagnostics.record("EXPORT_ERROR", e.toString());
                    new android.os.Handler(android.os.Looper.getMainLooper())
                        .post(()
                                  -> android.widget.Toast
                                      .makeText(app, "Report export failed: " + e.getMessage(),
                                          android.widget.Toast.LENGTH_LONG)
                                      .show());
                }
            }, "AnyiOS-export").start();
        } else if (request == EXPORT_SANDBOX) {
            GameLibrary.Game selected = null;
            for (GameLibrary.Game game : library.games())
                if (game.id.equals(exportGame))
                    selected = game;
            if (selected == null) {
                new AlertDialog.Builder(this)
                    .setTitle("Game no longer available")
                    .setMessage("The game was removed before its sandbox could be exported.")
                    .setPositiveButton("OK", null)
                    .show();
                return;
            }
            library.exportSandbox(selected, uri);
        }
    }
}
