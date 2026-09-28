package io.github.qazi0.quicksolar;

import android.app.Activity;
import android.app.UiModeManager;
import android.content.res.Configuration;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Calendar;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * QuickSolar -- the same APK on Android TV and on phones, for any GoodWe
 * hybrid (ET family) inverter. Nothing about the site is built in: on first start
 * the page shows a setup screen that discovers the inverter (Discovery.java) and
 * asks for battery size, charge limit and sunrise/sunset; the answers are kept in
 * SharedPreferences.
 *
 * The page (index.html) is bundled in the APK. It asks http://127.0.0.1:8768/api/...
 * and this activity answers:
 *
 *   /api/config[/save]  the setup (answered here, never forwarded)
 *   /api/discover       search the LAN for the inverter / a solard server
 *   live data           from a solard server if one is set up (records history);
 *                       otherwise, or while it is unreachable, straight from the
 *                       inverter (Inverter.java)
 *   history             from the solard server only; without one the charts say
 *                       "History Unavailable"
 *
 * The inverter is polled here only while the app is on screen and no server is
 * answering, so the dongle normally sees a single client. Nothing runs in the
 * background after the app is left.
 */
public final class MainActivity extends Activity {
    private static final int PERIOD_MS = 5000;
    private SharedPreferences prefs;

