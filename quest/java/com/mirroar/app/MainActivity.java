package com.mirroar.app;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/**
 * Thin Java layer around the native OpenXR app: hosts the hidden text field that
 * receives the Meta Quest system keyboard, keeps Wi-Fi broadcast reception alive
 * and rasterizes UI text for the native renderer.
 */
public class MainActivity extends NativeActivity {
    static {
        System.loadLibrary("mirroar");
    }

    // Implemented in native code (keyboard.cpp)
    static native void nativeOnText(String text);
    static native void nativeOnKey(int androidKeyCode, boolean down, int metaState);

    private KeyboardSink sink;
    private WifiManager.MulticastLock multicastLock;
    private boolean keyboardVisible;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sink = new KeyboardSink(this);
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicastLock = wifi.createMulticastLock("mirroar-discovery");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception e) {
            multicastLock = null;
        }
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        super.onDestroy();
    }

    /** Called from native code (any thread). */
    public void setKeyboardVisible(final boolean show) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (show) {
                    sink.setFocusable(true);
                    sink.setFocusableInTouchMode(true);
                    sink.requestFocus();
                    imm.restartInput(sink);
                    imm.showSoftInput(sink, InputMethodManager.SHOW_FORCED);
                } else {
                    imm.hideSoftInputFromWindow(sink.getWindowToken(), 0);
                }
                keyboardVisible = show;
            }
        });
    }

    /** Called from native code. */
    public boolean isKeyboardVisible() {
        return keyboardVisible;
    }

    /**
     * Rasterizes a UI panel: a title line plus body lines. Returns ARGB pixels (width*height).
     * Called from native code.
     */
    public static int[] renderPanel(String title, String body, int width, int height) {
        Bitmap bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);
        c.drawColor(Color.rgb(18, 20, 28));
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        float unit = height / 18f;
        p.setColor(Color.rgb(64, 156, 255));
        c.drawRoundRect(new RectF(unit, unit, width - unit, unit * 1.25f), unit, unit, p);

        p.setColor(Color.WHITE);
        p.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        p.setTextSize(unit * 1.6f);
        c.drawText(title, unit * 1.5f, unit * 4f, p);

        p.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        p.setTextSize(unit * 0.8f);
        float y = unit * 6f;
        for (String line : body.split("\n")) {
            if (line.startsWith("#")) {
                p.setColor(Color.rgb(140, 190, 255));
                line = line.substring(1);
            } else {
                p.setColor(Color.rgb(215, 220, 230));
            }
            c.drawText(line, unit * 1.5f, y, p);
            y += unit * 1.2f;
        }
        int[] px = new int[width * height];
        bmp.getPixels(px, 0, width, 0, 0, width, height);
        bmp.recycle();
        return px;
    }

    /**
     * Rasterizes the toolbar: equally wide buttons. Bit i of activeMask highlights button i
     * (toggles/latched modifiers), hover is the index under the pointer (-1 = none).
     */
    public static int[] renderToolbar(String[] labels, int activeMask, int hover, int width, int height) {
        Bitmap bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);
        c.drawColor(Color.TRANSPARENT);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        float r = height * 0.28f;
        p.setColor(Color.argb(235, 22, 25, 34));
        c.drawRoundRect(new RectF(0, 0, width, height), r, r, p);

        int n = labels.length;
        float pad = height * 0.10f;
        float bw = (width - pad) / n;
        p.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        p.setTextAlign(Paint.Align.CENTER);
        for (int i = 0; i < n; i++) {
            RectF b = new RectF(pad + i * bw, pad, pad + (i + 1) * bw - pad, height - pad);
            boolean active = (activeMask & (1 << i)) != 0;
            int bg = active ? Color.rgb(48, 128, 240) : Color.rgb(44, 49, 64);
            if (i == hover) bg = active ? Color.rgb(90, 160, 255) : Color.rgb(74, 82, 104);
            p.setColor(bg);
            c.drawRoundRect(b, r * 0.7f, r * 0.7f, p);
            p.setColor(Color.WHITE);
            float ts = height * 0.30f;
            p.setTextSize(ts);
            while (p.measureText(labels[i]) > b.width() * 0.92f && ts > 8) {
                ts -= 1;
                p.setTextSize(ts);
            }
            c.drawText(labels[i], b.centerX(), b.centerY() + ts * 0.36f, p);
        }
        int[] px = new int[width * height];
        bmp.getPixels(px, 0, width, 0, 0, width, height);
        bmp.recycle();
        return px;
    }

    /**
     * Rasterizes a UI laid out by native code (which also does the hit testing) as a list of items.
     * kinds: 0 title, 1 section header, 2 label, 3 value, 4 button. rects: left, top, right, bottom
     * in pixels, 4 per item. flags: bit 0 = hovered, bit 1 = on/active. Returns ARGB pixels.
     */
    public static int[] renderItems(int width, int height, int[] kinds, float[] rects, String[] texts, int[] flags) {
        Bitmap bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);
        c.drawColor(Color.TRANSPARENT);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        float r = Math.min(width, height) * 0.03f;
        p.setColor(Color.rgb(18, 20, 28));
        c.drawRoundRect(new RectF(0, 0, width, height), r, r, p);
        Typeface bold = Typeface.create("sans-serif-medium", Typeface.BOLD);
        Typeface medium = Typeface.create("sans-serif-medium", Typeface.NORMAL);
        Typeface normal = Typeface.create("sans-serif", Typeface.NORMAL);
        for (int i = 0; i < kinds.length; i++) {
            RectF b = new RectF(rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]);
            boolean hover = (flags[i] & 1) != 0, on = (flags[i] & 2) != 0;
            float h = b.height();
            switch (kinds[i]) {
                case 0:
                    drawFit(c, p, texts[i], b, h * 0.62f, bold, Color.WHITE, false);
                    break;
                case 1:
                    drawFit(c, p, texts[i], b, h * 0.46f, medium, Color.rgb(140, 190, 255), false);
                    break;
                case 2:
                    drawFit(c, p, texts[i], b, h * 0.40f, normal, Color.rgb(215, 220, 230), false);
                    break;
                case 3:
                    drawFit(c, p, texts[i], b, h * 0.42f, medium, Color.WHITE, true);
                    break;
                default: {
                    int bg = on ? Color.rgb(48, 128, 240) : Color.rgb(44, 49, 64);
                    if (hover) bg = on ? Color.rgb(90, 160, 255) : Color.rgb(74, 82, 104);
                    p.setColor(bg);
                    c.drawRoundRect(b, h * 0.25f, h * 0.25f, p);
                    drawFit(c, p, texts[i], b, h * 0.42f, medium, Color.WHITE, true);
                }
            }
        }
        int[] px = new int[width * height];
        bmp.getPixels(px, 0, width, 0, 0, width, height);
        bmp.recycle();
        return px;
    }

    /** Draws one line of text vertically centered in b, shrinking it until it fits the width. */
    private static void drawFit(Canvas c, Paint p, String text, RectF b, float size, Typeface tf, int color,
                                boolean center) {
        p.setTypeface(tf);
        p.setColor(color);
        p.setTextAlign(center ? Paint.Align.CENTER : Paint.Align.LEFT);
        float ts = size;
        p.setTextSize(ts);
        while (p.measureText(text) > b.width() * 0.96f && ts > 8) {
            ts -= 1;
            p.setTextSize(ts);
        }
        c.drawText(text, center ? b.centerX() : b.left, b.centerY() + ts * 0.36f, p);
    }

    /** Invisible view that owns the IME connection and forwards everything to native code. */
    static class KeyboardSink extends View {
        KeyboardSink(Context ctx) {
            super(ctx);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo out) {
            out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                    | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
            out.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                    | EditorInfo.IME_ACTION_NONE;
            out.initialSelStart = 0;
            out.initialSelEnd = 0;
            return new SinkConnection(this);
        }

        @Override
        public boolean onKeyDown(int keyCode, KeyEvent event) {
            nativeOnKey(keyCode, true, event.getMetaState());
            return true;
        }

        @Override
        public boolean onKeyUp(int keyCode, KeyEvent event) {
            nativeOnKey(keyCode, false, event.getMetaState());
            return true;
        }
    }

    /**
     * Input connection with an always-empty editor. Composing text (word suggestions) is
     * mirrored to the PC by sending backspaces for removed characters and typing the new ones.
     */
    static class SinkConnection extends BaseInputConnection {
        private String composing = "";

        SinkConnection(View v) {
            super(v, false);
        }

        private static void tapKey(int code) {
            nativeOnKey(code, true, 0);
            nativeOnKey(code, false, 0);
        }

        private void replaceComposing(String next) {
            int common = 0;
            int max = Math.min(composing.length(), next.length());
            while (common < max && composing.charAt(common) == next.charAt(common)) common++;
            for (int i = composing.length(); i > common; i--) tapKey(KeyEvent.KEYCODE_DEL);
            if (next.length() > common) nativeOnText(next.substring(common));
            composing = next;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            replaceComposing(text.toString());
            composing = "";
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            replaceComposing(text.toString());
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing = "";
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            for (int i = 0; i < beforeLength; i++) tapKey(KeyEvent.KEYCODE_DEL);
            for (int i = 0; i < afterLength; i++) tapKey(KeyEvent.KEYCODE_FORWARD_DEL);
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) nativeOnKey(event.getKeyCode(), true, event.getMetaState());
            else if (event.getAction() == KeyEvent.ACTION_UP) nativeOnKey(event.getKeyCode(), false, event.getMetaState());
            return true;
        }

        @Override
        public boolean performEditorAction(int actionCode) {
            tapKey(KeyEvent.KEYCODE_ENTER);
            return true;
        }

        @Override
        public CharSequence getTextBeforeCursor(int n, int flags) {
            return composing;
        }

        @Override
        public CharSequence getTextAfterCursor(int n, int flags) {
            return "";
        }
    }
}
