package com.britoshi.hyoshi;

import org.libsdl.app.SDLActivity;

// The activity for a game on Hyoshi Engine: SDL's, loading the game from libmain.so (the library
// hyoshi_add_app makes on Android). SDL is linked into it statically, so there's no libSDL3.so.
public class HyoshiActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
