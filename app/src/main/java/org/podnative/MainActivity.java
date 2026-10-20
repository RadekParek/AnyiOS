package org.podnative;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Intent;
import android.content.SharedPreferences;
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
import java.io.File;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

public final class MainActivity extends Activity {
    private static final int IMPORT_FILE = 10, EXPORT = 11, IMPORT_FOLDER = 12;
    private SharedPreferences prefs;
    private GameLibrary library;
    private PrototypeView prototype;
    private TextView liveStatus, progressLabel;
    private String tab = "Library", exportGame = "";
    private boolean resumeTest, dark, showingSplashes, resumed;
    private int presentationGeneration;
    private int background, surface, ink, muted, line, accent, soft;
    private long renderedRevision;
    private boolean renderedBusy;
    private String renderedNotice = "";
    private final Runnable libraryChanged = () -> {
        if (prototype != null || isFinishing() || isDestroyed())
            return;
        if (renderedRevision == library.revision && renderedBusy == library.busy
            && renderedNotice.equals(library.notice) && progressLabel != null)
            progressLabel.setText(library.progress);
        else
            home();
    };
    @Override
    public void onCreate(Bundle state) {
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        int mode = prefs.getInt("theme", 0);
        dark = mode == 2
            || mode == 0
                && (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                    == Configuration.UI_MODE_NIGHT_YES;
        setTheme(dark ? R.style.AppThemeDark : R.style.AppTheme);
        super.onCreate(state);
        library = GameLibrary.get(this);
        if (state != null) {
            resumeTest = state.getBoolean("prototype", false);
            tab = state.getString("tab", "Library");
            exportGame = state.getString("exportGame", "");
        }
        background = color(dark ? "#091813" : "#F2F8F4");
        surface = color(dark ? "#14271F" : "#FFFFFF");
        ink = color(dark ? "#EDF1FC" : "#192238");
        muted = color(dark ? "#A3ADC2" : "#6B758B");
        line = color(dark ? "#2B3445" : "#E6EAF2");
        accent = color(dark ? "#7AE8B5" : "#087F5B");
        soft = color(dark ? "#203E30" : "#DDF3E7");
        Diagnostics.record("APP", "library preview; game execution remains unavailable");
        home();
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
        TextView wordmark = text("PodNative", 19, ink, true);
        LinearLayout.LayoutParams wordmarkLp = new LinearLayout.LayoutParams(0, -2, 1);
        wordmarkLp.leftMargin = dp(10);
        brand.addView(wordmark, wordmarkLp);
        TextView preview = text("PREVIEW", 10, muted, true);
        preview.setLetterSpacing(0.08f);
        preview.setPadding(dp(10), dp(7), dp(10), dp(7));
        preview.setBackground(shape(soft, 8, false));
        brand.addView(preview);
        root.addView(brand);
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
        renderedBusy = library.busy;
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
        add.setEnabled(!library.busy);
        add.setAlpha(library.busy ? 0.5f : 1);
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
                "Import an IPA or an extracted game folder. We’ll keep a private copy, read its app details and select a supported ARM32 executable.",
                15, muted, false);
            explanation.setGravity(Gravity.CENTER);
            empty.addView(explanation);
            gap(empty, 16);
            Button button = action("Add your first game", true, this::addGame);
            button.setEnabled(!library.busy);
            empty.addView(button, new LinearLayout.LayoutParams(-1, -2));
        } else {
            gap(parent, 18);
            // Two comfortable columns; large text/narrow windows switch to a readable list.
            int columns = getResources().getConfiguration().screenWidthDp < 340
                    || getResources().getConfiguration().fontScale > 1.35f
                ? 1
                : getResources().getConfiguration().screenWidthDp >= 700 ? 3
                                                                         : 2;
            for (int index = 0; index < games.size(); index += columns) {
                LinearLayout grid = row();
                grid.setGravity(Gravity.TOP);
                for (int column = 0; column < columns; column++) {
                    int position = index + column;
                    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1);
                    lp.setMargins(
                        column == 0 ? 0 : dp(6), 0, column == columns - 1 ? 0 : dp(6), dp(14));
                    if (position < games.size())
                        grid.addView(gameCard(games.get(position)), lp);
                    else
                        grid.addView(new View(this), lp);
                }
                parent.addView(grid);
            }
        }
        LinearLayout status = card(parent);
        status.addView(text("Import is ready. Game execution is next.", 14, ink, true));
        gap(status, 7);
        status.addView(text(
            "This preview can organize and inspect your games. It cannot launch iOS games yet. Imported does not mean compatible.",
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
                        .setEnabled(!library.busy);
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
                button(progress, "Cancel import", false, library::cancel);
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
        card.setPadding(dp(12), dp(12), dp(12), dp(14));
        clickable(card, surface, 20, () -> gameDetails(game));
        card.setContentDescription(game.title + ", " + game.architecture + ", " + game.status
            + ", game launch unavailable");
        FrameLayout artwork = new FrameLayout(this);
        GradientDrawable artworkBackdrop = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[] {color(dark ? "#174C38" : "#CAF2DD"), color(dark ? "#15342C" : "#E4F4BC")});
        artworkBackdrop.setCornerRadius(dp(16));
        artwork.setBackground(artworkBackdrop);
        artwork.setClipToOutline(true);
        TextView letter = text(game.title.substring(0, game.title.offsetByCodePoints(0, 1))
                                   .toUpperCase(java.util.Locale.ROOT),
            42, accent, true);
        letter.setGravity(Gravity.CENTER);
        artwork.addView(letter, new FrameLayout.LayoutParams(-1, -1));
        if (game.cover().isFile()) {
            ImageView image = new ImageView(this);
            image.setImageURI(Uri.fromFile(game.cover()));
            image.setScaleType(ImageView.ScaleType.CENTER_CROP);
            artwork.addView(image, new FrameLayout.LayoutParams(-1, -1));
        }
        card.addView(artwork, new LinearLayout.LayoutParams(-1, dp(126)));
        gap(card, 12);
        TextView title = text(game.title, 16, ink, true);
        title.setMaxLines(2);
        title.setEllipsize(TextUtils.TruncateAt.END);
        title.setMinHeight(dp(40));
        card.addView(title);
        gap(card, 7);
        TextView subtitle = text(game.kind + " · " + game.architecture, 11, muted, false);
        subtitle.setMaxLines(1);
        subtitle.setEllipsize(TextUtils.TruncateAt.END);
        card.addView(subtitle);
        gap(card, 9);
        TextView status = text(!game.blockReason.isEmpty() ? game.status
                : game.status.equals("Encrypted")          ? "Encrypted executable"
                                                           : "Runtime not ready",
            10, muted, true);
        status.setPadding(dp(8), dp(5), dp(8), dp(5));
        status.setBackground(shape(soft, 7, false));
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
        button(content, "Import IPA", true, () -> {
            sheet.dismiss();
            selectFile();
        });
        button(content, "Import game folder", false, () -> {
            sheet.dismiss();
            selectFolder();
        });
        button(content, "Import executable only", false, () -> {
            sheet.dismiss();
            selectFile();
        });
        gap(content, 14);
        content.addView(text(
            "Folder import accepts an extracted .app bundle or a parent containing one app. ARM64-only games are not supported.",
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
                                   .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION),
            IMPORT_FILE);
    }
    private void selectFolder() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                                   .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                       | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),
            IMPORT_FOLDER);
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
        content.addView(text(!game.blockReason.isEmpty() ? "Cannot launch this import"
                : game.status.equals("Encrypted")        ? "Encrypted executable"
                                                         : "Game launch is not available yet",
            17, ink, true));
        gap(content, 8);
        content.addView(text(!game.blockReason.isEmpty() ? game.blockReason
                : game.status.equals("Encrypted")
                ? "The game was added, but its executable is protected. This build does not bypass DRM."
                : "Attempt launch reopens the saved executable and checks it with the native loader. It reports the first startup blocker. Guest execution and iOS frameworks are not integrated yet; no game code runs in this build.",
            14, muted, false));
        Button launch = action("Attempt launch · experimental", true, () -> {
            sheet.dismiss();
            java.lang.ref.WeakReference<MainActivity> owner =
                new java.lang.ref.WeakReference<>(this);
            library.preparePresentation(game, files -> {
                MainActivity activity = owner.get();
                if (activity != null && activity.resumed && !activity.isFinishing()
                    && !activity.isDestroyed())
                    activity.presentGameArtwork(game, files);
            });
        });
        launch.setEnabled(!library.busy);
        LinearLayout.LayoutParams launchLp = new LinearLayout.LayoutParams(-1, -2);
        launchLp.topMargin = dp(18);
        content.addView(launch, launchLp);
        if (!game.launchReport.isEmpty()) {
            button(content, "Latest launch report", false, () -> {
                TextView output = text(game.launchReport, 12, ink, false);
                output.setTextIsSelectable(true);
                output.setPadding(dp(20), dp(16), dp(20), dp(16));
                ScrollView reportScroll = new ScrollView(this);
                reportScroll.addView(output);
                new AlertDialog.Builder(this)
                    .setTitle("Launch attempt")
                    .setView(reportScroll)
                    .setPositiveButton("Done", null)
                    .setNeutralButton("Export", (dialog, which) -> exportReport(game))
                    .show();
            });
        }
        button(content, "View import report", false, () -> showReport(game));
        button(content, "Export report", false, () -> exportReport(game));
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
        }).setEnabled(!library.busy);
        button(content, "Remove local copy", false, () -> {
            if (library.busy)
                return;
            new AlertDialog.Builder(this)
                .setTitle("Remove " + game.title + "?")
                .setMessage(
                    "This removes PodNative’s private copy only. Your original IPA or game folder is not changed.")
                .setNegativeButton("Keep game", null)
                .setPositiveButton("Remove",
                    (dialog, which) -> {
                        library.remove(game);
                        sheet.dismiss();
                    })
                .show();
        }).setEnabled(!library.busy);
        sheet.setContentView(scroll);
        sheet.show();
        sheet.getWindow().setBackgroundDrawableResource(android.R.color.transparent);
        sheet.getWindow().setGravity(Gravity.BOTTOM);
        sheet.getWindow().setLayout(-1, -2);
    }
    private void showReport(GameLibrary.Game game) {
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
                    game == null ? "PodNative-diagnostics.txt" : "PodNative-game-report.txt"),
            EXPORT);
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
        settingRow(display, "Frame limit", "Vsync stays enabled; not an FPS guarantee",
            prefs.getInt("fps", 60) + " FPS",
            ()
                -> choose("Frame limit",
                    new String[] {"30 FPS", "60 FPS", "120 FPS · experimental"},
                    new int[] {30, 60, 120}, "fps", 60));
        String[] appearances = {"System", "Light", "Dark"};
        settingRow(display, "Appearance", "Choose your preferred look",
            appearances[Math.max(0, Math.min(2, prefs.getInt("theme", 0)))],
            () -> choose("Appearance", appearances, new int[] {0, 1, 2}, "theme", 0));
        display.addView(
            text("Rendering changes apply on the next graphics-test launch.", 12, muted, false));
        LinearLayout graphics = card(parent);
        graphics.addView(text("GRAPHICS", 11, muted, true));
        gap(graphics, 13);
        graphics.addView(text("Embedded ANGLE · Vulkan", 17, ink, true));
        gap(graphics, 7);
        graphics.addView(text(
            "Real upstream ANGLE is included in the app. No system driver switch, root access or untested fallback is used.",
            13, muted, false));
        button(graphics, "Run 3D graphics test", true, this::launch);
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
            "Imports include all IPA bytes or selected folder resources. Remove a game from its details to free its private copy. Limits: 2 GiB per import, 64 MiB per ARM32 slice, 64 games.",
            13, muted, false));
        LinearLayout about = card(parent);
        about.addView(text("PodNative " + BuildConfig.VERSION_NAME, 16, ink, true));
        gap(about, 9);
        about.addView(text(
            "Game execution, guest frameworks, audio and touch overlays are not available in this preview. ARM32 CPU diagnostics are developed and tested separately.",
            13, muted, false));
        button(about, "Reset display & appearance", false,
            ()
                -> new AlertDialog.Builder(this)
                    .setTitle("Reset settings?")
                    .setMessage("Imported games are kept.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Reset",
                        (dialog, which) -> {
                            prefs.edit().remove("resolution").remove("fps").remove("theme").apply();
                            recreate();
                        })
                    .show());
    }
    private void choose(String title, String[] labels, int[] values, String key, int fallback) {
        int selected = 0;
        for (int i = 0; i < values.length; i++)
            if (values[i] == prefs.getInt(key, fallback))
                selected = i;
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setSingleChoiceItems(labels, selected,
                (dialog, which) -> {
                    prefs.edit().putInt(key, values[which]).apply();
                    dialog.dismiss();
                    if (key.equals("theme"))
                        recreate();
                    else
                        home();
                })
            .setNegativeButton("Cancel", null)
            .show();
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
            .setEnabled(!library.busy);
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
    private void presentGameArtwork(GameLibrary.Game game, List<File> files) {
        if (files.isEmpty()) {
            library.attemptLaunch(game);
            return;
        }
        showingSplashes = true;
        int generation = ++presentationGeneration;
        LinearLayout root = column();
        root.setBackgroundColor(Color.BLACK);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        root.addView(
            action("‹  Cancel startup", false, this::stop), new LinearLayout.LayoutParams(-1, -2));
        liveStatus =
            text("Game launch artwork · embedded ANGLE/Vulkan\nNot frames rendered by guest code",
                12, Color.WHITE, false);
        liveStatus.setPadding(dp(16), dp(12), dp(16), dp(12));
        root.addView(liveStatus);
        prototype = new PrototypeView(this, files,
            info
            -> {
                if (liveStatus != null)
                    liveStatus.setText(info);
            },
            () -> {
                if (showingSplashes && generation == presentationGeneration && resumed) {
                    stop();
                    library.attemptLaunch(game);
                }
            });
        root.addView(prototype, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);
        root.requestApplyInsets();
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
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        } else
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
    private void stop() {
        showingSplashes = false;
        presentationGeneration++;
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
        resumed = false;
        boolean wasRunning = prototype != null;
        boolean wasArtwork = showingSplashes;
        if (wasRunning)
            stop();
        resumeTest = wasRunning && !wasArtwork;
        Diagnostics.record("LIFECYCLE", "paused; graphics resources released if active");
        super.onPause();
    }
    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        if (resumeTest) {
            resumeTest = false;
            launch();
        } else
            libraryChanged.run();
    }
    @Override
    protected void onSaveInstanceState(Bundle out) {
        out.putBoolean("prototype", (prototype != null && !showingSplashes) || resumeTest);
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
        if (result != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        if (request == IMPORT_FILE)
            library.importFile(uri);
        else if (request == IMPORT_FOLDER) {
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
                report = "PodNative game import report\nApp=" + selected.title
                    + "\nVersion=" + selected.version + "\nBuild=" + BuildConfig.SOURCE_REVISION
                    + "\n\n" + selected.report
                    + (selected.launchReport.isEmpty()
                            ? ""
                            : "\n\nLATEST LAUNCH ATTEMPT\n" + selected.launchReport);
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
            }, "PodNative-export").start();
        }
    }
}
