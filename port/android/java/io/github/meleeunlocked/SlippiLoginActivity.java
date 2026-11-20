// Slippi sign-in, as slippi-android does it (SlippiLoginActivity there): slippi.gg's own page
// (https://slippi.gg/online/enable) in a WebView. Once the player has signed in and has a connect code,
// the page offers user.json as a download; it is caught (the page makes it a Blob, read back through a
// small script, or a plain download fetched with the WebView's cookies) and written to the player's
// folder, where the game looks first (runtime/hle/slippi_account.h: <user-dir>/user.json). No Slippi
// keys ship in the app: the sign-in is slippi.gg's.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.meleeunlocked;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Base64;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

public class SlippiLoginActivity extends Activity {
    private static final String TAG = "SlippiLogin";
    static final String LOGIN_URL = "https://slippi.gg/online/enable";

    private WebView webview;
    private TextView status;
    private volatile boolean saved = false;

    // Every Blob the page makes for a download is read back (base64) to SlippiAndroid.onBlob.
    private static final String JS_BLOB_HOOK =
        "(function(){if(window.__mu_blob_hooked)return;window.__mu_blob_hooked=true;var orig=URL.createObjectURL;"
      + "URL.createObjectURL=function(blob){var url=orig.call(URL,blob);try{if(blob&&blob.size&&blob.size<1000000){"
      + "var r=new FileReader();r.onload=function(){var d=r.result||'';var c=d.indexOf(',');"
      + "if(window.SlippiAndroid)SlippiAndroid.onBlob(c>=0?d.substring(c+1):'');};r.readAsDataURL(blob);}}catch(e){}"
      + "return url;};})();";
    // The enable page renders its Download button once signed in: pressed for the player.
    private static final String JS_AUTO_DOWNLOAD =
        "(function(){if(window.__mu_clicked)return;var tries=0;var iv=setInterval(function(){tries++;"
      + "var nodes=document.querySelectorAll('button, a');for(var i=0;i<nodes.length;i++){var n=nodes[i];"
      + "var t=(n.innerText||n.textContent||'').trim().toLowerCase();if(t==='download'){window.__mu_clicked=true;"
      + "n.click();clearInterval(iv);return;}}if(tries>30)clearInterval(iv);},200);})();";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(14, 16, 22));
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        int pad = Math.round(8 * getResources().getDisplayMetrics().density);
        bar.setPadding(pad, pad, pad, pad);
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(16);
        bar.addView(status, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button cancel = new Button(this);
        cancel.setText("Cancel");
        cancel.setAllCaps(false);
        cancel.setOnClickListener(v -> finish());
        bar.addView(cancel);
        root.addView(bar);
        webview = new WebView(this);
        root.addView(webview, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);

        CookieManager cookies = CookieManager.getInstance();
        cookies.setAcceptCookie(true);
        cookies.setAcceptThirdPartyCookies(webview, true);
        WebSettings s = webview.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        webview.addJavascriptInterface(new Bridge(), "SlippiAndroid");
        webview.setWebViewClient(new WebViewClient() {
            @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap icon) {
                status.setText("Loading…");
                view.evaluateJavascript(JS_BLOB_HOOK, null);
            }
            @Override public void onPageFinished(WebView view, String url) {
                view.evaluateJavascript(JS_BLOB_HOOK, null);
                // The enable page asks for a sign-in first and offers the download once signed in; the
                // script presses Download when it appears and the sign-in is saved as it arrives.
                if (url != null && url.contains("slippi.gg/online/enable")) view.evaluateJavascript(JS_AUTO_DOWNLOAD, null);
                status.setText(url != null && url.contains("slippi.gg") ? "Sign in with your Slippi account" : "");
            }
        });
        webview.setDownloadListener((url, userAgent, disposition, mime, length) -> {
            if (url == null || url.startsWith("blob:")) return;   // the Blob hook has it
            fetch(url, userAgent);
        });
        webview.loadUrl(LOGIN_URL);
    }

    private class Bridge {
        @JavascriptInterface
        public void onBlob(String base64) {
            try {
                byte[] bytes = Base64.decode(base64, Base64.DEFAULT);
                int i = 0;
                while (i < bytes.length && Character.isWhitespace((char) bytes[i])) ++i;
                if (i >= bytes.length || bytes[i] != '{') return;   // user.json is a JSON object
                runOnUiThread(() -> { status.setText("Saving your sign-in…"); save(bytes); });
            } catch (IllegalArgumentException ignored) {
            }
        }
    }

    private void fetch(String url, String userAgent) {
        status.setText("Downloading…");
        new Thread(() -> {
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection) new URL(url).openConnection();
                c.setConnectTimeout(15000);
                c.setReadTimeout(30000);
                if (userAgent != null) c.setRequestProperty("User-Agent", userAgent);
                String cookie = CookieManager.getInstance().getCookie(url);
                if (cookie != null) c.setRequestProperty("Cookie", cookie);
                if (c.getResponseCode() / 100 != 2) { fail("HTTP " + c.getResponseCode() + ": sign in and try again"); return; }
                ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                try (InputStream in = new BufferedInputStream(c.getInputStream())) {
                    byte[] chunk = new byte[8192];
                    int n;
                    while ((n = in.read(chunk)) > 0) buffer.write(chunk, 0, n);
                }
                byte[] bytes = buffer.toByteArray();
                runOnUiThread(() -> save(bytes));
            } catch (IOException e) {
                fail("Download failed: " + e.getMessage());
            } finally {
                if (c != null) c.disconnect();
            }
        }, "slippi-user-json").start();
    }

    private void save(byte[] bytes) {
        if (saved) return;
        saved = true;
        File target = LauncherActivity.userJson(this);
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) { saved = false; fail("Cannot create " + parent); return; }
        File partial = new File(target.getPath() + ".partial");
        try (FileOutputStream out = new FileOutputStream(partial)) {
            out.write(bytes);
        } catch (IOException e) {
            saved = false;
            fail("Could not save the sign-in: " + e.getMessage());
            return;
        }
        if (!partial.renameTo(target)) { saved = false; fail("Could not save the sign-in"); return; }
        Log.i(TAG, "saved user.json (" + bytes.length + " bytes)");
        String code = LauncherActivity.connectCode(this);
        Toast.makeText(this, code != null ? "Signed in as " + code : "Signed in to Slippi", Toast.LENGTH_SHORT).show();
        setResult(RESULT_OK);
        finish();
    }

    private void fail(String message) {
        runOnUiThread(() -> {
            status.setText("Sign-in failed");
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        });
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_B && event.getAction() == KeyEvent.ACTION_UP) {
            onBackPressed();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        if (webview.canGoBack()) webview.goBack();
        else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        if (webview != null) { webview.stopLoading(); webview.destroy(); }
        super.onDestroy();
    }
}
