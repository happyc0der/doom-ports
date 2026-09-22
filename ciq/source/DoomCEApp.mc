/* DOOMCE for Connect IQ - application entry point.
 * Copyright (C) 2026 happyc0der.  GPL-3.0-or-later, see ../LICENSE. */
import Toybox.Application;
import Toybox.Lang;
import Toybox.WatchUi;

class DoomCEApp extends Application.AppBase {
    function initialize() {
        AppBase.initialize();
    }

    function getInitialView() {
        var engine = new Engine();
        return [ new DoomView(engine), new DoomDelegate(engine) ];
    }
}
