/* The one full-screen view.  A repeating timer paces redraws; game logic
 * and rendering both happen inside onUpdate (one frame = one tick), and a
 * frame that overruns the timer period asks for the next one at once
 * instead of waiting for the next tick.  Between frames the CPU idles -
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
        WatchUi.requestUpdate();
    }

    function onUpdate(dc) {
        mEngine.render(dc);
        if (mEngine.wantMore) { WatchUi.requestUpdate(); }
    }
}

/* Touch + button input.  Venu X1 has two buttons and a touchscreen, so:
 *   top button (ENTER)          fire
 *   bottom button (BACK/ESC)    quit
 *   tap  left / right edge      turn a fixed step (hold for continuous)
 *   drag left / right           aim (proportional)
 *   tap  upper centre           forward
 *   tap  the gun (lower centre) fire
 *   tap  HUD strip (bottom)     open the door you are facing
 *   swipe up / down             forward / back
 *   any tap after death         restart                                  */
class DoomDelegate extends WatchUi.BehaviorDelegate {
    hidden var mEngine;
    hidden var mDragX = 0;

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
        if (d == WatchUi.SWIPE_UP)    { mEngine.action(ACT_FWD,  BURST); }
        if (d == WatchUi.SWIPE_DOWN)  { mEngine.action(ACT_BACK, BURST); }
        return true;
    }

    /* horizontal drag turns the view in proportion to the finger's travel */
    function onDrag(evt) {
        var c = evt.getCoordinates();
        if (evt.getType() == WatchUi.DRAG_TYPE_START) { mDragX = c[0]; return true; }
        var dx = c[0] - mDragX;
        if (dx > 2 || dx < -2) { mEngine.dragTurn(dx); mDragX = c[0]; }
        return true;
    }
}