    private WebView web;
    private final Inverter inverter = new Inverter();
    private volatile Thread poller;
    private volatile boolean serverDown;        // set when a request to the server fails

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        /* a TV: leanback, TV UI mode, or simply no touchscreen (older Android TV boxes) */
        boolean onTv = getPackageManager().hasSystemFeature("android.software.leanback")
                || ((UiModeManager) getSystemService(UI_MODE_SERVICE)).getCurrentModeType() == Configuration.UI_MODE_TYPE_TELEVISION
                || !getPackageManager().hasSystemFeature("android.hardware.touchscreen");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setStatusBarColor(0xFF0C0D10);
        getWindow().setNavigationBarColor(0xFF0C0D10);
        prefs = getSharedPreferences("solar", MODE_PRIVATE);
        applyConfig();
        web = new WebView(this);
        web.getSettings().setJavaScriptEnabled(true);
        web.setBackgroundColor(0xFF0C0D10);
        web.setWebViewClient(new Client());
        if (onTv) web.setSystemUiVisibility(View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN);
        setContentView(web);
        web.loadUrl("file:///android_asset/index.html");
    }

    /* Direct inverter polling: only while visible, and only while no server answers. */
    @Override protected void onResume() {
        super.onResume();
        final Thread t = new Thread(new Runnable() {
            public void run() {
                long next = 0;
                while (poller == Thread.currentThread()) {
                    if ((serverDown || server().isEmpty()) && System.currentTimeMillis() >= next) {
                        inverter.poll(PERIOD_MS);
                        next = System.currentTimeMillis() + PERIOD_MS;
                    } else if (!serverDown && !server().isEmpty()) {
                        inverter.close();
                    }
                    try { Thread.sleep(500); } catch (InterruptedException e) { break; }
                }
                inverter.close();
            }
        }, "inverter-poll");
        poller = t;
        t.start();
    }

    @Override protected void onPause() {
        super.onPause();
        Thread t = poller;
        poller = null;
        if (t != null) t.interrupt();
    }

    /** Answers the bundled page's API calls. */
    private final class Client extends WebViewClient {
        /* links (About / splash: GitHub) open in the browser, not inside the dashboard */
        @Override public boolean shouldOverrideUrlLoading(WebView v, String url) {
            if (url.startsWith("http://") || url.startsWith("https://")) {
                try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); } catch (Exception ignored) { }
                return true;
            }
            return false;
        }

        @Override public WebResourceResponse shouldInterceptRequest(WebView v, WebResourceRequest req) {
            if (!req.getUrl().toString().startsWith("http://127.0.0.1:8768/")) return null;
            String path = req.getUrl().getPath();
            String query = req.getUrl().getEncodedQuery();
            String server = server();
            String full = server + path + (query == null ? "" : "?" + query);

            if ("/api/config".equals(path)) return json(configJson());
            if ("/api/config/save".equals(path)) { save(req.getUrl()); return json(configJson()); }
            if ("/api/discover".equals(path)) return json(Discovery.run());
            if ("/api/probe".equals(path)) return json(Discovery.probeOne(String.valueOf(req.getUrl().getQueryParameter("ip")).trim()));
            if ("/api/now".equals(path)) {
                String body = server.isEmpty() ? null : fetch(full, 1500);
                if (body != null) { serverDown = false; return json(body); }
                serverDown = true;                          // no server: straight from the inverter
                return json(inverter.json());
            }
            if ("/api/day".equals(path) || "/api/days".equals(path)) {
                String body = server.isEmpty() ? null : fetch(full, 4000);
                if (body != null) { serverDown = false; return json(body); }
                serverDown = true;
                if ("/api/days".equals(path)) return json("{\"offline\":true,\"first\":0,\"days\":[]}");
                Calendar c = Calendar.getInstance();
                int nowMin = c.get(Calendar.HOUR_OF_DAY) * 60 + c.get(Calendar.MINUTE);
                String today = String.format("%04d-%02d-%02d", c.get(Calendar.YEAR), c.get(Calendar.MONTH) + 1,
                        c.get(Calendar.DAY_OF_MONTH));
                boolean isToday = query == null || query.contains(today);
                return json("{\"offline\":true,\"date\":0,\"today\":" + isToday + ",\"now_min\":" + nowMin
                        + ",\"points\":[],\"summary\":" + (isToday ? inverter.todaySummary : "null") + "}");
            }
            return null;
        }
    }

    /* ---------------- setup (SharedPreferences) ---------------- */
    private String server() { return prefs.getString("server", ""); }

    private void applyConfig() {
        inverter.configure(prefs.getString("inverter_ip", ""), prefs.getString("transport", "tcp"),
                prefs.getFloat("capacity_kwh", 0));
    }

    /** /api/config/save?inverter_ip=&transport=&server=&capacity_kwh=&charge_limit=&sun_mode=&sunrise=&sunset= */
    private void save(Uri u) {
        SharedPreferences.Editor e = prefs.edit();
        for (String k : new String[] { "inverter_ip", "transport", "server", "sun_mode", "sunrise", "sunset", "lat", "lon" }) {
            String v = u.getQueryParameter(k);
            if (v != null) e.putString(k, v.trim());
        }
        for (String k : new String[] { "capacity_kwh", "charge_limit" }) {
            String v = u.getQueryParameter(k);
            try { if (v != null) e.putFloat(k, Float.parseFloat(v)); } catch (NumberFormatException ignored) { }
        }
        e.putBoolean("configured", true).apply();
        serverDown = false;
        applyConfig();
    }

    private String configJson() {
        return String.format(Locale.US,
            "{\"app\":true,\"configured\":%s,\"inverter_ip\":%s,\"transport\":%s,\"server\":%s,"
            + "\"capacity_kwh\":%.1f,\"charge_limit\":%.0f,\"sun_mode\":%s,\"sunrise\":%s,\"sunset\":%s,\"lat\":%s,\"lon\":%s,\"version\":%s}",
            prefs.getBoolean("configured", false), q(prefs.getString("inverter_ip", "")), q(prefs.getString("transport", "tcp")),
            q(server()), prefs.getFloat("capacity_kwh", 0), prefs.getFloat("charge_limit", 100),
            q(prefs.getString("sun_mode", "auto")), q(prefs.getString("sunrise", "06:00")), q(prefs.getString("sunset", "18:00")),
            num(prefs.getString("lat", "")), num(prefs.getString("lon", "")), q(versionName()));
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return ""; }
    }

    /** a stored number as JSON, or null */
    private static String num(String s) {
        try { return String.valueOf(Double.parseDouble(s)); } catch (Exception e) { return "null"; }
    }

    private static String q(String s) { return s == null ? "null" : "\"" + s.replace("\\", "").replace("\"", "'") + "\""; }

    private static WebResourceResponse json(String body) {
        Map<String, String> h = new HashMap<String, String>();
        h.put("Access-Control-Allow-Origin", "*");
        h.put("Cache-Control", "no-store");
        try {
            return new WebResourceResponse("application/json", "utf-8", 200, "OK", h,
                    new ByteArrayInputStream(body.getBytes("UTF-8")));
        } catch (Exception e) {
            return null;
        }
    }

    private static String fetch(String url, int readTimeoutMs) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(1200);
            c.setReadTimeout(readTimeoutMs);
            if (c.getResponseCode() != 200) { c.disconnect(); return null; }
            InputStream in = c.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            in.close();
            c.disconnect();
            return out.toString("UTF-8");
        } catch (Exception e) {
            return null;
        }
    }
}
