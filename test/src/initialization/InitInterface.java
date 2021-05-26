package initialization;

/**
 * Interface Initialization Does Not Initialize Superinterfaces
 *
 * Status:
 */
public class InitInterface {
    private static interface I {
        int i = 1, ii = out("ii", 2);
    }
    private static interface J extends I {
        int j = out("j", 3), jj = out("jj", 4);
    }
    private static interface K extends J {
        int k = out("k", 5);
    }
    
    static int out(String s, int i) {
        System.out.println(s + "=" + i);
        return i;
    }

    public void test() {
        System.out.println(J.i);
        System.out.println(K.j);
    }
    
    public static void main(String[] args) {
        new InitInterface().test();
    }
    
}
