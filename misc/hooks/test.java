
public class test {
    public static void main(String[] args) {
        System.out.println("Java test");
        zsim.roi_begin();
        for (int i = 0; i < 42; i++) zsim.heartbeat();
        zsim.roi_end();
        System.out.println("Java test done");
    }
}

