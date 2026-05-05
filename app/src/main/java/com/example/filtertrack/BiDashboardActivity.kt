package com.example.filtertrack

import android.annotation.SuppressLint
import android.content.Intent
import android.graphics.Bitmap
import android.net.http.SslError
import android.os.Bundle
import android.view.ViewGroup
import android.webkit.ConsoleMessage
import android.webkit.SslErrorHandler
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.appcompat.app.AppCompatActivity

class BiDashboardActivity : AppCompatActivity() {

    private lateinit var webView: WebView
    private var handledLoadFailure = false

    companion object {
        private const val BI_URL = "https://filtertrack-api.fly.dev/bi"
        private const val BI_USER_KEY = "ft_user_a53299e1f0624132a1d645bd81d359ee"
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
        WebView.setWebContentsDebuggingEnabled(true)

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

        webView.loadUrl(BI_URL)
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
        val js = """
            (function() {
              try {
                var key = "$BI_USER_KEY";
                localStorage.setItem("filtertrack.bi.key", key);
                var input = document.getElementById("accessKey");
                if (input) input.value = key;
                var loginPanel = document.getElementById("login");
                if (loginPanel && !loginPanel.classList.contains("hidden")) {
                  var loginButton = document.getElementById("loginBtn");
                  if (loginButton) loginButton.click();
                }
              } catch (err) {
                console.error("FilterTrack BI auto-login failed", err);
              }
            })();
        """.trimIndent()
        webView.post { webView.evaluateJavascript(js, null) }
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
