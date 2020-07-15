import java.util.ArrayList;

/**
 * Expect output:
 * 1299721
 * 2750161
 * 4256249
 * 5800139
 * 7368791
 * 8960467
 *
 * Status: Fail
 */
public class PrimeNumber {

    public static void main(String[] args) {
        ArrayList<Integer> res = new ArrayList<>();
        int last = 3;
        res.add(last);
        while (true) {
            last = last + 2;
            boolean prime = true;
            for (int v : res) {
                if (v * v > last) {
                    break;
                }
                if (last % v == 0) {
                    prime = false;
                    break;
                }
            }
            if (prime) {
                res.add(last);
                if (res.size() % 100000 == 0) {
                    System.out.println(last);
                }
                if (last > 9999999) {
                    break;
                }
            }
        }
    }
}