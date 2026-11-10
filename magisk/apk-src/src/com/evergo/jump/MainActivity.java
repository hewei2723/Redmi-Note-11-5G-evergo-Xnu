package com.evergo.jump;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.InputStreamReader;

/**
 * One screen, one decision.
 *
 * Tapping the launcher icon opens a confirmation dialog. Confirming runs the
 * evergo-jump script as root, which copies the bundled firmware into reserved
 * DRAM and branches into it. The phone then leaves Android entirely, so this
 * activity does not expect to come back: if the jump works, the process dies
 * with the OS.
 *
 * Nothing here is destructive to storage. A failed attempt is recovered by
 * holding power for about fifteen seconds.
 */
public class MainActivity extends Activity {

    // Prefer the module's own copy so this works even if /system/bin is not on
    // the su PATH. Fall back to the mounted name.
    private static final String JUMP_CMD =
        "[ -x /data/adb/modules/evergo-jump/system/bin/evergo-jump ]" +
        " && /data/adb/modules/evergo-jump/system/bin/evergo-jump" +
        " || evergo-jump";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        new AlertDialog.Builder(this)
            .setTitle("Start UEFI firmware?")
            .setMessage(
                "This will hand the phone over to the evergo UEFI firmware.\n\n"
              + "• Android will stop until you power-cycle.\n"
              + "• No partition is modified; storage is untouched.\n"
              + "• If the screen stays black, hold power for ~15 seconds.\n\n"
              + "The firmware shows the UniXNU menu. XNU itself does not boot yet.")
            .setPositiveButton("Start", new android.content.DialogInterface.OnClickListener() {
                public void onClick(android.content.DialogInterface d, int w) { runJump(); }
            })
            .setNegativeButton("Cancel", new android.content.DialogInterface.OnClickListener() {
                public void onClick(android.content.DialogInterface d, int w) { finish(); }
            })
            .setCancelable(false)
            .show();
    }

    private void runJump() {
        new Thread(new Runnable() {
            public void run() {
                final String out = execRoot(JUMP_CMD);
                // If we get here, the jump did not happen (a real jump never
                // returns). Show whatever the script said so the user can act.
                new Handler(Looper.getMainLooper()).post(new Runnable() {
                    public void run() {
                        new AlertDialog.Builder(MainActivity.this)
                            .setTitle("Jump did not start")
                            .setMessage(out.isEmpty() ? "No output. Check that the module is installed and root works." : out)
                            .setPositiveButton("OK", new android.content.DialogInterface.OnClickListener() {
                                public void onClick(android.content.DialogInterface d, int w) { finish(); }
                            })
                            .show();
                    }
                });
            }
        }).start();
    }

    /** Run a command through su and return combined output. */
    private String execRoot(String cmd) {
        StringBuilder sb = new StringBuilder();
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
            BufferedReader e = new BufferedReader(new InputStreamReader(p.getErrorStream()));
            while ((line = e.readLine()) != null) sb.append(line).append('\n');
            p.waitFor();
        } catch (Exception ex) {
            sb.append("error: ").append(ex.getMessage());
        }
        return sb.toString().trim();
    }
}
