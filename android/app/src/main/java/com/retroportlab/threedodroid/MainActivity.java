package com.retroportlab.threedodroid;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.text.format.DateUtils;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class MainActivity extends Activity {
    // ---------------------------------------------------------------- look --
    private static final int BG = 0xFF0C0D14, PANEL = 0xFF171926, PANEL_FOCUS = 0xFF252840;
    private static final int ACCENT = 0xFFFFC247, TEXT = 0xFFF2F2F5, SUBTEXT = 0xFF9AA0B4;

    private static final int PICK_GAME = 1, PICK_BIOS = 2, PICK_FOLDER = 3, SAVE_LOG = 4;
    private String pendingLog = null;
    private static final int SCREEN_LIBRARY = 0, SCREEN_SETTINGS = 1, SCREEN_GAME = 2, SCREEN_UPDATE = 3;
    private static final int BIOS_SIZE = 1 << 20;

    private SharedPreferences prefs;
    private FrameLayout root;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private int screen = SCREEN_LIBRARY;

    private Updater.Release foundUpdate = null;
    private volatile boolean updateBusy = false, updateCancel = false;

    // in-game state
    private boolean menuOpen = false;
    private int keyMask = 0, axisMask = 0;
    private String gameTitle = null;
    private View pauseMenu = null;
    private TextView fpsView;
    private final List<ParcelFileDescriptor> openFiles = new ArrayList<>();
    private final Runnable fpsTick = new Runnable() {
        @Override public void run() {
            if (fpsView == null || screen != SCREEN_GAME) return;
            fpsView.setText(String.format(Locale.US, "%.0f FPS", NativeLib.getFps()));
            ui.postDelayed(this, 500);
        }
    };

    /** A disc image in the games folder. */
    static final class Game {
        final Uri uri; final String fileName, title;
        Uri tree; String folderId;   // for .cue sheets: where to find the track files
        Game(Uri uri, String fileName) {
            this.uri = uri; this.fileName = fileName;
            this.title = fileName.replaceFirst("\\.[^.]+$", "").replace('_', ' ');
        }
        String type() {
            String l = fileName.toLowerCase(Locale.US);
            int dot = l.lastIndexOf('.');
            return dot >= 0 ? l.substring(dot + 1) : "";
        }
    }

    // ------------------------------------------------------------ lifecycle --
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = getSharedPreferences("3dodroid", MODE_PRIVATE);
        root = new FrameLayout(this);
        root.setBackgroundColor(BG);
        setContentView(root);
        hideSystemUi();
        NativeLib.setNvramPath(new File(getFilesDir(), "nvram.bin").getAbsolutePath());
        Uri launched = getIntent() != null ? getIntent().getData() : null;
        if (launched != null) startGame(new Game(launched, displayName(launched)));
        else showLibrary();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Uri u = intent.getData();
        if (u != null) startGame(new Game(u, displayName(u)));
    }

    @Override protected void onPause() { super.onPause(); NativeLib.setPaused(true); }
    @Override protected void onResume() {
        super.onResume();
        hideSystemUi();
        if (screen == SCREEN_GAME && !menuOpen) NativeLib.setPaused(false);
    }
    @Override protected void onDestroy() { NativeLib.stop(); closeFiles(); super.onDestroy(); }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    // ------------------------------------------------------------- widgets --
    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(10));
        if (stroke != 0) g.setStroke(dp(2), stroke);
        return g;
    }

    /** Card background that lights up with an accent outline when focused by the gamepad. */
    private StateListDrawable cardBg() {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_focused}, rounded(PANEL_FOCUS, ACCENT));
        s.addState(new int[]{android.R.attr.state_pressed}, rounded(PANEL_FOCUS, ACCENT));
        s.addState(new int[]{}, rounded(PANEL, 0));
        return s;
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private LinearLayout row(String title, String subtitle, String value, View.OnClickListener click) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(16), dp(10), dp(16), dp(10));
        r.setBackground(cardBg());
        r.setFocusable(true);
        r.setClickable(true);
        r.setOnClickListener(click);
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(text(title, 17, TEXT, true));
        if (subtitle != null && !subtitle.isEmpty()) texts.addView(text(subtitle, 13, SUBTEXT, false));
        r.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
        if (value != null) {
            TextView v = text(value, 15, ACCENT, true);
            v.setTag("value");
            r.addView(v);
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = dp(6);
        r.setLayoutParams(lp);
        return r;
    }

    private void setRowValue(View rowView, String value) {
        View v = rowView.findViewWithTag("value");
        if (v instanceof TextView) ((TextView) v).setText(value);
    }

    private TextView sectionTitle(String s) {
        TextView t = text(s.toUpperCase(Locale.US), 12, SUBTEXT, true);
        t.setLetterSpacing(0.12f);
        t.setPadding(dp(4), dp(14), 0, dp(6));
        return t;
    }

    private LinearLayout header(String subtitle, View... actions) {
        LinearLayout h = new LinearLayout(this);
        h.setOrientation(LinearLayout.HORIZONTAL);
        h.setGravity(Gravity.CENTER_VERTICAL);
        h.setPadding(dp(24), dp(16), dp(24), dp(8));
        TextView logo = text("3DO", 26, ACCENT, true);
        logo.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        TextView logo2 = text("DROID", 26, TEXT, true);
        logo2.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        h.addView(logo);
        h.addView(logo2);
        h.addView(text("   " + subtitle, 14, SUBTEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        for (View a : actions) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.leftMargin = dp(8);
            h.addView(a, lp);
        }
        return h;
    }

    private TextView pill(String label, View.OnClickListener click) {
        TextView b = text(label, 14, TEXT, true);
        b.setPadding(dp(16), dp(8), dp(16), dp(8));
        b.setBackground(cardBg());
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        return b;
    }

    private void page(String subtitle, LinearLayout list, View... actions) {
        root.removeAllViews();
        root.setBackgroundColor(BG);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.addView(header(subtitle, actions));
        list.setOrientation(LinearLayout.VERTICAL);
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setPadding(dp(24), 0, dp(24), dp(16));
        sv.setClipToPadding(false);
        sv.addView(list);
        page.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));
        TextView foot = text("A  select     B  back     Select + Start  in-game menu", 12, SUBTEXT, false);
        foot.setPadding(dp(28), dp(4), 0, dp(10));
        page.addView(foot);
        root.addView(page, new FrameLayout.LayoutParams(-1, -1));
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (Exception e) { return "?"; }
    }

    private File biosFile() { return new File(getFilesDir(), "bios.bin"); }
    private boolean haveBios() { return biosFile().length() == BIOS_SIZE; }

    // ------------------------------------------------------------- library --
    private void showLibrary() {
        NativeLib.stop();
        closeFiles();
        screen = SCREEN_LIBRARY; menuOpen = false; pauseMenu = null;
        ui.removeCallbacks(fpsTick);
        LinearLayout list = new LinearLayout(this);
        page("v" + versionName(), list, pill("Open file", v -> pick(PICK_GAME)), pill("Settings", v -> showSettings()));

        String folder = prefs.getString("folder", null);
        if (!haveBios() || folder == null) {
            list.addView(sectionTitle("Welcome"));
            TextView intro = text("3DODroid needs two things: the BIOS from your own 3DO console (a 1 MB file such as "
                    + "panafz10.bin), and the folder that holds your games (.chd, .cue or .iso discs).", 15, SUBTEXT, false);
            intro.setPadding(dp(4), 0, dp(4), dp(12));
            list.addView(intro);
            View bios = row(haveBios() ? "BIOS ready" : "Choose BIOS file", "Copied into the app once", haveBios() ? "✓" : null, v -> pick(PICK_BIOS));
            View choose = row(folder != null ? "Games folder chosen" : "Choose games folder", "Pick the folder once; 3DODroid remembers it",
                    folder != null ? "✓" : null, v -> pick(PICK_FOLDER));
            list.addView(bios);
            list.addView(choose);
            (haveBios() ? choose : bios).requestFocus();
            return;
        }

        if (foundUpdate != null) list.addView(updateBanner());
        else autoCheckForUpdate(list);
        TextView scanning = text("Looking for games…", 15, SUBTEXT, false);
        scanning.setPadding(dp(4), dp(16), 0, 0);
        list.addView(scanning);
        Uri tree = Uri.parse(folder);
        new Thread(() -> {
            List<Game> games = scanFolder(tree);
            ui.post(() -> { if (screen == SCREEN_LIBRARY && list.isAttachedToWindow()) fillLibrary(list, scanning, games); });
        }).start();
    }

    private void fillLibrary(LinearLayout list, View scanning, List<Game> games) {
        list.removeView(scanning);
        list.addView(sectionTitle(games.size() + (games.size() == 1 ? " game" : " games")));
        if (games.isEmpty()) {
            list.addView(row("No discs found", "Put .chd, .cue or .iso files in the folder (or one folder down), or choose another folder",
                    null, v -> pick(PICK_FOLDER)));
        }
        View first = null;
        for (Game g : games) {
            View r = row(g.title, g.type().toUpperCase(Locale.US), null, v -> startGame(g));
            list.addView(r);
            if (first == null) first = r;
        }
        if (first != null) first.requestFocus();
    }

    private List<Game> scanFolder(Uri tree) {
        List<Game> out = new ArrayList<>();
        try { scanDir(tree, DocumentsContract.getTreeDocumentId(tree), out, true); }
        catch (Exception ignored) { }
        Collections.sort(out, (a, b) -> a.title.compareToIgnoreCase(b.title));
        return out;
    }

    /** Disc images in one folder; discs often sit in a folder each, so look one level down too. */
    private void scanDir(Uri tree, String dirId, List<Game> out, boolean descend) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dirId);
        List<Game> bins = new ArrayList<>();
        boolean haveCue = false;
        try (Cursor c = getContentResolver().query(children, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))) {
                    if (descend) scanDir(tree, id, out, false);
                    continue;
                }
                String lower = name.toLowerCase(Locale.US);
                Game g = new Game(DocumentsContract.buildDocumentUriUsingTree(tree, id), name);
                g.tree = tree; g.folderId = dirId;
                if (lower.endsWith(".chd") || lower.endsWith(".iso")) out.add(g);
                else if (lower.endsWith(".cue")) { out.add(g); haveCue = true; }
                else if (lower.endsWith(".bin") && c.getLong(3) > 16L * 1024 * 1024) bins.add(g);   // skips BIOS files
            }
        } catch (Exception ignored) { }
        if (!haveCue) out.addAll(bins);   // a lone .bin with no cue sheet
    }

    // ------------------------------------------------------------ settings --
    private void showSettings() {
        screen = SCREEN_SETTINGS;
        LinearLayout list = new LinearLayout(this);
        page("Settings", list, pill("Back", v -> showLibrary()));
        list.addView(sectionTitle("Console"));
        View bios = row("BIOS", "The 1 MB ROM from your own 3DO (for example panafz10.bin)",
                haveBios() ? "Loaded" : "Not set", v -> pick(PICK_BIOS));
        list.addView(bios);
        list.addView(row("Games folder", folderLabel(), "Change", v -> pick(PICK_FOLDER)));
        list.addView(sectionTitle("Sound and display"));
        LinearLayout snd = row("Sound", null, prefs.getBoolean("sound", true) ? "On" : "Off", null);
        snd.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("sound", true);
            prefs.edit().putBoolean("sound", on).apply();
            setRowValue(snd, on ? "On" : "Off");
        });
        list.addView(snd);
        LinearLayout fps = row("Show frame rate", null, prefs.getBoolean("showFps", false) ? "On" : "Off", null);
        fps.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("showFps", false);
            prefs.edit().putBoolean("showFps", on).apply();
            setRowValue(fps, on ? "On" : "Off");
        });
        list.addView(fps);
        list.addView(sectionTitle("Updates"));
        LinearLayout auto = row("Check automatically", "Looks on GitHub for a new version at most twice a day",
                prefs.getBoolean("autoUpdate", true) ? "On" : "Off", null);
        auto.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("autoUpdate", true);
            prefs.edit().putBoolean("autoUpdate", on).apply();
            setRowValue(auto, on ? "On" : "Off");
        });
        list.addView(auto);
        final View[] check = new View[1];
        check[0] = row("Check for updates now", "You have v" + versionName(), "", v -> checkForUpdate(check[0]));
        list.addView(check[0]);
        list.addView(sectionTitle("About"));
        TextView about = text("3DODroid v" + versionName() + " is an experimental 3DO emulator written from scratch. "
                + "It needs your own BIOS dump. Not affiliated with The 3DO Company or Panasonic.", 14, SUBTEXT, false);
        about.setPadding(dp(4), 0, dp(4), dp(12));
        list.addView(about);
        bios.requestFocus();
    }

    private String folderLabel() {
        String f = prefs.getString("folder", null);
        if (f == null) return "Not chosen";
        String id = DocumentsContract.getTreeDocumentId(Uri.parse(f));
        return id.replace("primary:", "Internal storage/");
    }

    // ------------------------------------------------------------- updates --
    private void autoCheckForUpdate(LinearLayout list) {
        if (!prefs.getBoolean("autoUpdate", true) || updateBusy) return;
        long last = prefs.getLong("updateChecked", 0);
        if (System.currentTimeMillis() - last < 12 * DateUtils.HOUR_IN_MILLIS) return;
        prefs.edit().putLong("updateChecked", System.currentTimeMillis()).apply();
        new Thread(() -> {
            try {
                Updater.Release r = Updater.latest();
                if (r != null && Updater.newer(r.tag, versionName())) ui.post(() -> {
                    foundUpdate = r;
                    if (screen == SCREEN_LIBRARY && list.isAttachedToWindow()) list.addView(updateBanner(), 0);
                });
            } catch (Exception ignored) { }
        }).start();
    }

    private View updateBanner() {
        LinearLayout r = row("Update available: " + foundUpdate.tag, "You have v" + versionName() + ". Select to see what's new and install.",
                "Update", v -> showUpdate(foundUpdate));
        TextView b = (TextView) r.findViewWithTag("value");
        b.setTextColor(BG);
        b.setPadding(dp(10), dp(4), dp(10), dp(4));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(ACCENT); bg.setCornerRadius(dp(6));
        b.setBackground(bg);
        return r;
    }

    private void checkForUpdate(View rowView) {
        if (updateBusy) return;
        updateBusy = true;
        setRowValue(rowView, "Checking…");
        new Thread(() -> {
            String msg; Updater.Release found = null;
            try {
                Updater.Release r = Updater.latest();
                if (r == null) msg = "No releases yet";
                else if (Updater.newer(r.tag, versionName())) { found = r; msg = r.tag + " available"; }
                else msg = "Up to date";
            } catch (Exception e) {
                msg = e instanceof java.net.UnknownHostException ? "No internet" : "Couldn't check";
                final String detail = e.getMessage();
                ui.post(() -> { if (detail != null) toast(detail); });
            }
            final String m = msg; final Updater.Release f = found;
            ui.post(() -> {
                updateBusy = false;
                prefs.edit().putLong("updateChecked", System.currentTimeMillis()).apply();
                if (f != null) { foundUpdate = f; showUpdate(f); }
                else if (rowView.isAttachedToWindow()) setRowValue(rowView, m);
            });
        }).start();
    }

    private void showUpdate(Updater.Release r) {
        screen = SCREEN_UPDATE;
        updateCancel = false;
        LinearLayout list = new LinearLayout(this);
        page("Update", list, pill("Back", v -> { updateCancel = true; showSettings(); }));
        list.addView(sectionTitle("3DODroid " + r.tag));
        TextView sub = text("You have v" + versionName() + (r.apkSize > 0
                ? String.format(Locale.US, "  ·  download %.1f MB", r.apkSize / 1048576.0) : ""), 14, SUBTEXT, false);
        sub.setPadding(dp(4), 0, dp(4), dp(10));
        list.addView(sub);
        final View[] go = new View[1];
        go[0] = row("Download and install", "Your BIOS, saves and settings are kept", null, v -> downloadAndInstall(r, go[0]));
        list.addView(go[0]);
        list.addView(row("Open the releases page", Updater.RELEASES_PAGE, null, v -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(r.pageUrl))); }
            catch (Exception e) { toast("No web browser found"); }
        }));
        if (r.notes != null && !r.notes.trim().isEmpty()) {
            list.addView(sectionTitle("What's new"));
            TextView notes = text(r.notes.replace("\r", "").replaceAll("(?m)^#+\\s*", "").replace("**", "").trim(), 15, TEXT, false);
            notes.setPadding(dp(4), 0, dp(4), dp(12));
            notes.setLineSpacing(0, 1.15f);
            list.addView(notes);
        }
        go[0].requestFocus();
    }

    private void downloadAndInstall(Updater.Release r, View rowView) {
        if (updateBusy) { updateCancel = true; return; }
        if (!Updater.canInstall(this)) {
            toast("Allow 3DODroid to install apps, then press Back and select Download again");
            try { Updater.openInstallPermission(this); } catch (Exception e) { toast("Open Android Settings > Apps > 3DODroid > Install unknown apps"); }
            return;
        }
        updateBusy = true; updateCancel = false;
        TextView title = (TextView) ((LinearLayout) ((LinearLayout) rowView).getChildAt(0)).getChildAt(0);
        title.setText("Downloading…  (select again to cancel)");
        new Thread(() -> {
            String error = null; File apk = null;
            try {
                long[] lastUi = {0};
                apk = Updater.download(this, r, (done, total) -> {
                    long now = System.currentTimeMillis();
                    if (now - lastUi[0] > 200) {
                        lastUi[0] = now;
                        final String t = total > 0 ? String.format(Locale.US, "Downloading…  %d%%  (select again to cancel)", done * 100 / total)
                                : String.format(Locale.US, "Downloading…  %.1f MB", done / 1048576.0);
                        ui.post(() -> title.setText(t));
                    }
                    return !updateCancel;
                });
                if (apk != null) Updater.install(this, apk);
            } catch (Exception e) {
                error = e.getMessage() != null ? e.getMessage() : "Download failed";
            }
            final String err = error; final boolean cancelled = apk == null && err == null;
            ui.post(() -> {
                updateBusy = false;
                if (!title.isAttachedToWindow()) return;
                if (err != null) { title.setText("Download and install"); toast(err); }
                else if (cancelled) title.setText("Download and install");
                else title.setText("Installing…  confirm on the next screen");
            });
        }).start();
    }

    // --------------------------------------------------------------- files --
    private void pick(int code) {
        Intent i;
        if (code == PICK_FOLDER) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
        }
        try { startActivityForResult(i, code); }
        catch (Exception e) { toast("No file picker available"); }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) { if (req == SAVE_LOG) pendingLog = null; return; }
        Uri u = data.getData();
        if (req == SAVE_LOG) {
            try (java.io.OutputStream o = getContentResolver().openOutputStream(u)) {
                o.write(pendingLog.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                toast("Debug log saved");
            } catch (Exception e) { toast("Couldn't save the log"); }
            pendingLog = null;
            return;
        }
        if (req == PICK_FOLDER) {
            try { getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (Exception ignored) { }
            prefs.edit().putString("folder", u.toString()).apply();
            showLibrary();
        } else if (req == PICK_BIOS) {
            byte[] b = readAll(u, BIOS_SIZE + 1);
            if (b == null || b.length != BIOS_SIZE) { toast("That isn't a 3DO BIOS: it should be exactly 1 MB"); return; }
            try (FileOutputStream o = new FileOutputStream(biosFile())) { o.write(b); }
            catch (Exception e) { toast("Couldn't save the BIOS"); return; }
            toast("BIOS loaded");
            if (screen == SCREEN_SETTINGS) showSettings(); else showLibrary();
        } else if (req == PICK_GAME) {
            startGame(new Game(u, displayName(u)));
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) { }
        String p = uri.getLastPathSegment();
        return p != null ? p.substring(p.lastIndexOf('/') + 1) : "game";
    }

    private byte[] readAll(Uri uri, int limit) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[65536];
            for (int n; (n = in.read(b)) > 0; ) {
                out.write(b, 0, n);
                if (out.size() > limit) break;
            }
            return out.toByteArray();
        } catch (Exception e) { return null; }
    }

    private byte[] readFile(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) f.length()];
            int off = 0;
            for (int n; off < b.length && (n = in.read(b, off, b.length - off)) > 0; ) off += n;
            return off == b.length ? b : null;
        } catch (Exception e) { return null; }
    }

    private void closeFiles() {
        for (ParcelFileDescriptor p : openFiles) try { p.close(); } catch (Exception ignored) { }
        openFiles.clear();
    }

    private int openFd(Uri u) throws Exception {
        ParcelFileDescriptor p = getContentResolver().openFileDescriptor(u, "r");
        if (p == null) throw new Exception("Couldn't open " + displayName(u));
        openFiles.add(p);
        return p.getFd();
    }

    /** The track files a cue sheet names, found next to it and opened. */
    private boolean openCueTracks(Game g) {
        List<String> names = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(getContentResolver().openInputStream(g.uri)))) {
            for (String line; (line = r.readLine()) != null; ) {
                String t = line.trim();
                if (!t.regionMatches(true, 0, "FILE", 0, 4)) continue;
                int q1 = t.indexOf('"'), q2 = q1 >= 0 ? t.indexOf('"', q1 + 1) : -1;
                String n = q2 > q1 ? t.substring(q1 + 1, q2) : t.substring(4).trim().split("\\s+")[0];
                names.add(n.substring(Math.max(n.lastIndexOf('/'), n.lastIndexOf('\\')) + 1));
            }
        } catch (Exception e) { toast("Couldn't read the cue sheet"); return false; }

        Map<String, Uri> here = new HashMap<>();
        if (g.tree != null) {
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(g.tree, g.folderId);
            try (Cursor c = getContentResolver().query(children, new String[]{
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
                while (c != null && c.moveToNext())
                    here.put(c.getString(1).toLowerCase(Locale.US), DocumentsContract.buildDocumentUriUsingTree(g.tree, c.getString(0)));
            } catch (Exception ignored) { }
        }
        String[] n = new String[names.size()];
        int[] fds = new int[names.size()];
        for (int i = 0; i < names.size(); i++) {
            Uri t = here.get(names.get(i).toLowerCase(Locale.US));
            if (t == null) {
                toast(g.tree == null ? "Open .cue games from the library (choose their folder) so the track files can be found"
                        : "Missing track file: " + names.get(i));
                return false;
            }
            try { fds[i] = openFd(t); } catch (Exception e) { toast(e.getMessage()); return false; }
            n[i] = names.get(i);
        }
        NativeLib.setCueFiles(n, fds);
        return true;
    }

    // ---------------------------------------------------------------- game --
    private void startGame(Game g) {
        if (!haveBios()) { toast("Choose your 3DO BIOS first"); showSettings(); return; }
        NativeLib.stop();
        closeFiles();
        String type = g.type();
        if (!type.equals("chd") && !type.equals("cue") && !type.equals("iso") && !type.equals("bin")) {
            toast("3DODroid plays .chd, .cue, .iso and .bin discs"); return;
        }
        NativeLib.setCueFiles(null, null);
        if (type.equals("cue") && !openCueTracks(g)) { closeFiles(); return; }
        int fd;
        try { fd = openFd(g.uri); } catch (Exception e) { toast(e.getMessage()); closeFiles(); return; }
        byte[] bios = readFile(biosFile());
        if (bios == null || !NativeLib.loadGame(bios, fd, type)) {
            toast(bios == null ? "Couldn't read the BIOS" : NativeLib.lastError());
            closeFiles();
            if (screen != SCREEN_LIBRARY) showLibrary();
            return;
        }
        enterGame(g.title);
    }

    private void enterGame(String title) {
        gameTitle = title;
        screen = SCREEN_GAME;
        root.removeAllViews();
        root.setBackgroundColor(Color.BLACK);
        SurfaceView view = new AspectSurface(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { NativeLib.setSurface(null); }
        });
        root.addView(view, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        keyMask = axisMask = 0;
        menuOpen = false; pauseMenu = null;
        fpsView = null;
        ui.removeCallbacks(fpsTick);
        if (prefs.getBoolean("showFps", false)) {
            fpsView = text("-- FPS", 14, 0xFFFFE066, true);
            fpsView.setBackgroundColor(0x80000000);
            fpsView.setPadding(dp(8), dp(2), dp(8), dp(2));
            root.addView(fpsView, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START));
            ui.postDelayed(fpsTick, 500);
        }
        NativeLib.setSound(prefs.getBoolean("sound", true));
        NativeLib.start();
    }

    /** Keeps the picture at 4:3 and as large as the screen allows. */
    static class AspectSurface extends SurfaceView {
        AspectSurface(Context c) { super(c); }
        @Override protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            if (w * 3 > h * 4) w = h * 4 / 3; else h = w * 3 / 4;
            setMeasuredDimension(w, h);
        }
    }

    private void openPauseMenu() {
        if (menuOpen) return;
        menuOpen = true;
        keyMask = axisMask = 0;
        NativeLib.setButtons(0);
        NativeLib.setPaused(true);

        FrameLayout dim = new FrameLayout(this);
        dim.setBackgroundColor(0xB0000000);
        dim.setClickable(true);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(20), dp(20), dp(20));
        panel.setBackground(rounded(BG, 0x40FFFFFF));
        panel.addView(text(gameTitle, 20, TEXT, true));
        TextView hint = text("Paused", 13, SUBTEXT, false);
        hint.setPadding(0, 0, 0, dp(12));
        panel.addView(hint);
        View resume = row("Resume", null, null, v -> closePauseMenu());
        panel.addView(resume);
        panel.addView(row("Reset console", "Restarts the game from the BIOS", null, v -> { NativeLib.reset(); closePauseMenu(); }));
        panel.addView(row("Save debug log", "A small text file to send with bug reports", null, v -> saveDebugLog()));
        panel.addView(row("Quit to library", "Your saves are kept", null, v -> showLibrary()));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(420), -2, Gravity.CENTER);
        dim.addView(panel, lp);
        pauseMenu = dim;
        root.addView(dim, new FrameLayout.LayoutParams(-1, -1));
        resume.requestFocus();
    }

    private void saveDebugLog() {
        StringBuilder b = new StringBuilder();
        b.append("3DODroid v").append(versionName()).append("\n");
        b.append("Device: ").append(android.os.Build.MANUFACTURER).append(' ').append(android.os.Build.MODEL)
                .append(", Android ").append(android.os.Build.VERSION.RELEASE).append("\n");
        b.append("Game: ").append(gameTitle).append("\n\n");
        b.append(NativeLib.debugLog());
        pendingLog = b.toString();
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        String safe = gameTitle == null ? "game" : gameTitle.replaceAll("[^A-Za-z0-9]+", "-").replaceAll("^-|-$", "");
        i.putExtra(Intent.EXTRA_TITLE, "3dodroid-log-" + safe + ".txt");
        try { startActivityForResult(i, SAVE_LOG); }
        catch (Exception e) { toast("No file picker available"); }
    }

    private void closePauseMenu() {
        if (!menuOpen) return;
        if (pauseMenu != null) root.removeView(pauseMenu);
        pauseMenu = null;
        menuOpen = false;
        keyMask = axisMask = 0;
        NativeLib.setButtons(0);
        NativeLib.setPaused(false);
    }

    // --------------------------------------------------------------- input --
    /** Retroid layout: the face buttons left to right are 3DO A, B, C; Start is P (play), Select is X (stop). */
    private static int mapKey(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: return NativeLib.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return NativeLib.DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return NativeLib.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return NativeLib.RIGHT;
            case KeyEvent.KEYCODE_BUTTON_X: return NativeLib.A;
            case KeyEvent.KEYCODE_BUTTON_Y: return NativeLib.A;
            case KeyEvent.KEYCODE_BUTTON_A: return NativeLib.B;
            case KeyEvent.KEYCODE_BUTTON_B: return NativeLib.C;
            case KeyEvent.KEYCODE_BUTTON_START: return NativeLib.P;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return NativeLib.X;
            case KeyEvent.KEYCODE_BUTTON_L1: case KeyEvent.KEYCODE_BUTTON_L2: return NativeLib.L;
            case KeyEvent.KEYCODE_BUTTON_R1: case KeyEvent.KEYCODE_BUTTON_R2: return NativeLib.R;
        }
        return 0;
    }

    private boolean selectHeld, startHeld;
    private int swallowUps = 0;

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN, up = e.getAction() == KeyEvent.ACTION_UP;

        if (menuOpen && swallowUps > 0 && up && (code == KeyEvent.KEYCODE_BUTTON_START || code == KeyEvent.KEYCODE_BUTTON_SELECT)) {
            swallowUps--;
            return true;
        }
        if (screen != SCREEN_GAME || menuOpen) {
            if (code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_DPAD_CENTER) {
                if (up) { View f = getCurrentFocus(); if (f != null) f.performClick(); }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_START) {
                if (up) {
                    if (menuOpen) closePauseMenu();
                    else if (screen == SCREEN_SETTINGS) showLibrary();
                    else if (screen == SCREEN_UPDATE) { updateCancel = true; showSettings(); }
                    else if (code == KeyEvent.KEYCODE_BACK) finish();
                }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_SELECT) return true;
            return super.dispatchKeyEvent(e);
        }

        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) selectHeld = down;
        if (code == KeyEvent.KEYCODE_BUTTON_START) startHeld = down;
        if (selectHeld && startHeld) { selectHeld = startHeld = false; swallowUps = 2; openPauseMenu(); return true; }
        if (code == KeyEvent.KEYCODE_BACK) { if (up) openPauseMenu(); return true; }
        int bit = mapKey(code);
        if (bit == 0) return super.dispatchKeyEvent(e);
        if (down) keyMask |= bit; else if (up) keyMask &= ~bit;
        NativeLib.setButtons(keyMask | axisMask);
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (screen == SCREEN_GAME && !menuOpen && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && e.getAction() == MotionEvent.ACTION_MOVE) {
            float x = e.getAxisValue(MotionEvent.AXIS_HAT_X), y = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            float lx = e.getAxisValue(MotionEvent.AXIS_X), ly = e.getAxisValue(MotionEvent.AXIS_Y);
            int m = 0;
            if (x < -0.5f || lx < -0.5f) m |= NativeLib.LEFT;
            if (x > 0.5f || lx > 0.5f) m |= NativeLib.RIGHT;
            if (y < -0.5f || ly < -0.5f) m |= NativeLib.UP;
            if (y > 0.5f || ly > 0.5f) m |= NativeLib.DOWN;
            if (e.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.5f) m |= NativeLib.L;
            if (e.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.5f) m |= NativeLib.R;
            axisMask = m;
            NativeLib.setButtons(keyMask | axisMask);
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    private void hideSystemUi() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }
}
