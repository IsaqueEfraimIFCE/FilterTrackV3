package com.filtertrack

import android.graphics.Color
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import androidx.core.graphics.Insets
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.updatePadding

/** Keeps content clear of system bars, cutouts, and gesture areas. */
fun AppCompatActivity.applyFilterTrackSystemBars(root: View) {
    WindowCompat.setDecorFitsSystemWindows(window, false)
    window.statusBarColor = Color.TRANSPARENT
    window.navigationBarColor = Color.WHITE
    WindowInsetsControllerCompat(window, root).apply {
        isAppearanceLightStatusBars = true
        isAppearanceLightNavigationBars = true
    }

    ViewCompat.setOnApplyWindowInsetsListener(root) { view, windowInsets ->
        val systemBars = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
        val cutout = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())
        val safe = Insets.max(systemBars, cutout)
        view.updatePadding(left = safe.left, top = safe.top, right = safe.right, bottom = safe.bottom)
        windowInsets
    }
    ViewCompat.requestApplyInsets(root)
}
