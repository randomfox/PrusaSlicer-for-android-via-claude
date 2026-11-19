# Called from native code (libprusaslicer.so) by name.
-keep class org.prusaslicer.android.GuiNative { *; }
-keep class org.prusaslicer.android.NativeSlicer { *; }
-keep interface org.prusaslicer.android.GuiHost { *; }
-keep class * implements org.prusaslicer.android.GuiHost { *; }
# Methods called from JavaScript in web views.
-keepclassmembers class * {
    @android.webkit.JavascriptInterface <methods>;
}
