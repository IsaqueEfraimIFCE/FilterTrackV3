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
import android.widget.FrameLayout
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity

class BiDashboardActivity : AppCompatActivity() {

    private lateinit var webView: WebView
    private var handledLoadFailure = false

    companion object {
        private const val BI_URL = "https://filtertrack-api.fly.dev/bi"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.WHITE)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        webView = WebView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        root.addView(webView)
        setContentView(root)
        applyFilterTrackSystemBars(root)
        setupWebView()
        installWebViewBackHandler()
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
                    if (intent.getBooleanExtra(MainActivity.EXTRA_INITIAL_GUIDE, false)) {
                        injectGuideOverlay()
                    }
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

    private fun injectGuideOverlay() {
        val js = """
            (function() {
              if (document.getElementById('filtertrack-initial-guide')) return;
              var overlay = document.createElement('div');
              overlay.id = 'filtertrack-initial-guide';
              overlay.style.cssText = 'position:fixed;inset:0;z-index:2147483647;display:flex;align-items:flex-end;padding:16px;font-family:system-ui,sans-serif;pointer-events:none';
              overlay.innerHTML = '<div id="filtertrack-bi-spotlight" style="position:fixed;border:3px solid #0068b4;box-shadow:0 0 0 9999px rgba(8,12,20,.58),0 0 0 5px rgba(0,104,180,.25);pointer-events:none"></div><div style="width:100%;max-width:560px;margin:0 auto;background:#fff;color:#161616;padding:20px;border-top:4px solid #0068b4;box-shadow:0 12px 36px rgba(0,0,0,.3);pointer-events:auto">' +
                '<div style="font-size:12px;color:#0068b4;font-weight:700;letter-spacing:.08em;text-transform:uppercase">Guia · 13 de 13</div>' +
                '<h2 style="margin:8px 0;font-size:22px">Painel BI</h2>' +
                '<p style="margin:0 0 12px;line-height:1.45">Aqui ficam os dados sincronizados de toda a operação: indicadores, filtros, sessões e propostas. Use os filtros de período e estação para analisar tendências e exporte CSV ou JSON quando precisar compartilhar os resultados.</p>' +
                '<p style="margin:0 0 12px;color:#525252;font-size:14px">O BI precisa de internet. Os dados de campo continuam sendo coletados localmente quando a rede não está disponível.</p>' +
                '<label style="display:flex;align-items:center;gap:10px;margin:0 0 16px;font-size:14px"><input id="showGuideNextTime" type="checkbox" style="width:20px;height:20px"> Mostrar tutorial novamente na próxima abertura</label>' +
                '<button onclick="window.FilterTrackAndroid.completeInitialGuide(!!document.getElementById(&quot;showGuideNextTime&quot;).checked)" style="width:100%;min-height:48px;border:0;background:#0068b4;color:#fff;font-size:16px">Concluir guia</button>' +
              '</div>';
              document.body.appendChild(overlay);
              var target = document.querySelector('nav') || document.querySelector('header') || document.querySelector('main');
              var spot = document.getElementById('filtertrack-bi-spotlight');
              if (target && spot) {
                var r = target.getBoundingClientRect();
                spot.style.left = Math.max(4, r.left - 5) + 'px';
                spot.style.top = Math.max(4, r.top - 5) + 'px';
                spot.style.width = Math.max(40, r.width + 10) + 'px';
                spot.style.height = Math.max(40, r.height + 10) + 'px';
              }
            })();
        """.trimIndent()
        webView.postDelayed({ webView.evaluateJavascript(js, null) }, 700)
    }
    private fun toJsString(value: String): String =
        "\"" + value
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r") + "\""

    private inner class BiDownloadBridge {
        @JavascriptInterface
        fun completeInitialGuide(showNextTime: Boolean) {
            runOnUiThread {
                getSharedPreferences(MainActivity.GUIDE_PREFS, MODE_PRIVATE)
                    .edit()
                    .putBoolean(MainActivity.GUIDE_COMPLETE, true)
                    .putBoolean(MainActivity.GUIDE_SHOW_ON_STARTUP, showNextTime)
                    .apply()
                val home = Intent(this@BiDashboardActivity, MainActivity::class.java).apply {
                    addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                    putExtra("reset_to_initial_screen", true)
                    putExtra("suppress_initial_guide_once", true)
                }
                startActivity(home)
                finish()
            }
        }

        @JavascriptInterface
        fun downloadBiFile(url: String?, filename: String?) {
            runOnUiThread {
                downloadFromBi(url, filename, webView.settings.userAgentString, null, null)
            }
        }
    }

    private fun installWebViewBackHandler() {
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (webView.canGoBack()) webView.goBack() else finish()
            }
        })
    }}
