/* The one full-screen view.  A repeating timer requests a redraw at a
 * fixed cadence; game logic and rendering both happen inside onUpdate so
 * they stay 1:1 with frames actually shown.  Between frames the CPU idles -
 * this, not the renderer, is what keeps the battery cost down. */
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.Timer;
import Toybox.WatchUi;

const TICK_MS = 50;                    /* ~20 fps target; the engine measures */

class DoomView extends WatchUi.View {
    hidden var mEngine;
    hidden var mTimer;

    function initialize(engine) {
        View.initialize();
        mEngine = engine;
    }

    function onShow() {
        if (mTimer == null) {
            mTimer = new Timer.Timer();
            mTimer.start(method(:onTick), TICK_MS, true);
        }
    }

    function onHide() {
        if (mTimer != null) {
            mTimer.stop();
            mTimer = null;
        }
    }

    function onTick() as Void {
        mEngine.tick();
        WatchUi.requestUpdate();
    }

    function onUpdate(dc) {
        mEngine.render(dc);
    }
}

/* Touch + button input.  Venu X1 has two buttons and a touchscreen, so:
 *   top button (ENTER)          fire
 *   bottom button (BACK/ESC)    quit
 *   tap  left / right edge      turn   (burst; hold for continuous)
 *   tap  upper centre           forward
 *   tap  the gun (lower centre) fire
 *   tap  HUD strip (bottom)     open the door you are facing
 *   swipe left / right          strafe
 *   swipe up / down             forward / back
 *   any tap after death         restart                                  */
class DoomDelegate extends WatchUi.BehaviorDelegate {
    hidden var mEngine;

    function initialize(engine) {
        BehaviorDelegate.initialize();
        mEngine = engine;
    }

    hidden function zoneOf(x, y) {
        if (y >= VIEW_H) { return ACT_USE; }
        if (x < 140)     { return ACT_LEFT; }
        if (x >= 308)    { return ACT_RIGHT; }
        if (y < 220)     { return ACT_FWD; }
        return ACT_FIRE;
    }

    function onKey(evt) {
        var k = evt.getKey();
        if (k == WatchUi.KEY_ENTER || k == WatchUi.KEY_START) {
            mEngine.action(ACT_FIRE, 0);
            return true;
        }
        if (k == WatchUi.KEY_UP)   { mEngine.action(ACT_FWD,  BURST); return true; }
        if (k == WatchUi.KEY_DOWN) { mEngine.action(ACT_BACK, BURST); return true; }
        return false;                            /* BACK/ESC: let the system exit */
    }

    function onTap(evt) {
        var c = evt.getCoordinates();
        mEngine.action(zoneOf(c[0], c[1]), BURST);
        return true;
    }

    function onHold(evt) {
        var c = evt.getCoordinates();
        mEngine.hold(zoneOf(c[0], c[1]));
        return true;
    }

    function onRelease(evt) {
        mEngine.release();
        return true;
    }

    function onSwipe(evt) {
        var d = evt.getDirection();
        if (d == WatchUi.SWIPE_LEFT)  { mEngine.action(ACT_SLEFT,  BURST); }
        if (d == WatchUi.SWIPE_RIGHT) { mEngine.action(ACT_SRIGHT, BURST); }
        if (d == WatchUi.SWIPE_UP)    { mEngine.action(ACT_FWD,    BURST); }
        if (d == WatchUi.SWIPE_DOWN)  { mEngine.action(ACT_BACK,   BURST); }
        return true;
    }
}
