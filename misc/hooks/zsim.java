// package zsim;

public class zsim {
    public static native void roi_begin();
    public static native void roi_end();
    public static native void heartbeat();
    static {
        System.loadLibrary("zsim_jni");
    }
}

