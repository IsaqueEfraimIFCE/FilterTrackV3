package com.filtertrack

import android.annotation.SuppressLint
import android.app.DownloadManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.net.http.SslError
import android.os.Bundle
import android.os.Environment
import android.view.ViewGroup
import android.webkit.ConsoleMessage
import android.webkit.JavascriptInterface
import android.webkit.SslErrorHandler
import android.webkit.URLUtil
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity

class BiDashboardActivity : AppCompatActivity() {

    private lateinit var webView: WebView
    private var handledLoadFailure = false

    companion object {
        private const val BI_URL = "https://filtertrack-api.fly.dev/bi"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        webView = WebView(this)
        webView.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )
        setContentView(webView)
        setupWebView()
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            loadWithOverviewMode = true
            useWideViewPort = true
            cacheMode = WebSettings.LOAD_DEFAULT
            mixedContentMode = WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE
        }
        WebView.setWebContentsDebuggingEnabled(BuildConfig.DEBUG)

        webView.setDownloadListener { url, userAgent, contentDisposition, mimeType, _ ->
            downloadFromBi(url, null, userAgent, contentDisposition, mimeType)
        }

        webView.webChromeClient = object : WebChromeClient() {
            override fun onConsoleMessage(msg: ConsoleMessage): Boolean {
                android.util.Log.d("BIWebView", "[${msg.messageLevel()}] ${msg.message()} @${msg.lineNumber()}")
                return true
            }
        }

        webView.webViewClient = object : WebViewClient() {
            override fun onPageStarted(view: WebView?, url: String?, favicon: Bitmap?) {
                handledLoadFailure = false
            }

            override fun onPageFinished(view: WebView?, url: String?) {
                if (!handledLoadFailure) {
                    injectUserLogin()
                }
            }

            override fun onReceivedError(
                view: WebView?,
                request: WebResourceRequest?,
                error: WebResourceError?
            ) {
                if (request?.isForMainFrame == true) {
                    routeBackToInitialScreen("Erro ao conectar ao BI")
                }
            }

            override fun onReceivedHttpError(
                view: WebView?,
                request: WebResourceRequest?,
                errorResponse: WebResourceResponse?
            ) {
                if (request?.isForMainFrame == true && (errorResponse?.statusCode ?: 200) >= 400) {
                    routeBackToInitialScreen("Erro ao conectar ao BI")
                }
            }

            override fun onReceivedSslError(
                view: WebView?,
                handler: SslErrorHandler?,
                error: SslError?
            ) {
                handler?.cancel()
                routeBackToInitialScreen("Erro ao conectar ao BI")
            }
        }

        webView.addJavascriptInterface(BiDownloadBridge(), "FilterTrackAndroid")
        webView.loadUrl(BI_URL)
    }

    private fun downloadFromBi(
        url: String?,
        requestedFilename: String?,
        userAgent: String?,
        contentDisposition: String?,
        mimeType: String?
    ) {
        if (url.isNullOrBlank()) return
        try {
            val uri = Uri.parse(url)
            if (!isAllowedBiDownload(uri)) {
                Toast.makeText(this, "Download bloqueado: origem invalida", Toast.LENGTH_LONG).show()
                return
            }
            val filename = sanitizeDownloadFilename(
                requestedFilename?.takeIf { it.isNotBlank() }
                    ?: URLUtil.guessFileName(url, contentDisposition, mimeType)
            )
            val request = DownloadManager.Request(uri).apply {
                setTitle(filename)
                setDescription("Baixando dados do FilterTrack BI")
                setMimeType(guessDownloadMimeType(filename, mimeType))
                userAgent?.takeIf { it.isNotBlank() }?.let {
                    addRequestHeader("User-Agent", it)
                }
                currentBiAccessKey()?.let { addRequestHeader("X-Access-Key", it) }
                setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, filename)
                setAllowedOverMetered(true)
                setAllowedOverRoaming(true)
            }
            val manager = getSystemService(Context.DOWNLOAD_SERVICE) as DownloadManager
            manager.enqueue(request)
            Toast.makeText(this, "Download iniciado: $filename", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Toast.makeText(this, "Falha ao iniciar download: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun sanitizeDownloadFilename(filename: String): String {
        val cleaned = filename
            .replace(Regex("""[\\/:*?"<>|\x00-\x1F]"""), "_")
            .trim()
            .trim('.')
        return cleaned.takeIf { it.isNotBlank() } ?: "filtertrack-download.csv"
    }

    private fun guessDownloadMimeType(filename: String, mimeType: String?): String {
        if (!mimeType.isNullOrBlank()) return mimeType
        return if (filename.endsWith(".json", ignoreCase = true)) {
            "application/json"
        } else {
            "text/csv"
        }
    }

    private fun isAllowedBiDownload(uri: Uri): Boolean {
        return uri.scheme == "https" &&
            uri.host == "filtertrack-api.fly.dev" &&
            (uri.path ?: "").startsWith("/filtertrack/bi/export/")
    }

    private fun routeBackToInitialScreen(message: String) {
        if (handledLoadFailure || isFinishing || isDestroyed) return
        handledLoadFailure = true
        val intent = Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
            putExtra("reset_to_initial_screen", true)
            putExtra(MainActivity.EXTRA_STARTUP_ERROR, message)
        }
        startActivity(intent)
        finish()
    }

    private fun injectUserLogin() {
        val biAccessKey = currentBiAccessKey()
        if (biAccessKey.isNullOrBlank()) return
        val js = """
            (function() {
              try {
                var key = ${toJsString(biAccessKey)};
                localStorage.setItem("filtertrack.bi.key", key);
                var input = document.getElementById("accessKey");
                if (input) input.value = key;
                var loginPanel = document.getElementById("login");
                if (loginPanel && !loginPanel.classList.contains("hidden")) {
                  var loginButton = document.getElementById("loginBtn");
                  if (loginButton) loginButton.click();
                }
                if (window.FilterTrackAndroid) {
                  window.downloadBiFile = function(path, filename) {
                    try {
                      var url = new URL(path, window.location.origin);
                      if (key) url.searchParams.set("accessKey", key);
                      window.FilterTrackAndroid.downloadBiFile(url.toString(), filename || "");
                    } catch (downloadErr) {
                      console.error("FilterTrack BI native download failed", downloadErr);
                    }
                  };
                }
              } catch (err) {
                console.error("FilterTrack BI auto-login failed", err);
              }
            })();
        """.trimIndent()
        webView.post { webView.evaluateJavascript(js, null) }
    }

    private fun currentBiAccessKey(): String? =
        BuildConfig.FILTERTRACK_BI_ADMIN_KEY.takeIf { it.isNotBlank() }
            ?: BuildConfig.FILTERTRACK_BI_USER_KEY.takeIf { it.isNotBlank() }

    private fun toJsString(value: String): String =
        "\"" + value
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r") + "\""

    private inner class BiDownloadBridge {
        @JavascriptInterface
        fun downloadBiFile(url: String?, filename: String?) {
            runOnUiThread {
                downloadFromBi(url, filename, webView.settings.userAgentString, null, null)
            }
        }
    }

    override fun onBackPressed() {
        if (webView.canGoBack()) webView.goBack() else super.onBackPressed()
    }

    override fun onDestroy() {
        super.onDestroy()
        try {
            webView.destroy()
        } catch (_: Throwable) {
        }
    }
}
