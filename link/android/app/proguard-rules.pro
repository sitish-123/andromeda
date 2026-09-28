# JNA finds its native entry points and structures by reflection, and the UniFFI bindings are called through it.
-keep class com.sun.jna.** { *; }
-keep class * implements com.sun.jna.** { *; }
-keep class org.umbriel.link.ffi.** { *; }
-dontwarn java.awt.**
